/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI side of LinuxKPI Bluetooth: the parts of Linux's HCI core a
 * transport driver calls (net/bluetooth/hci_core.h), on top of Haiku's
 * Bluetooth stack through lkpi_bt_bridge.h. Written for LinuxKPI_HaikuOS;
 * no Linux code. Model: see hci_core.h.
 *
 * Semantics kept from Linux where drivers observe them:
 *  - hci_recv_frame() accepts frames only while the device is up or
 *    initialising (HCI_UP / HCI_INIT), else -ENXIO; it never sleeps (it
 *    is called from URB completions) and hands frames to a work item;
 *  - hci_send_frame() frees the skb when the driver's send() fails;
 *  - __hci_cmd_sync*(): one request at a time; Command Complete returns
 *    the return parameters (starting with the status byte), a failed
 *    Command Status returns its error, a successful Command Status with
 *    no event requested returns -ENODATA, a requested event returns its
 *    parameters; timeout -ETIMEDOUT; hci_cmd_sync_cancel(err) ends the
 *    pending request with -err;
 *  - __hci_cmd_send() only for vendor commands (OGF 0x3f);
 *  - setup() runs at the first open (HCI_SETUP), or at every open with
 *    HCI_QUIRK_NON_PERSISTENT_SETUP; shutdown() before close() unless the
 *    device is being unregistered;
 *  - sync-command work and frame reception run on separate workqueues
 *    (a sync command waits for an event the receive work delivers).
 * Events answering the driver's own commands are consumed here; all other
 * events and ACL data go to Haiku once bluetooth_server brought the device
 * up. SCO/ISO data are counted and dropped: Haiku has no audio path yet.
 */
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/bitmap.h>
#include <linux/unaligned.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "lkpi_bt_bridge.h"

extern const char *gDriverName;		/* freebsd_network haiku-module.h */

#define	LKPI_HCI_MAX_DEVS	32
#define	LKPI_HCI_MAX_CONNS	32

struct lkpi_hci_conn {
	u16	handle;
	u8	type;		/* ACL_LINK, SCO_LINK, ESCO_LINK, LE_LINK */
};

struct lkpi_hci_sync {
	struct list_head		link;
	hci_cmd_sync_work_func_t	func;
	void				*data;
	hci_cmd_sync_work_destroy_t	destroy;
};

struct lkpi_hci {
	struct hci_dev		*hdev;
	struct lkpi_bt_node	*node;

	struct workqueue_struct	*rx_wq;		/* frames from the driver */
	struct work_struct	rx_work;
	struct sk_buff_head	rx_q;

	struct workqueue_struct	*sync_wq;	/* hci_cmd_sync_queue() */
	struct work_struct	sync_work;
	struct list_head	sync_list;
	spinlock_t		sync_lock;

	struct mutex		op_lock;	/* up / down / unregister */
	struct mutex		req_lock;	/* one sync request */
	spinlock_t		req_slock;	/* the fields below */
	bool			req_pending;
	u16			req_opcode;
	u8			req_event;
	struct sk_buff		*req_skb;
	int			req_err;
	struct completion	req_done;

	spinlock_t		conn_lock;
	int			nconns;
	struct lkpi_hci_conn	conns[LKPI_HCI_MAX_CONNS];
};

static DEFINE_MUTEX(lkpi_hci_list_lock);
static LIST_HEAD(lkpi_hci_list);
static DECLARE_BITMAP(lkpi_hci_ids, LKPI_HCI_MAX_DEVS);

/* ---- errors --------------------------------------------------------------- */

int
bt_to_errno(u16 code)
{
	switch (code) {
	case 0x00:	return (0);
	case 0x01:	return (-EBADRQC);	/* unknown command */
	case 0x02:	return (-ENOTCONN);	/* unknown connection id */
	case 0x03:	return (-EIO);		/* hardware failure */
	case 0x04:	return (-EHOSTDOWN);	/* page timeout */
	case 0x05:	return (-EACCES);	/* authentication failure */
	case 0x06:	return (-EBADE);	/* PIN or key missing */
	case 0x07:	return (-ENOMEM);	/* memory capacity exceeded */
	case 0x08:	return (-ETIMEDOUT);	/* connection timeout */
	case 0x09:			/* connection limit exceeded */
	case 0x0a:	return (-EMLINK);	/* synchronous limit exceeded */
	case 0x0b:	return (-EALREADY);	/* connection already exists */
	case 0x0c:	return (-EBUSY);	/* command disallowed */
	case 0x0d:			/* rejected: limited resources */
	case 0x0e:			/* rejected: security */
	case 0x0f:	return (-ECONNREFUSED);	/* rejected: bad address */
	case 0x10:	return (-ETIMEDOUT);	/* accept timeout exceeded */
	case 0x11:			/* unsupported feature/parameter */
	case 0x1a:	return (-EOPNOTSUPP);	/* unsupported remote feature */
	case 0x12:	return (-EINVAL);	/* invalid parameters */
	case 0x13:			/* remote user terminated */
	case 0x14:			/* remote low resources */
	case 0x15:	return (-ECONNRESET);	/* remote power off */
	case 0x16:	return (-ECONNABORTED);	/* local host terminated */
	case 0x17:	return (-ELOOP);	/* repeated attempts */
	case 0x18:	return (-EACCES);	/* pairing not allowed */
	case 0x1e:			/* invalid LMP/LL parameters */
	case 0x1f:	return (-EPROTO);	/* unspecified error */
	case 0x3c:	return (-EHOSTUNREACH);	/* advertising timeout */
	}
	return (-EIO);
}

