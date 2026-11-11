/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku side of LinuxKPI Bluetooth: each Linux hci_dev becomes an HCI
 * transport of Haiku's Bluetooth stack, exactly as h2generic's devices are:
 *
 *   /dev/bluetooth/h2/<driver>/<n>   found by bluetooth_server, which
 *                                    watches the subdirectories of
 *                                    /dev/bluetooth/h2 [V: BluetoothServer,
 *                                    DeviceManager::StartMonitoringDevice]
 *   open()     RegisterDriver() with Haiku's HCI module ("bluetooth/hci/v1")
 *   ioctl()    GET_HCI_ID, BT_UP, ISSUE_BT_COMMAND, GET_STATS
 *              (<bluetooth/HCI/btHCI_transport.h>)
 *   RX         PostTransportPacket(hci_id, BT_EVENT/BT_ACL, data, len)
 *   TX         the module calls our SendCommand/SendACL hooks
 *
 * Behaviour copied from h2generic where Haiku's stack relies on it [V]:
 *  - the command net_buffer stays the caller's; ACL buffers are ours to
 *    free (PostACL ignores the hook's result and never frees them);
 *  - one open at a time per node.
 * Unlike h2generic, a command issued before BT_UP brings the device up
 * first (the Linux driver must have run open() and setup()).
 *
 * Built WITHOUT the LinuxKPI preinclude (Haiku side).
 */
#include <KernelExport.h>
#include <Drivers.h>
#include <lock.h>
#include <kernel.h>		/* IS_USER_ADDRESS */
#include <net_buffer.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_transport.h>
#include <util/AutoLock.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lkpi_bt_bridge.h"
#include "lkpi_driver.h"

extern "C" const char *gDriverName;	/* freebsd_network haiku-module.h */

/* one HCI command: 3 header bytes + up to 255 parameter bytes */
#define	LKPI_BT_MAX_COMMAND	(3 + 255)

struct lkpi_bt_node {
	lkpi_bt_node	*next;
	char		path[B_PATH_NAME_LENGTH];
	void		*ctx;		/* hci_dev; NULL once destroyed */
	int32		refs;		/* list/creator + open handle */
	int32		users;		/* calls into ctx in progress */
	bool		opened;
	bool		up;
	hci_id		hid;		/* Haiku HCI index, -1 if none */
	mutex		lock;		/* opened, up, hid; serializes ioctls */
};

static mutex sLock = MUTEX_INITIALIZER("lkpi bt nodes");
static lkpi_bt_node *sNodes;
static bt_hci_module_info *sHci;
static net_buffer_module_info *sBuf;
static int32 sModuleUsers;

static status_t lkpi_bt_open(const char *, uint32, void **);
static status_t lkpi_bt_close(void *);
static status_t lkpi_bt_free(void *);
static status_t lkpi_bt_control(void *, uint32, void *, size_t);
static status_t lkpi_bt_read(void *, off_t, void *, size_t *);
static status_t lkpi_bt_write(void *, off_t, const void *, size_t *);
static status_t lkpi_bt_send_command(hci_id, net_buffer *);
static status_t lkpi_bt_send_acl(hci_id, net_buffer *);
static status_t lkpi_bt_send_sco(hci_id, net_buffer *);

static device_hooks sDeviceHooks = {
	lkpi_bt_open,
	lkpi_bt_close,
	lkpi_bt_free,
	lkpi_bt_control,
	lkpi_bt_read,
	lkpi_bt_write,
	NULL,
	NULL,
	NULL,
	NULL
};

static bt_hci_transport_hooks sTransportHooks = {
	lkpi_bt_send_command,
	lkpi_bt_send_acl,
	lkpi_bt_send_sco,
	NULL,		/* SendESCO */
	NULL,		/* DeliverStatistics */
	H2
};


static status_t
lkpi_bt_status(int r)
{
	switch (r) {
	case LKPI_BT_OK:	return B_OK;
	case LKPI_BT_NODEV:	return ENODEV;
	case LKPI_BT_NOMEM:	return B_NO_MEMORY;
	case LKPI_BT_INVAL:	return B_BAD_VALUE;
	case LKPI_BT_UNSUPP:	return B_NOT_SUPPORTED;
	case LKPI_BT_TIMEOUT:	return B_TIMED_OUT;
	}
	return B_IO_ERROR;
}

static void
lkpi_bt_put(lkpi_bt_node *node)
{
	if (atomic_add(&node->refs, -1) == 1) {
		mutex_destroy(&node->lock);
		delete node;
	}
}

/* Enter a call into the LinuxKPI side; false if the hci_dev is gone. */
static bool
lkpi_bt_enter(lkpi_bt_node *node, void **ctx)
{
	MutexLocker _(sLock);
	if (node->ctx == NULL)
		return false;
	node->users++;
	*ctx = node->ctx;
	return true;
}

static void
lkpi_bt_leave(lkpi_bt_node *node)
{
	MutexLocker _(sLock);
	node->users--;
}

static lkpi_bt_node *
lkpi_bt_find_hid(hci_id hid)
{
	for (lkpi_bt_node *n = sNodes; n != NULL; n = n->next) {
		if (n->hid == hid)
			return n;
	}
	return NULL;
}

/* ---- modules ----------------------------------------------------------- */

static status_t
lkpi_bt_get_modules(void)
{
	MutexLocker _(sLock);
	if (sModuleUsers++ > 0)
		return B_OK;
	status_t s = get_module(NET_BUFFER_MODULE_NAME, (module_info **)&sBuf);
	if (s != B_OK) {
		sModuleUsers--;
		return s;
	}
	s = get_module(BT_HCI_MODULE_NAME, (module_info **)&sHci);
	if (s != B_OK) {
		put_module(NET_BUFFER_MODULE_NAME);
		sBuf = NULL;
		sModuleUsers--;
		return s;
	}
	return B_OK;
}

static void
lkpi_bt_put_modules(void)
{
	MutexLocker _(sLock);
	if (--sModuleUsers > 0)
		return;
	put_module(BT_HCI_MODULE_NAME);
	put_module(NET_BUFFER_MODULE_NAME);
	sHci = NULL;
	sBuf = NULL;
}

/* ---- Haiku HCI transport hooks -------------------------------------------- */

static status_t
lkpi_bt_tx(hci_id hid, net_buffer *nbuf, int type)
{
	lkpi_bt_node *node;
	void *ctx = NULL;

	{
		MutexLocker _(sLock);
		node = lkpi_bt_find_hid(hid);
		if (node == NULL || node->ctx == NULL || !node->up)
			return B_DEV_NOT_READY;
		node->users++;
		ctx = node->ctx;
	}

	status_t s;
	size_t len = nbuf->size;
	uint8 *data = (uint8 *)malloc(len > 0 ? len : 1);
	if (data == NULL)
		s = B_NO_MEMORY;
	else if ((s = sBuf->read(nbuf, 0, data, len)) == B_OK)
		s = lkpi_bt_status(lkpi_hci_send(ctx, type, data, len));
	free(data);
	lkpi_bt_leave(node);
	return s;
}

static status_t
lkpi_bt_send_command(hci_id hid, net_buffer *nbuf)
{
	/* the buffer stays the caller's (as with h2generic) */
	return lkpi_bt_tx(hid, nbuf, LKPI_BT_CMD);
}

static status_t
lkpi_bt_send_acl(hci_id hid, net_buffer *nbuf)
{
	/* ours: PostACL() neither frees it nor looks at our result */
	status_t s = lkpi_bt_tx(hid, nbuf, LKPI_BT_ACL);
	sBuf->free(nbuf);
	return s;
}

static status_t
lkpi_bt_send_sco(hci_id hid, net_buffer *nbuf)
{
	/* no isochronous USB transfers yet (see LinuxKPI USB) */
	sBuf->free(nbuf);
	return B_NOT_SUPPORTED;
}

/* ---- device hooks -------------------------------------------------------- */

static status_t
lkpi_bt_open(const char *name, uint32 flags, void **cookie)
{
	lkpi_bt_node *node = NULL;

	{
		MutexLocker _(sLock);
		for (lkpi_bt_node *n = sNodes; n != NULL; n = n->next) {
			if (strcmp(n->path, name) == 0 && n->ctx != NULL) {
				node = n;
				atomic_add(&node->refs, 1);
				break;
			}
		}
	}
	if (node == NULL)
		return ENODEV;

	MutexLocker locker(node->lock);
	if (node->opened) {
		locker.Unlock();
		lkpi_bt_put(node);
		return B_BUSY;
	}
	status_t s = lkpi_bt_get_modules();
	if (s != B_OK) {
		dprintf("%s: %s: Bluetooth stack modules unavailable: %s\n",
			gDriverName, name, strerror(s));
		locker.Unlock();
		lkpi_bt_put(node);
		return s;
	}
	bluetooth_device *bdev = NULL;
	s = sHci->RegisterDriver(&sTransportHooks, &bdev);
	if (s != B_OK || bdev == NULL) {
		lkpi_bt_put_modules();
		locker.Unlock();
		lkpi_bt_put(node);
		return s != B_OK ? s : B_ERROR;
	}
	{
		MutexLocker _(sLock);
		node->hid = bdev->index;
		node->opened = true;
	}
	*cookie = node;
	return B_OK;
}

static status_t
lkpi_bt_close(void *cookie)
{
	lkpi_bt_node *node = (lkpi_bt_node *)cookie;
	MutexLocker locker(node->lock);
	void *ctx;
	hci_id hid;

	if (node->up && lkpi_bt_enter(node, &ctx)) {
		lkpi_hci_down(ctx);
		lkpi_bt_leave(node);
	}
	{
		MutexLocker _(sLock);
		node->up = false;
		node->opened = false;
		hid = node->hid;
		node->hid = -1;
		/* lkpi_bt_node_rx() posts outside the lock: let it finish */
		while (node->users > 0) {
			_.Unlock();
			snooze(1000);
			_.Lock();
		}
	}
	sHci->UnregisterDriver(hid);
	lkpi_bt_put_modules();
	return B_OK;
}

static status_t
lkpi_bt_free(void *cookie)
{
	lkpi_bt_put((lkpi_bt_node *)cookie);
	return B_OK;
}

/* node->lock held */
static status_t
lkpi_bt_up(lkpi_bt_node *node)
{
	void *ctx;

	if (node->up)
		return B_OK;
	if (!lkpi_bt_enter(node, &ctx))
		return ENODEV;
	int r = lkpi_hci_up(ctx);
	lkpi_bt_leave(node);
	if (r != LKPI_BT_OK)
		return lkpi_bt_status(r);
	MutexLocker _(sLock);
	node->up = true;
	return B_OK;
}

static status_t
lkpi_bt_control(void *cookie, uint32 op, void *buffer, size_t length)
{
	lkpi_bt_node *node = (lkpi_bt_node *)cookie;
	MutexLocker locker(node->lock);
	status_t s;
	void *ctx;

	if (buffer == NULL || !IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;

	switch (op) {
	case GET_HCI_ID:
		return user_memcpy(buffer, &node->hid, sizeof(hci_id));

	case BT_UP:
		return lkpi_bt_up(node);

	case ISSUE_BT_COMMAND:
	{
		uint8 cmd[LKPI_BT_MAX_COMMAND];

		if (length < 3 || length > sizeof(cmd))
			return B_BAD_VALUE;
		if (user_memcpy(cmd, buffer, length) != B_OK)
			return B_BAD_ADDRESS;
		if ((size_t)cmd[2] + 3 != length)
			return B_BAD_VALUE;	/* plen disagrees with length */
		s = lkpi_bt_up(node);
		if (s != B_OK)
			return s;
		if (!lkpi_bt_enter(node, &ctx))
			return ENODEV;
		s = lkpi_bt_status(lkpi_hci_send(ctx, LKPI_BT_CMD, cmd,
			length));
		lkpi_bt_leave(node);
		return s;
	}

	case GET_STATS:
	{
		struct lkpi_bt_stats st;
		bt_hci_statistics hs;

		memset(&st, 0, sizeof(st));
		if (lkpi_bt_enter(node, &ctx)) {
			lkpi_hci_stats(ctx, &st);
			lkpi_bt_leave(node);
		}
		/* Haiku's counters are 8 bit (bt_stat_t); they wrap there too */
		memset(&hs, 0, sizeof(hs));
		hs.acceptedTX = hs.successfulTX
			= (bt_stat_t)(st.cmd_tx + st.acl_tx + st.sco_tx);
		hs.errorTX = (bt_stat_t)st.err_tx;
		hs.acceptedRX = hs.successfulRX
			= (bt_stat_t)(st.evt_rx + st.acl_rx + st.sco_rx);
		hs.errorRX = (bt_stat_t)st.err_rx;
		hs.commandTX = (bt_stat_t)st.cmd_tx;
		hs.eventRX = (bt_stat_t)st.evt_rx;
		hs.aclTX = (bt_stat_t)st.acl_tx;
		hs.aclRX = (bt_stat_t)st.acl_rx;
		hs.scoTX = (bt_stat_t)st.sco_tx;
		hs.scoRX = (bt_stat_t)st.sco_rx;
		hs.bytesRX = (bt_stat_t)st.byte_rx;
		hs.bytesTX = (bt_stat_t)st.byte_tx;
		return user_memcpy(buffer, &hs, min_c(length, sizeof(hs)));
	}
	}
	return B_DEV_INVALID_IOCTL;
}

static status_t
lkpi_bt_read(void *cookie, off_t pos, void *buffer, size_t *length)
{
	*length = 0;		/* data flows through the HCI module */
	return B_OK;
}

static status_t
lkpi_bt_write(void *cookie, off_t pos, const void *buffer, size_t *length)
{
	*length = 0;
	return B_NOT_SUPPORTED;
}

/* ---- LinuxKPI side ------------------------------------------------------- */

extern "C" int
lkpi_bt_node_create(void *ctx, const struct lkpi_bt_node_info *info,
	struct lkpi_bt_node **_node)
{
	lkpi_bt_node *node = new(std::nothrow) lkpi_bt_node;
	if (node == NULL)
		return LKPI_BT_NOMEM;
	node->next = NULL;
	snprintf(node->path, sizeof(node->path), "bluetooth/h2/%s/%d",
		info->driver, info->index);
	node->ctx = ctx;
	node->refs = 1;
	node->users = 0;
	node->opened = false;
	node->up = false;
	node->hid = -1;
	mutex_init(&node->lock, "lkpi bt node");

	{
		MutexLocker _(sLock);
		node->next = sNodes;
		sNodes = node;
	}
	status_t s = lkpi_devfs_add(node->path, &sDeviceHooks);
	if (s != B_OK) {
		MutexLocker _(sLock);
		for (lkpi_bt_node **pp = &sNodes; *pp != NULL; pp = &(*pp)->next) {
			if (*pp == node) {
				*pp = node->next;
				break;
			}
		}
		_.Unlock();
		lkpi_bt_put(node);
		return s == B_NO_MEMORY ? LKPI_BT_NOMEM : LKPI_BT_INVAL;
	}
	dprintf("%s: %s%s%s (%04x:%04x)\n", gDriverName, node->path,
		info->desc != NULL ? ": " : "", info->desc != NULL ? info->desc : "",
		info->vendor, info->product);
	*_node = node;
	return LKPI_BT_OK;
}

extern "C" void
lkpi_bt_node_destroy(struct lkpi_bt_node *node)
{
	if (node == NULL)
		return;
	{
		MutexLocker _(sLock);
		node->ctx = NULL;		/* no new calls into the hci_dev */
		while (node->users > 0) {	/* wait for running ones */
			_.Unlock();
			snooze(1000);
			_.Lock();
		}
		for (lkpi_bt_node **pp = &sNodes; *pp != NULL; pp = &(*pp)->next) {
			if (*pp == node) {
				*pp = node->next;
				break;
			}
		}
		node->up = false;
	}
	/* an open handle keeps the node (refs) until free(); its calls now
	 * fail with ENODEV, and bluetooth_server closes it */
	lkpi_devfs_remove(node->path);
	lkpi_bt_put(node);
}

extern "C" int
lkpi_bt_node_rx(struct lkpi_bt_node *node, int type, const void *data,
	size_t len)
{
	bt_packet_t t;
	hci_id hid;

	switch (type) {
	case LKPI_BT_EVT:	t = BT_EVENT; break;
	case LKPI_BT_ACL:	t = BT_ACL; break;
	default:		return LKPI_BT_UNSUPP;
	}
	{
		MutexLocker _(sLock);
		if (!node->opened || !node->up || node->hid < 0)
			return LKPI_BT_NODEV;
		hid = node->hid;
		node->users++;		/* close() waits before unregistering */
	}
	status_t s = sHci->PostTransportPacket(hid, t, (void *)data, len);
	lkpi_bt_leave(node);
	return s == B_OK ? LKPI_BT_OK : LKPI_BT_IO;
}
