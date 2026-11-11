/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku side of LinuxKPI USB: Haiku's public USB bus manager
 * (bus_managers/usb/v3, <USB3.h>), which has been the stable driver API
 * since R1/beta1, behind the plain-C bridge in lkpi_usb_bridge.h.
 *
 * Built WITHOUT the LinuxKPI preinclude: Haiku types and errors only.
 *
 * Facts this relies on (Haiku source at patches/haiku/BASE_COMMIT):
 *  - register_driver(name, descriptors, n, NULL) copies the descriptors;
 *    a device matches if vendor/product match (0 = any) and the class
 *    triple matches the device or any interface alt setting. Calling it
 *    again with the same name replaces the list. On hotplug of a matching
 *    device the stack calls devfs_rescan_driver(name), which loads the
 *    add-on if it is not loaded (Stack::NotifyDeviceChange) [V].
 *  - install_notify() reports present devices synchronously, under the
 *    stack's explore lock; device_added returning an error keeps no
 *    cookie, and other drivers are notified independently
 *    (Device::ReportDevice) [V].
 *  - callbacks run in the stack's finisher thread and may queue new
 *    transfers (h2generic and usb_disk do) [V].
 *  - cancel_queued_transfers() cancels every transfer of a pipe; there is
 *    no per-transfer cancel [V]; completions report B_CANCELED.
 *  - cancelled transfers may complete in the cancelling thread [H, review:
 *    host controller code not in the checked tree]; LinuxKPI USB defers
 *    cancels that can come from atomic context.
 *  - reset_port() only resets the port (Hub::ResetPort) and nothing
 *    re-enumerates a device that stays connected [V]; so it is not used
 *    (usb_reset_device() fails, see lkpi_usb.c).
 *  - deleting a pipe (set_alt_interface) drops its transfers without
 *    calling their callbacks (~Pipe: CancelQueuedTransfers(true)) [H];
 *    LinuxKPI USB kills its URBs on the old alternate first.
 *  - the active alternate of an interface is usb_interface_list::active;
 *    its endpoints' pipes are valid after set_alt_interface() [V].
 */
#include <KernelExport.h>
#include <USB3.h>
#include <lock.h>
#include <stdlib.h>
#include <string.h>

#include "lkpi_usb_bridge.h"
#include "lkpi_driver.h"		/* lkpi_devfs_notify_enter/exit */

struct lkpi_hk_dev {
	usb_device			dev;
	const usb_configuration_info	*config;	/* active, or first */
	void				*ctx;		/* LinuxKPI side */
	struct lkpi_hk_dev		*next;
};

static usb_module_info *sUSB;
static const char *sDriverName;
static mutex sDevLock = MUTEX_INITIALIZER("lkpi usb devices");
static struct lkpi_hk_dev *sDevs;

static int
lkpi_hk_status(status_t s)
{
	switch (s) {
	case B_OK:			return (LKPI_HK_OK);
	case B_CANCELED:		return (LKPI_HK_CANCELED);
	case B_DEV_STALLED:		return (LKPI_HK_STALL);
	case B_DEV_CRC_ERROR:
	case B_DEV_BAD_PID:
	case B_DEV_UNEXPECTED_PID:	return (LKPI_HK_CRC);
	case B_DEV_DATA_OVERRUN:
	case B_DEV_FIFO_OVERRUN:	return (LKPI_HK_OVERRUN);
	case B_DEV_DATA_UNDERRUN:
	case B_DEV_FIFO_UNDERRUN:	return (LKPI_HK_SHORT);
	case B_DEV_TIMEOUT:
	case B_TIMED_OUT:		return (LKPI_HK_TIMEOUT);
	case B_DEV_NOT_READY:
	case B_DEV_NO_MEDIA:
	case B_ENTRY_NOT_FOUND:		return (LKPI_HK_GONE);
	case B_NO_MEMORY:
	case B_DEV_NO_MEMORY:		return (LKPI_HK_NOMEM);
	case B_BAD_VALUE:		return (LKPI_HK_INVAL);
	case B_NOT_SUPPORTED:		return (LKPI_HK_UNSUPP);
	}
	return (LKPI_HK_ERROR);
}