static int
lkpi_hci_result(int err)
{
	switch (err) {
	case 0:			return (LKPI_BT_OK);
	case -ENOMEM:		return (LKPI_BT_NOMEM);
	case -ENODEV:
	case -ENXIO:
	case -ESHUTDOWN:	return (LKPI_BT_NODEV);
	case -EINVAL:		return (LKPI_BT_INVAL);
	case -EOPNOTSUPP:	return (LKPI_BT_UNSUPP);
	case -ETIMEDOUT:	return (LKPI_BT_TIMEOUT);
	}
	return (LKPI_BT_IO);
}

/* ---- allocation ----------------------------------------------------------- */

static void
lkpi_hci_devcd_timeout(struct work_struct *work)
{
	struct hci_dev *hdev = container_of(to_delayed_work(work),
	    struct hci_dev, dump.dump_timeout);

	hdev->dump.state = HCI_DEVCOREDUMP_TIMEOUT;	/* as Linux */
}

struct hci_dev *
hci_alloc_dev_priv(int sizeof_priv)
{
	struct hci_dev *hdev;
	struct lkpi_hci *h;

	if (sizeof_priv < 0)
		return (NULL);
	hdev = kzalloc(sizeof(*hdev) + sizeof_priv, GFP_KERNEL);
	if (hdev == NULL)
		return (NULL);
	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (h == NULL) {
		kfree(hdev);
		return (NULL);
	}
	h->hdev = hdev;
	hdev->lkpi = h;
	INIT_LIST_HEAD(&hdev->list);
	mutex_init(&hdev->lock);
	hdev->id = HCI_DEV_NONE;
	hdev->dump.state = HCI_DEVCOREDUMP_IDLE;
	INIT_DELAYED_WORK(&hdev->dump.dump_timeout, lkpi_hci_devcd_timeout);
	/* Linux's defaults for fields drivers may read before setup */
	hdev->voice_setting = 0x0060;
	hdev->num_iac = 0x01;

	skb_queue_head_init(&h->rx_q);
	INIT_LIST_HEAD(&h->sync_list);
	spin_lock_init(&h->sync_lock);
	mutex_init(&h->op_lock);
	mutex_init(&h->req_lock);
	spin_lock_init(&h->req_slock);
	init_completion(&h->req_done);
	spin_lock_init(&h->conn_lock);
	return (hdev);
}

void
hci_free_dev(struct hci_dev *hdev)
{
	struct lkpi_hci *h;

	if (hdev == NULL)
		return;
	h = hdev->lkpi;
	if (h != NULL) {
		skb_queue_purge(&h->rx_q);
		mutex_destroy(&h->op_lock);
		mutex_destroy(&h->req_lock);
		kfree(h);
	}
	mutex_destroy(&hdev->lock);
	kfree(hdev);
}

void
hci_release_dev(struct hci_dev *hdev)
{
	/* Linux: final release from the device core; here hci_free_dev() */
}

/* ---- synchronous requests --------------------------------------------- */

/* End the pending request with err (if any). */
static void
lkpi_hci_req_end(struct lkpi_hci *h, int err)
{
	unsigned long flags;

	spin_lock_irqsave(&h->req_slock, flags);
	if (h->req_pending && h->req_skb == NULL && h->req_err == 0) {
		h->req_err = err;
		complete(&h->req_done);
	}
	spin_unlock_irqrestore(&h->req_slock, flags);
}

/*
 * An event from the controller: does it answer the pending request?
 * Takes the skb (and returns true) if so.
 */
static bool
lkpi_hci_req_event(struct lkpi_hci *h, struct sk_buff *skb)
{
	const u8 *p = skb->data;
	u8 evt = p[0], plen = p[1];
	unsigned long flags;
	bool taken = false;
	int err = 0;

	spin_lock_irqsave(&h->req_slock, flags);
	if (!h->req_pending || h->req_skb != NULL || h->req_err != 0)
		goto out;

	if (evt == HCI_EV_CMD_COMPLETE && plen >= 3 && skb->len >= 5 &&
	    get_unaligned_le16(p + 3) == h->req_opcode) {
		if (h->req_event != 0)
			goto out;	/* waits for another event */
		skb_pull(skb, HCI_EVENT_HDR_SIZE +
		    sizeof(struct hci_ev_cmd_complete));
		taken = true;
	} else if (evt == HCI_EV_CMD_STATUS && plen >= 4 && skb->len >= 6 &&
	    get_unaligned_le16(p + 4) == h->req_opcode) {
		if (p[2] != 0)
			err = bt_to_errno(p[2]);
		else if (h->req_event == 0)
			err = -ENODATA;	/* Linux: nothing to return */
		else
			goto out;	/* the requested event follows */
	} else if (h->req_event != 0 && evt == h->req_event) {
		skb_pull(skb, HCI_EVENT_HDR_SIZE);
		taken = true;
	} else
		goto out;

	if (taken)
		h->req_skb = skb;
	else
		h->req_err = err;
	complete(&h->req_done);
	spin_unlock_irqrestore(&h->req_slock, flags);
	if (!taken)
		kfree_skb(skb);
	return (true);
out:
	spin_unlock_irqrestore(&h->req_slock, flags);
	return (false);
}

static struct sk_buff *
lkpi_hci_cmd_skb(u16 opcode, u32 plen, const void *param)
{
	struct hci_command_hdr *hdr;
	struct sk_buff *skb;

	if (plen > 255)
		return (NULL);
	skb = bt_skb_alloc(HCI_COMMAND_HDR_SIZE + plen, GFP_KERNEL);
	if (skb == NULL)
		return (NULL);
	hdr = skb_put(skb, HCI_COMMAND_HDR_SIZE);
	hdr->opcode = cpu_to_le16(opcode);
	hdr->plen = plen;
	if (plen > 0)
		skb_put_data(skb, param, plen);
	hci_skb_pkt_type(skb) = HCI_COMMAND_PKT;
	hci_skb_opcode(skb) = opcode;
	return (skb);
}

struct sk_buff *
__hci_cmd_sync_ev(struct hci_dev *hdev, u16 opcode, u32 plen,
    const void *param, u8 event, u32 timeout)
{
	struct lkpi_hci *h = hdev->lkpi;
	struct sk_buff *skb, *resp;
	unsigned long flags;
	int err;

	if (hci_dev_test_flag(hdev, HCI_UNREGISTER))
		return (ERR_PTR(-ENODEV));
	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return (ERR_PTR(-ENETDOWN));
	skb = lkpi_hci_cmd_skb(opcode, plen, param);
	if (skb == NULL)
		return (ERR_PTR(plen > 255 ? -EINVAL : -ENOMEM));
	hci_skb_event(skb) = event;

	mutex_lock(&h->req_lock);
	spin_lock_irqsave(&h->req_slock, flags);
	h->req_pending = true;
	h->req_opcode = opcode;
	h->req_event = event;
	h->req_skb = NULL;
	h->req_err = 0;
	reinit_completion(&h->req_done);
	spin_unlock_irqrestore(&h->req_slock, flags);

	err = hci_send_frame(hdev, skb);	/* frees skb on error */
	if (err == 0)
		wait_for_completion_timeout(&h->req_done, timeout);

	spin_lock_irqsave(&h->req_slock, flags);
	h->req_pending = false;
	resp = h->req_skb;
	if (err == 0)
		err = h->req_err;
	h->req_skb = NULL;
	spin_unlock_irqrestore(&h->req_slock, flags);
	mutex_unlock(&h->req_lock);

	if (resp != NULL)
		return (resp);
	if (err == 0) {
		bt_dev_err(hdev, "command 0x%4.4x tx timeout", opcode);
		err = -ETIMEDOUT;
	}
	return (ERR_PTR(err));
}

struct sk_buff *
__hci_cmd_sync(struct hci_dev *hdev, u16 opcode, u32 plen, const void *param,
    u32 timeout)
{
	return (__hci_cmd_sync_ev(hdev, opcode, plen, param, 0, timeout));
}