/* ---- device notifications -------------------------------------------- */

static status_t
lkpi_hk_device_added(usb_device dev, void **cookie)
{
	struct lkpi_hk_dev *hk;
	int r;

	hk = calloc(1, sizeof(*hk));
	if (hk == NULL)
		return (B_NO_MEMORY);
	hk->dev = dev;
	hk->config = sUSB->get_configuration(dev);
	if (hk->config == NULL)
		hk->config = sUSB->get_nth_configuration(dev, 0);
	if (hk->config == NULL) {
		free(hk);
		return (B_ERROR);
	}

	/* probe() may publish device nodes; the stack rescans us after */
	lkpi_devfs_notify_enter();
	r = lkpi_usb_device_added(hk, &hk->ctx);
	lkpi_devfs_notify_exit();
	if (r != LKPI_HK_OK) {
		free(hk);
		return (B_ERROR);	/* not ours: Haiku keeps no cookie */
	}
	mutex_lock(&sDevLock);
	hk->next = sDevs;
	sDevs = hk;
	mutex_unlock(&sDevLock);
	*cookie = hk;
	return (B_OK);
}

static status_t
lkpi_hk_device_removed(void *cookie)
{
	struct lkpi_hk_dev *hk = cookie, **pp;

	lkpi_devfs_notify_enter();
	lkpi_usb_device_removed(hk->ctx);
	lkpi_devfs_notify_exit();
	mutex_lock(&sDevLock);
	for (pp = &sDevs; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == hk) {
			*pp = hk->next;
			break;
		}
	}
	mutex_unlock(&sDevLock);
	free(hk);
	return (B_OK);
}

static usb_notify_hooks sHooks = {
	lkpi_hk_device_added,
	lkpi_hk_device_removed
};

static status_t
lkpi_hk_register(const struct lkpi_hk_match *m, size_t count)
{
	usb_support_descriptor *d;
	status_t s;
	size_t i;

	if (count == 0)		/* an empty list would mean "every device" */
		return (B_BAD_VALUE);
	d = calloc(count, sizeof(*d));
	if (d == NULL)
		return (B_NO_MEMORY);
	for (i = 0; i < count; i++) {
		d[i].dev_class = m[i].dev_class;
		d[i].dev_subclass = m[i].dev_subclass;
		d[i].dev_protocol = m[i].dev_protocol;
		d[i].vendor = m[i].vendor;
		d[i].product = m[i].product;
	}
	s = sUSB->register_driver(sDriverName, d, count, NULL);	/* copies */
	free(d);
	return (s);
}

int
lkpi_hk_usb_start(const char *driver_name, const struct lkpi_hk_match *m,
    size_t count)
{
	status_t s;

	if (sUSB != NULL)
		return (LKPI_HK_OK);
	s = get_module(B_USB_MODULE_NAME, (module_info **)&sUSB);
	if (s != B_OK) {
		dprintf("%s: LinuxKPI USB: no USB bus manager (%s)\n",
		    driver_name, strerror(s));
		sUSB = NULL;
		return (lkpi_hk_status(s));
	}
	sDriverName = driver_name;
	s = lkpi_hk_register(m, count);
	if (s == B_OK)
		s = sUSB->install_notify(driver_name, &sHooks);
	if (s != B_OK) {
		put_module(B_USB_MODULE_NAME);
		sUSB = NULL;
	}
	return (lkpi_hk_status(s));
}

void
lkpi_hk_usb_stop(void)
{
	if (sUSB == NULL)
		return;
	/* calls device_removed for every device we kept a cookie for */
	sUSB->uninstall_notify(sDriverName);
	put_module(B_USB_MODULE_NAME);
	sUSB = NULL;
}

/* ---- descriptors -------------------------------------------------------- */

const uint8_t *
lkpi_hk_device_descriptor(struct lkpi_hk_dev *hk)
{
	return ((const uint8_t *)sUSB->get_device_descriptor(hk->dev));
}