struct sk_buff *
hci_cmd_sync(struct hci_dev *hdev, u16 opcode, u32 plen, const void *param,
    u32 timeout)
{
	return (__hci_cmd_sync_ev(hdev, opcode, plen, param, 0, timeout));
}

int
__hci_cmd_sync_status(struct hci_dev *hdev, u16 opcode, u32 plen,
    const void *param, u32 timeout)
{
	struct sk_buff *skb;
	u8 status;

	skb = __hci_cmd_sync(hdev, opcode, plen, param, timeout);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "opcode 0x%4.4x failed: %ld", opcode,
		    PTR_ERR(skb));
		return (PTR_ERR(skb));
	}
	status = skb->len > 0 ? skb->data[0] : 0;	/* HCI status */
	kfree_skb(skb);
	return (status);
}

int
__hci_cmd_send(struct hci_dev *hdev, u16 opcode, u32 plen, const void *param)
{
	struct sk_buff *skb;

	if (hci_opcode_ogf(opcode) != 0x3f) {
		/* Linux: a command without a waiter would leave the core's
		 * command flow control stuck; only vendor commands */
		bt_dev_err(hdev, "unresponded command not supported");
		return (-EINVAL);
	}
	skb = lkpi_hci_cmd_skb(opcode, plen, param);
	if (skb == NULL)
		return (plen > 255 ? -EINVAL : -ENOMEM);
	return (hci_send_frame(hdev, skb));
}

void
hci_cmd_sync_cancel(struct hci_dev *hdev, int err)
{
	if (hdev->lkpi != NULL)
		lkpi_hci_req_end(hdev->lkpi, -err);
}

void
hci_cmd_sync_cancel_sync(struct hci_dev *hdev, int err)
{
	hci_cmd_sync_cancel(hdev, err);
}

static void
lkpi_hci_sync_work(struct work_struct *work)
{
	struct lkpi_hci *h = container_of(work, struct lkpi_hci, sync_work);
	struct lkpi_hci_sync *e;
	int err;

	for (;;) {
		spin_lock(&h->sync_lock);
		e = list_first_entry_or_null(&h->sync_list,
		    struct lkpi_hci_sync, link);
		if (e != NULL)
			list_del(&e->link);
		spin_unlock(&h->sync_lock);
		if (e == NULL)
			break;
		err = e->func(h->hdev, e->data);
		if (e->destroy != NULL)
			e->destroy(h->hdev, e->data, err);
		kfree(e);
	}
}

int
hci_cmd_sync_submit(struct hci_dev *hdev, hci_cmd_sync_work_func_t func,
    void *data, hci_cmd_sync_work_destroy_t destroy)
{
	struct lkpi_hci *h = hdev->lkpi;
	struct lkpi_hci_sync *e;

	if (hci_dev_test_flag(hdev, HCI_UNREGISTER) || h->sync_wq == NULL)
		return (-ENODEV);
	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (e == NULL)
		return (-ENOMEM);
	e->func = func;
	e->data = data;
	e->destroy = destroy;
	spin_lock(&h->sync_lock);
	list_add_tail(&e->link, &h->sync_list);
	spin_unlock(&h->sync_lock);
	queue_work(h->sync_wq, &h->sync_work);
	return (0);
}

int
hci_cmd_sync_queue(struct hci_dev *hdev, hci_cmd_sync_work_func_t func,
    void *data, hci_cmd_sync_work_destroy_t destroy)
{
	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return (-ENETDOWN);
	return (hci_cmd_sync_submit(hdev, func, data, destroy));
}

/* Drop queued sync work (device going down): destroy(-ECANCELED). */
static void
lkpi_hci_sync_flush(struct lkpi_hci *h)
{
	struct lkpi_hci_sync *e;

	if (h->sync_wq == NULL)
		return;
	flush_workqueue(h->sync_wq);
	for (;;) {
		spin_lock(&h->sync_lock);
		e = list_first_entry_or_null(&h->sync_list,
		    struct lkpi_hci_sync, link);
		if (e != NULL)
			list_del(&e->link);
		spin_unlock(&h->sync_lock);
		if (e == NULL)
			break;
		if (e->destroy != NULL)
			e->destroy(h->hdev, e->data, -ECANCELED);
		kfree(e);
	}
}

/* ---- connections (observed in events) ------------------------------------- */

static void
lkpi_hci_conn_add(struct lkpi_hci *h, u16 handle, u8 type)
{
	struct hci_dev *hdev = h->hdev;
	bool added = false;
	int i;

	spin_lock(&h->conn_lock);
	for (i = 0; i < h->nconns; i++) {
		if (h->conns[i].handle == handle)
			break;
	}
	if (i == h->nconns && h->nconns < LKPI_HCI_MAX_CONNS) {
		h->conns[h->nconns].handle = handle;
		h->conns[h->nconns].type = type;
		h->nconns++;
		added = true;
	}
	spin_unlock(&h->conn_lock);
	if (added && hdev->notify != NULL)
		hdev->notify(hdev, HCI_NOTIFY_CONN_ADD);
}

static void
lkpi_hci_conn_del(struct lkpi_hci *h, u16 handle)
{
	struct hci_dev *hdev = h->hdev;
	bool removed = false;
	int i;

	spin_lock(&h->conn_lock);
	for (i = 0; i < h->nconns; i++) {
		if (h->conns[i].handle == handle) {
			h->conns[i] = h->conns[--h->nconns];
			removed = true;
			break;
		}
	}
	spin_unlock(&h->conn_lock);
	if (removed && hdev->notify != NULL)
		hdev->notify(hdev, HCI_NOTIFY_CONN_DEL);
}

static void
lkpi_hci_conn_reset(struct lkpi_hci *h)
{
	spin_lock(&h->conn_lock);
	h->nconns = 0;
	spin_unlock(&h->conn_lock);
}

/* Connection events, so hci_conn_num() and notify() work for drivers
 * (btusb switches its SCO alternate setting on them). */
static void
lkpi_hci_track(struct lkpi_hci *h, const u8 *p, unsigned int len)
{
	u8 evt = p[0], plen = p[1];
	const u8 *e = p + HCI_EVENT_HDR_SIZE;

	if (len < HCI_EVENT_HDR_SIZE + (unsigned)plen)
		return;
	switch (evt) {
	case HCI_EV_CONN_COMPLETE:	/* status handle bdaddr link_type */
		if (plen >= 10 && e[0] == 0)
			lkpi_hci_conn_add(h, hci_handle(get_unaligned_le16(e + 1)),
			    e[9] == 0x00 ? SCO_LINK : ACL_LINK);
		break;
	case HCI_EV_SYNC_CONN_COMPLETE:	/* status handle bdaddr link_type */
		if (plen >= 10 && e[0] == 0)
			lkpi_hci_conn_add(h, hci_handle(get_unaligned_le16(e + 1)),
			    e[9] == 0x02 ? ESCO_LINK : SCO_LINK);
		break;
	case HCI_EV_DISCONN_COMPLETE:	/* status handle reason */
		if (plen >= 3 && e[0] == 0)
			lkpi_hci_conn_del(h, hci_handle(get_unaligned_le16(e + 1)));
		break;
	case HCI_EV_LE_META:		/* subevent status handle */
		/* LE (Enhanced) Connection Complete v1/v2 */
		if (plen >= 4 && (e[0] == 0x01 || e[0] == 0x0a ||
		    e[0] == 0x29) && e[1] == 0)
			lkpi_hci_conn_add(h, hci_handle(get_unaligned_le16(e + 2)),
			    LE_LINK);
		break;
	}
}

unsigned int
hci_conn_num(struct hci_dev *hdev, __u8 type)
{
	struct lkpi_hci *h = hdev->lkpi;
	unsigned int n = 0;
	int i;

	spin_lock(&h->conn_lock);
	for (i = 0; i < h->nconns; i++)
		n += h->conns[i].type == type;
	spin_unlock(&h->conn_lock);
	return (n);
}

unsigned int
hci_conn_count(struct hci_dev *hdev)
{
	struct lkpi_hci *h = hdev->lkpi;
	unsigned int n;

	spin_lock(&h->conn_lock);
	n = h->nconns;
	spin_unlock(&h->conn_lock);
	return (n);
}

/* ---- frames ------------------------------------------------------------- */

int
hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	int err;

	if (!test_bit(HCI_RUNNING, &hdev->flags)) {
		kfree_skb(skb);
		return (-EINVAL);
	}
	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:	hdev->stat.cmd_tx++; break;
	case HCI_ACLDATA_PKT:	hdev->stat.acl_tx++; break;
	case HCI_SCODATA_PKT:	hdev->stat.sco_tx++; break;
	}
	err = hdev->send(hdev, skb);
	if (err < 0) {
		bt_dev_err(hdev, "sending frame failed (%d)", err);
		kfree_skb(skb);
		return (err);
	}
	return (0);
}