uint32_t
lkpi_hk_device_id(struct lkpi_hk_dev *hk)
{
	return ((uint32_t)hk->dev);
}

int
lkpi_hk_config_value(struct lkpi_hk_dev *hk)
{
	return (hk->config->descr->configuration_value);
}

int
lkpi_hk_num_interfaces(struct lkpi_hk_dev *hk)
{
	return ((int)hk->config->interface_count);
}

static const usb_interface_list *
lkpi_hk_iflist(struct lkpi_hk_dev *hk, int ifidx)
{
	if (ifidx < 0 || (size_t)ifidx >= hk->config->interface_count)
		return (NULL);
	return (&hk->config->interface[ifidx]);
}

static const usb_interface_info *
lkpi_hk_alt(struct lkpi_hk_dev *hk, int ifidx, int alt)
{
	const usb_interface_list *il = lkpi_hk_iflist(hk, ifidx);

	if (il == NULL || alt < 0 || (size_t)alt >= il->alt_count)
		return (NULL);
	return (&il->alt[alt]);
}

int
lkpi_hk_num_alts(struct lkpi_hk_dev *hk, int ifidx)
{
	const usb_interface_list *il = lkpi_hk_iflist(hk, ifidx);

	return (il != NULL ? (int)il->alt_count : 0);
}

int
lkpi_hk_active_alt(struct lkpi_hk_dev *hk, int ifidx)
{
	const usb_interface_list *il = lkpi_hk_iflist(hk, ifidx);

	if (il == NULL || il->active == NULL)
		return (0);
	return ((int)(il->active - il->alt));
}

const uint8_t *
lkpi_hk_alt_descriptor(struct lkpi_hk_dev *hk, int ifidx, int alt)
{
	const usb_interface_info *ai = lkpi_hk_alt(hk, ifidx, alt);

	return (ai != NULL ? (const uint8_t *)ai->descr : NULL);
}

int
lkpi_hk_alt_num_endpoints(struct lkpi_hk_dev *hk, int ifidx, int alt)
{
	const usb_interface_info *ai = lkpi_hk_alt(hk, ifidx, alt);

	return (ai != NULL ? (int)ai->endpoint_count : 0);
}

const uint8_t *
lkpi_hk_ep_descriptor(struct lkpi_hk_dev *hk, int ifidx, int alt, int ep)
{
	const usb_interface_info *ai = lkpi_hk_alt(hk, ifidx, alt);

	if (ai == NULL || ep < 0 || (size_t)ep >= ai->endpoint_count)
		return (NULL);
	return ((const uint8_t *)ai->endpoint[ep].descr);
}

size_t
lkpi_hk_alt_extra(struct lkpi_hk_dev *hk, int ifidx, int alt, uint8_t *buf,
    size_t size)
{
	const usb_interface_info *ai = lkpi_hk_alt(hk, ifidx, alt);
	size_t i, n = 0;

	if (ai == NULL)
		return (0);
	for (i = 0; i < ai->generic_count; i++) {
		const usb_generic_descriptor *g = &ai->generic[i]->generic;
		size_t len = g->length;

		if (len < 2)
			continue;
		if (buf != NULL && n + len <= size)
			memcpy(buf + n, g, len);
		n += len;
	}
	return (n);
}

/* ---- configuration ------------------------------------------------------ */

int
lkpi_hk_activate(struct lkpi_hk_dev *hk)
{
	const usb_configuration_info *cur = sUSB->get_configuration(hk->dev);
	status_t s;

	if (cur != NULL) {
		hk->config = cur;
		return (LKPI_HK_OK);
	}
	s = sUSB->set_configuration(hk->dev, hk->config);
	if (s == B_OK)
		hk->config = sUSB->get_configuration(hk->dev);
	return (lkpi_hk_status(s));
}

int
lkpi_hk_set_alt(struct lkpi_hk_dev *hk, int ifidx, int alt)
{
	const usb_interface_info *ai = lkpi_hk_alt(hk, ifidx, alt);

	if (ai == NULL)
		return (LKPI_HK_INVAL);
	return (lkpi_hk_status(sUSB->set_alt_interface(hk->dev, ai)));
}