int
hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct lkpi_hci *h;

	if (hdev == NULL || (!test_bit(HCI_UP, &hdev->flags) &&
	    !test_bit(HCI_INIT, &hdev->flags))) {
		kfree_skb(skb);
		return (-ENXIO);
	}
	switch (hci_skb_pkt_type(skb)) {
	case HCI_EVENT_PKT:
	case HCI_ACLDATA_PKT:
	case HCI_SCODATA_PKT:
	case HCI_ISODATA_PKT:
		break;
	default:
		kfree_skb(skb);
		return (-EINVAL);
	}
	h = hdev->lkpi;
	bt_cb(skb)->incoming = 1;
	skb_queue_tail(&h->rx_q, skb);
	queue_work(h->rx_wq, &h->rx_work);
	return (0);
}

int
hci_recv_diag(struct hci_dev *hdev, struct sk_buff *skb)
{
	/* vendor diagnostics: no consumer on Haiku */
	kfree_skb(skb);
	return (0);
}

static void
lkpi_hci_rx_work(struct work_struct *work)
{
	struct lkpi_hci *h = container_of(work, struct lkpi_hci, rx_work);
	struct hci_dev *hdev = h->hdev;
	struct lkpi_bt_node *node;
	struct sk_buff *skb;
	int type;

	while ((skb = skb_dequeue(&h->rx_q)) != NULL) {
		switch (hci_skb_pkt_type(skb)) {
		case HCI_EVENT_PKT:
			hdev->stat.evt_rx++;
			if (skb->len < HCI_EVENT_HDR_SIZE) {
				hdev->stat.err_rx++;
				break;
			}
			if (lkpi_hci_req_event(h, skb))
				continue;	/* answered our command */
			lkpi_hci_track(h, skb->data, skb->len);
			type = LKPI_BT_EVT;
			goto haiku;
		case HCI_ACLDATA_PKT:
			hdev->stat.acl_rx++;
			type = LKPI_BT_ACL;
		haiku:
			/* hci_unregister_dev() clears h->node, then flushes
			 * this queue before it destroys the node */
			node = READ_ONCE(h->node);
			if (node != NULL && test_bit(HCI_UP, &hdev->flags))
				lkpi_bt_node_rx(node, type, skb->data,
				    skb->len);
			break;
		case HCI_SCODATA_PKT:
			hdev->stat.sco_rx++;
			break;
		}
		kfree_skb(skb);
	}
}

/* ---- registration ------------------------------------------------------- */

int
hci_register_dev(struct hci_dev *hdev)
{
	struct lkpi_hci *h = hdev->lkpi;
	struct lkpi_bt_node_info info;
	int id, r;

	if (h == NULL || hdev->open == NULL || hdev->close == NULL ||
	    hdev->send == NULL)
		return (-EINVAL);

	mutex_lock(&lkpi_hci_list_lock);
	id = find_first_zero_bit(lkpi_hci_ids, LKPI_HCI_MAX_DEVS);
	if (id >= LKPI_HCI_MAX_DEVS) {
		mutex_unlock(&lkpi_hci_list_lock);
		return (-ENFILE);
	}
	set_bit(id, lkpi_hci_ids);
	mutex_unlock(&lkpi_hci_list_lock);
	hdev->id = id;
	snprintf(hdev->name, sizeof(hdev->name), "hci%d", id);
	hdev->dev.kobj.name = hdev->name;	/* dev_name() */

	h->rx_wq = alloc_ordered_workqueue(hdev->name, 0);
	h->sync_wq = alloc_ordered_workqueue(hdev->name, 0);
	if (h->rx_wq == NULL || h->sync_wq == NULL) {
		r = -ENOMEM;
		goto fail;
	}
	INIT_WORK(&h->rx_work, lkpi_hci_rx_work);
	INIT_WORK(&h->sync_work, lkpi_hci_sync_work);
	hci_dev_set_flag(hdev, HCI_SETUP);	/* setup() at first open */

	memset(&info, 0, sizeof(info));
	info.driver = gDriverName;
	info.index = id;
	if (hdev->bus == HCI_USB && hdev->dev.parent != NULL) {
		struct usb_device *udev =
		    interface_to_usbdev(to_usb_interface(hdev->dev.parent));

		info.vendor = le16_to_cpu(udev->descriptor.idVendor);
		info.product = le16_to_cpu(udev->descriptor.idProduct);
		info.desc = "USB Bluetooth controller";
	}
	mutex_lock(&lkpi_hci_list_lock);
	list_add_tail(&hdev->list, &lkpi_hci_list);
	mutex_unlock(&lkpi_hci_list_lock);

	r = lkpi_bt_node_create(hdev, &info, &h->node);
	if (r != LKPI_BT_OK) {
		mutex_lock(&lkpi_hci_list_lock);
		list_del_init(&hdev->list);
		mutex_unlock(&lkpi_hci_list_lock);
		r = (r == LKPI_BT_NOMEM) ? -ENOMEM : -EINVAL;
		goto fail;
	}
	return (id);
fail:
	if (h->rx_wq != NULL)
		destroy_workqueue(h->rx_wq);
	if (h->sync_wq != NULL)
		destroy_workqueue(h->sync_wq);
	h->rx_wq = h->sync_wq = NULL;
	mutex_lock(&lkpi_hci_list_lock);
	clear_bit(id, lkpi_hci_ids);
	mutex_unlock(&lkpi_hci_list_lock);
	hdev->id = HCI_DEV_NONE;
	return (r);
}

/* op_lock held */
static void
lkpi_hci_do_close(struct hci_dev *hdev, bool shutdown)
{
	struct lkpi_hci *h = hdev->lkpi;

	if (!test_bit(HCI_UP, &hdev->flags))
		return;
	if (shutdown && hdev->shutdown != NULL &&
	    !hci_dev_test_flag(hdev, HCI_UNREGISTER) &&
	    test_bit(HCI_RUNNING, &hdev->flags))
		hdev->shutdown(hdev);
	/* queued sync work still talks to the controller: run it while
	 * events are accepted (HCI_UP), then stop */
	lkpi_hci_sync_flush(h);
	clear_bit(HCI_UP, &hdev->flags);
	lkpi_hci_req_end(h, -ENODEV);
	flush_workqueue(h->rx_wq);
	skb_queue_purge(&h->rx_q);
	if (hdev->flush != NULL)
		hdev->flush(hdev);
	clear_bit(HCI_RUNNING, &hdev->flags);
	hdev->close(hdev);
	lkpi_hci_conn_reset(h);
}

void
hci_unregister_dev(struct hci_dev *hdev)
{
	struct lkpi_hci *h = hdev->lkpi;
	struct lkpi_bt_node *node;

	if (h == NULL || hdev->id == HCI_DEV_NONE)
		return;
	hci_dev_set_flag(hdev, HCI_UNREGISTER);
	cancel_delayed_work_sync(&hdev->dump.dump_timeout);
	/* a setup in progress gets no more answers: end its request now so
	 * the bridge's wait for running calls is short */
	lkpi_hci_req_end(h, -ENODEV);
	node = h->node;
	WRITE_ONCE(h->node, NULL);	/* receive work stops using it */
	flush_workqueue(h->rx_wq);
	lkpi_bt_node_destroy(node);	/* no new calls from Haiku */

	mutex_lock(&h->op_lock);
	lkpi_hci_do_close(hdev, false);
	mutex_unlock(&h->op_lock);

	lkpi_hci_sync_flush(h);
	destroy_workqueue(h->sync_wq);
	flush_workqueue(h->rx_wq);
	destroy_workqueue(h->rx_wq);
	h->rx_wq = h->sync_wq = NULL;
	skb_queue_purge(&h->rx_q);

	mutex_lock(&lkpi_hci_list_lock);
	list_del_init(&hdev->list);
	clear_bit(hdev->id, lkpi_hci_ids);
	mutex_unlock(&lkpi_hci_list_lock);
	hdev->id = HCI_DEV_NONE;
}

int
hci_reset_dev(struct hci_dev *hdev)
{
	static const u8 hw_err[] = { HCI_EV_HARDWARE_ERROR, 0x01, 0x00 };
	struct sk_buff *skb;

	/* Linux: report a hardware error, the host stack resets the
	 * controller; here Haiku's stack gets the event */
	skb = bt_skb_alloc(sizeof(hw_err), GFP_ATOMIC);
	if (skb == NULL)
		return (-ENOMEM);
	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
	skb_put_data(skb, hw_err, sizeof(hw_err));
	return (hci_recv_frame(hdev, skb));
}

int
hci_suspend_dev(struct hci_dev *hdev)
{
	return (0);	/* Haiku has no system suspend for drivers yet */
}

int
hci_resume_dev(struct hci_dev *hdev)
{
	return (0);
}

/* ---- device core dumps: unavailable ------------------------------------- */

int
hci_devcd_register(struct hci_dev *hdev, coredump_t coredump,
    dmp_hdr_t dmp_hdr, notify_change_t notify_change)
{
	return (-EOPNOTSUPP);
}

int
hci_devcd_init(struct hci_dev *hdev, u32 dump_size)
{
	return (-EOPNOTSUPP);
}

int
hci_devcd_append(struct hci_dev *hdev, struct sk_buff *skb)
{
	kfree_skb(skb);
	return (-EOPNOTSUPP);
}