void *
lkpi_hk_pipe(struct lkpi_hk_dev *hk, uint8_t epaddr)
{
	size_t i, e;

	for (i = 0; i < hk->config->interface_count; i++) {
		const usb_interface_info *ai = hk->config->interface[i].active;

		if (ai == NULL)
			continue;
		for (e = 0; e < ai->endpoint_count; e++) {
			if (ai->endpoint[e].descr->endpoint_address == epaddr)
				return ((void *)(addr_t)ai->endpoint[e].handle);
		}
	}
	return (NULL);
}

/* ---- transfers ---------------------------------------------------------- */

struct lkpi_hk_req {
	lkpi_hk_done_fn	done;
	void		*cookie;
};

/* Haiku callback -> bridge callback. One small record per transfer, freed
 * before the bridge callback runs (which may queue the next transfer). */
static void
lkpi_hk_callback(void *cookie, status_t status, void *data, size_t actual)
{
	struct lkpi_hk_req *r = cookie;
	lkpi_hk_done_fn done = r->done;
	void *c = r->cookie;

	(void)data;
	free(r);
	done(c, lkpi_hk_status(status), actual);
}

int
lkpi_hk_queue(struct lkpi_hk_dev *hk, void *pipe, int type, void *buf,
    size_t len, lkpi_hk_done_fn done, void *cookie)
{
	struct lkpi_hk_req *r;
	status_t s;

	(void)hk;
	if (pipe == NULL)
		return (LKPI_HK_INVAL);
	r = malloc(sizeof(*r));
	if (r == NULL)
		return (LKPI_HK_NOMEM);
	r->done = done;
	r->cookie = cookie;
	switch (type) {
	case LKPI_HK_BULK:
		s = sUSB->queue_bulk((usb_pipe)(addr_t)pipe, buf, len,
		    lkpi_hk_callback, r);
		break;
	case LKPI_HK_INTR:
		s = sUSB->queue_interrupt((usb_pipe)(addr_t)pipe, buf, len,
		    lkpi_hk_callback, r);
		break;
	default:
		/* isochronous: not yet (docs/BLUETOOTH.md, SCO voice) */
		s = B_NOT_SUPPORTED;
		break;
	}
	if (s != B_OK)
		free(r);
	return (lkpi_hk_status(s));
}

int
lkpi_hk_queue_control(struct lkpi_hk_dev *hk, const uint8_t setup[8],
    void *buf, lkpi_hk_done_fn done, void *cookie)
{
	struct lkpi_hk_req *r;
	status_t s;
	uint16 value = setup[2] | (setup[3] << 8);
	uint16 index = setup[4] | (setup[5] << 8);
	uint16 length = setup[6] | (setup[7] << 8);

	r = malloc(sizeof(*r));
	if (r == NULL)
		return (LKPI_HK_NOMEM);
	r->done = done;
	r->cookie = cookie;
	s = sUSB->queue_request(hk->dev, setup[0], setup[1], value, index,
	    length, buf, lkpi_hk_callback, r);
	if (s != B_OK)
		free(r);
	return (lkpi_hk_status(s));
}

int
lkpi_hk_cancel(struct lkpi_hk_dev *hk, void *pipe)
{
	(void)hk;
	if (pipe == NULL)
		return (LKPI_HK_INVAL);
	return (lkpi_hk_status(sUSB->cancel_queued_transfers(
	    (usb_pipe)(addr_t)pipe)));
}

int
lkpi_hk_cancel_control(struct lkpi_hk_dev *hk)
{
	return (lkpi_hk_status(sUSB->cancel_queued_requests(hk->dev)));
}

int
lkpi_hk_clear_halt(struct lkpi_hk_dev *hk, void *pipe)
{
	(void)hk;
	if (pipe == NULL)
		return (LKPI_HK_INVAL);
	return (lkpi_hk_status(sUSB->clear_feature((usb_id)(addr_t)pipe,
	    USB_FEATURE_ENDPOINT_HALT)));
}