int
hci_devcd_append_pattern(struct hci_dev *hdev, u8 pattern, u32 len)
{
	return (-EOPNOTSUPP);
}

int
hci_devcd_complete(struct hci_dev *hdev)
{
	return (-EOPNOTSUPP);
}

int
hci_devcd_abort(struct hci_dev *hdev)
{
	return (-EOPNOTSUPP);
}

/* ---- bridge: called by lkpi_bt_haiku.cpp -------------------------------- */

int
lkpi_hci_up(void *ctx)
{
	struct hci_dev *hdev = ctx;
	struct lkpi_hci *h = hdev->lkpi;
	int err = 0;

	mutex_lock(&h->op_lock);
	if (hci_dev_test_flag(hdev, HCI_UNREGISTER)) {
		err = -ENODEV;
		goto out;
	}
	if (test_bit(HCI_UP, &hdev->flags))
		goto out;

	set_bit(HCI_INIT, &hdev->flags);
	err = hdev->open(hdev);
	if (err != 0) {
		clear_bit(HCI_INIT, &hdev->flags);
		bt_dev_err(hdev, "open failed (%d)", err);
		goto out;
	}
	set_bit(HCI_RUNNING, &hdev->flags);

	if (hdev->setup != NULL && (hci_dev_test_flag(hdev, HCI_SETUP) ||
	    hci_test_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP))) {
		err = hdev->setup(hdev);
		if (err != 0)
			bt_dev_err(hdev, "setup failed (%d)", err);
	}
	if (err == 0 && hdev->post_init != NULL) {
		err = hdev->post_init(hdev);
		if (err != 0)
			bt_dev_err(hdev, "post_init failed (%d)", err);
	}
	if (err != 0) {
		lkpi_hci_req_end(h, -ENODEV);
		lkpi_hci_sync_flush(h);
		flush_workqueue(h->rx_wq);
		skb_queue_purge(&h->rx_q);
		if (hdev->flush != NULL)
			hdev->flush(hdev);
		clear_bit(HCI_RUNNING, &hdev->flags);
		hdev->close(hdev);
		clear_bit(HCI_INIT, &hdev->flags);
		goto out;
	}
	hci_dev_clear_flag(hdev, HCI_SETUP);
	set_bit(HCI_UP, &hdev->flags);
	clear_bit(HCI_INIT, &hdev->flags);
	bt_dev_info(hdev, "up");
out:
	mutex_unlock(&h->op_lock);
	return (lkpi_hci_result(err));
}

void
lkpi_hci_down(void *ctx)
{
	struct hci_dev *hdev = ctx;
	struct lkpi_hci *h = hdev->lkpi;

	mutex_lock(&h->op_lock);
	lkpi_hci_do_close(hdev, true);
	mutex_unlock(&h->op_lock);
}

int
lkpi_hci_send(void *ctx, int type, const void *data, size_t len)
{
	struct hci_dev *hdev = ctx;
	struct sk_buff *skb;

	if (!test_bit(HCI_UP, &hdev->flags))
		return (LKPI_BT_NODEV);
	switch (type) {
	case LKPI_BT_CMD:
		if (len < HCI_COMMAND_HDR_SIZE)
			return (LKPI_BT_INVAL);
		break;
	case LKPI_BT_ACL:
		if (len < HCI_ACL_HDR_SIZE)
			return (LKPI_BT_INVAL);
		break;
	default:
		return (LKPI_BT_UNSUPP);
	}
	skb = bt_skb_alloc(len, GFP_KERNEL);
	if (skb == NULL)
		return (LKPI_BT_NOMEM);
	skb_put_data(skb, data, len);
	hci_skb_pkt_type(skb) = type;	/* LKPI_BT_* are HCI_*_PKT */
	if (type == LKPI_BT_CMD)
		hci_skb_opcode(skb) = get_unaligned_le16(data);
	return (lkpi_hci_result(hci_send_frame(hdev, skb)));
}

void
lkpi_hci_stats(void *ctx, struct lkpi_bt_stats *st)
{
	struct hci_dev *hdev = ctx;

	st->cmd_tx = hdev->stat.cmd_tx;
	st->evt_rx = hdev->stat.evt_rx;
	st->acl_tx = hdev->stat.acl_tx;
	st->acl_rx = hdev->stat.acl_rx;
	st->sco_tx = hdev->stat.sco_tx;
	st->sco_rx = hdev->stat.sco_rx;
	st->err_tx = hdev->stat.err_tx;
	st->err_rx = hdev->stat.err_rx;
	st->byte_tx = hdev->stat.byte_tx;
	st->byte_rx = hdev->stat.byte_rx;
}
