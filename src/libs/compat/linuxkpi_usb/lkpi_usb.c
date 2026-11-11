/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI side of LinuxKPI USB: Linux's USB driver model (struct
 * usb_device / usb_interface, driver binding, URBs, anchors, synchronous
 * messages) on top of the bridge in lkpi_usb_bridge.h, which talks to
 * Haiku's USB bus manager.
 *
 * Semantics follow Linux's drivers/usb/core (urb.c, driver.c, message.c,
 * hcd.c) where drivers can observe them; written for LinuxKPI_HaikuOS, no
 * Linux code. Points that matter to drivers:
 *  - completion order: unanchor, urb->status, complete(), use_count--,
 *    wake killers (Linux __usb_hcd_giveback_urb); anchors' suspend_wakeups
 *    keep usb_kill_anchored_urbs() from returning while a completion that
 *    may re-anchor is still running;
 *  - usb_kill_urb(): reject++, unlink with -ENOENT, wait for use_count 0;
 *  - usb_driver_release_interface() calls the driver's disconnect() for
 *    that interface (btusb relies on it for its SCO/diag interfaces);
 *  - interface/device class matching per usb_match_one_id().
 *
 * Haiku differences handled here:
 *  - Haiku cancels all transfers of a pipe at once. Unlinking one URB
 *    cancels its pipe; other IN URBs cancelled as a side effect are queued
 *    again. Limitation: a multi-packet bulk IN transfer cancelled midway
 *    loses the packets it had received (Haiku reports length 0); drivers
 *    that unlink single RX URBs while others are queued on the same pipe
 *    could see a gap. btusb/btmtk only kill whole anchors. OUT/control
 *    siblings cannot be safely re-sent (could duplicate data on the
 *    wire) and complete with -ECONNRESET, logged.
 *  - usb_unlink_urb() cancels from a work item (Haiku may complete the
 *    cancelled transfers in the cancelling thread); usb_kill_urb() and
 *    poison, which may sleep, cancel directly.
 *  - usb_set_interface() kills the URBs of the old alternate first (Linux
 *    usb_disable_interface()): Haiku drops a deleted pipe's transfers
 *    without completing them.
 *  - usb_reset_device() is not supported (fails with -EIO).
 *  - USB speed is not published by Haiku's USB API; it is inferred from
 *    bulk endpoint sizes (exact per USB 2.0 5.8.3 / USB 3.x 9.6.6 for bulk)
 *    and bcdUSB, or reported as USB_SPEED_FULL when nothing tells.
 *  - Isochronous transfers are not implemented yet (submit returns
 *    -EOPNOTSUPP): Haiku's Bluetooth stack has no SCO audio path to feed.
 */
#include <linux/usb.h>
#include <linux/usb/quirks.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/kernel.h>

#include <sys/kernel.h>		/* SYSINIT */

#include "lkpi_usb_bridge.h"

extern const char *gDriverName;		/* freebsd_network haiku-module.h */

static LIST_HEAD(lkpi_usb_drivers);
static LIST_HEAD(lkpi_usb_devices);
static DEFINE_MUTEX(lkpi_usb_lock);	/* drivers, devices, binding */
/* udev->state, lkpi_hk, lkpi_hk_users; never held across a blocking call */
static DEFINE_SPINLOCK(lkpi_usb_state_lock);
/* usb_host_endpoint.urb_list */
static DEFINE_SPINLOCK(lkpi_usb_ep_lock);
/* deferred cancels of usb_unlink_urb() (see lkpi_usb_unlink) */
static struct workqueue_struct *lkpi_usb_wq;
static DECLARE_WAIT_QUEUE_HEAD(lkpi_usb_kill_wq);
static atomic_t lkpi_usb_devnum = ATOMIC_INIT(0);
static bool lkpi_usb_started;

/* stage of a URB (urb->lkpi_xfer) */
#define	LKPI_URB_DATA		((void *)0)
#define	LKPI_URB_ZLP		((void *)1)	/* zero-length packet sent */

static int
lkpi_usb_errno(int hk)
{
	switch (hk) {
	case LKPI_HK_OK:	return (0);
	case LKPI_HK_CANCELED:	return (-ECONNRESET);
	case LKPI_HK_STALL:	return (-EPIPE);
	case LKPI_HK_CRC:	return (-EILSEQ);
	case LKPI_HK_OVERRUN:	return (-EOVERFLOW);
	case LKPI_HK_SHORT:	return (-EREMOTEIO);
	case LKPI_HK_TIMEOUT:	return (-ETIMEDOUT);
	case LKPI_HK_GONE:	return (-ESHUTDOWN);
	case LKPI_HK_NOMEM:	return (-ENOMEM);
	case LKPI_HK_INVAL:	return (-EINVAL);
	case LKPI_HK_UNSUPP:	return (-EOPNOTSUPP);
	}
	return (-EPROTO);
}

/*
 * Pin udev's Haiku handle for one call; NULL once the device is gone.
 * lkpi_usb_device_removed() waits for every pin before Haiku frees it.
 */
static struct lkpi_hk_dev *
lkpi_usb_hk_get(struct usb_device *udev)
{
	struct lkpi_hk_dev *hk;

	spin_lock(&lkpi_usb_state_lock);
	hk = udev->lkpi_hk;
	if (udev->state == USB_STATE_NOTATTACHED || hk == NULL)
		hk = NULL;
	else
		udev->lkpi_hk_users++;
	spin_unlock(&lkpi_usb_state_lock);
	return (hk);
}

static void
lkpi_usb_hk_put(struct usb_device *udev)
{
	bool wake;

	spin_lock(&lkpi_usb_state_lock);
	wake = --udev->lkpi_hk_users == 0;
	spin_unlock(&lkpi_usb_state_lock);
	if (wake)
		wake_up_all(&lkpi_usb_kill_wq);
}

/* ---- devices ----------------------------------------------------------- */

static void
lkpi_usb_release(struct kref *ref)
{
	struct usb_device *udev = container_of(ref, struct usb_device,
	    lkpi_ref);
	int i, a;

	for (i = 0; i < udev->lkpi_ninterfaces; i++) {
		struct usb_interface *intf = &udev->lkpi_interfaces[i];

		for (a = 0; a < (int)intf->num_altsetting; a++) {
			kfree(intf->altsetting[a].endpoint);
			kfree(intf->altsetting[a].extra);
		}
		kfree(intf->altsetting);
		kfree(intf->dev.kobj.name);
	}
	kfree(udev->lkpi_interfaces);
	kfree(udev->dev.kobj.name);
	kfree(udev);
}

struct usb_device *
usb_get_dev(struct usb_device *dev)
{
	if (dev != NULL)
		kref_get(&dev->lkpi_ref);
	return (dev);
}

void
usb_put_dev(struct usb_device *dev)
{
	if (dev != NULL)
		kref_put(&dev->lkpi_ref, lkpi_usb_release);
}

struct usb_interface *
usb_ifnum_to_if(const struct usb_device *dev, unsigned ifnum)
{
	int i;

	if (dev == NULL)
		return (NULL);
	for (i = 0; i < dev->lkpi_ninterfaces; i++) {
		struct usb_interface *intf = &dev->lkpi_interfaces[i];

		if (intf->altsetting[0].desc.bInterfaceNumber == ifnum)
			return (intf);
	}
	return (NULL);
}

struct usb_host_interface *
usb_altnum_to_altsetting(const struct usb_interface *intf, unsigned int altnum)
{
	unsigned i;

	for (i = 0; i < intf->num_altsetting; i++) {
		if (intf->altsetting[i].desc.bAlternateSetting == altnum)
			return (&intf->altsetting[i]);
	}
	return (NULL);
}

int
usb_find_common_endpoints(struct usb_host_interface *alt,
    struct usb_endpoint_descriptor **bulk_in,
    struct usb_endpoint_descriptor **bulk_out,
    struct usb_endpoint_descriptor **int_in,
    struct usb_endpoint_descriptor **int_out)
{
	int i;

	if (bulk_in) *bulk_in = NULL;
	if (bulk_out) *bulk_out = NULL;
	if (int_in) *int_in = NULL;
	if (int_out) *int_out = NULL;
	/* first match of each kind, as Linux does */
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *e = &alt->endpoint[i].desc;

		if (bulk_in && *bulk_in == NULL && usb_endpoint_is_bulk_in(e))
			*bulk_in = e;
		else if (bulk_out && *bulk_out == NULL &&
		    usb_endpoint_is_bulk_out(e))
			*bulk_out = e;
		else if (int_in && *int_in == NULL && usb_endpoint_is_int_in(e))
			*int_in = e;
		else if (int_out && *int_out == NULL &&
		    usb_endpoint_is_int_out(e))
			*int_out = e;
	}
	if ((bulk_in && *bulk_in == NULL) || (bulk_out && *bulk_out == NULL) ||
	    (int_in && *int_in == NULL) || (int_out && *int_out == NULL))
		return (-ENXIO);
	return (0);
}

/* Point ep_in[]/ep_out[] at the endpoints of intf's current alt setting. */
static void
lkpi_usb_map_endpoints(struct usb_device *udev, struct usb_interface *intf,
    bool on)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	int i;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_host_endpoint *ep = &alt->endpoint[i];
		int n = usb_endpoint_num(&ep->desc);

		if (usb_endpoint_dir_in(&ep->desc))
			udev->ep_in[n] = on ? ep : NULL;
		else
			udev->ep_out[n] = on ? ep : NULL;
		ep->enabled = on;
	}
}

static enum usb_device_speed
lkpi_usb_speed(struct usb_device *udev)
{
	int i, a, e, maxp = 0;

	/* Bulk wMaxPacketSize is fixed per speed: 64 full, 512 high,
	 * 1024 SuperSpeed. Interrupt/iso sizes are not conclusive. */
	for (i = 0; i < udev->lkpi_ninterfaces; i++) {
		struct usb_interface *intf = &udev->lkpi_interfaces[i];

		for (a = 0; a < (int)intf->num_altsetting; a++) {
			struct usb_host_interface *alt = &intf->altsetting[a];

			for (e = 0; e < alt->desc.bNumEndpoints; e++) {
				if (usb_endpoint_xfer_bulk(
				    &alt->endpoint[e].desc))
					maxp = max(maxp, usb_endpoint_maxp(
					    &alt->endpoint[e].desc));
			}
		}
	}
	if (maxp >= 1024)
		return (USB_SPEED_SUPER);
	if (maxp >= 512)
		return (USB_SPEED_HIGH);
	if (maxp > 0)
		return (USB_SPEED_FULL);
	/* no bulk endpoints: bcdUSB 3.x devices run SuperSpeed or USB 2 */
	return (USB_SPEED_FULL);
}

static void lkpi_usb_reset_work(struct work_struct *work);

static struct usb_device *
lkpi_usb_build(struct lkpi_hk_dev *hk)
{
	struct usb_device *udev;
	const uint8_t *d;
	int i, a, e, n;

	udev = kzalloc(sizeof(*udev), GFP_KERNEL);
	if (udev == NULL)
		return (NULL);
	kref_init(&udev->lkpi_ref);
	INIT_LIST_HEAD(&udev->lkpi_link);
	atomic_set(&udev->lkpi_inflight, 0);
	udev->lkpi_hk = hk;
	udev->devnum = (atomic_inc_return(&lkpi_usb_devnum) % 127) + 1;
	udev->state = USB_STATE_CONFIGURED;
	udev->can_submit = 1;
	memcpy(&udev->descriptor, lkpi_hk_device_descriptor(hk),
	    USB_DT_DEVICE_SIZE);
	udev->dev.kobj.name = kasprintf(GFP_KERNEL, "usb-%u", (unsigned)
	    lkpi_hk_device_id(hk));
	snprintf(udev->devpath, sizeof(udev->devpath), "%u",
	    (unsigned)lkpi_hk_device_id(hk));

	/* ep0 */
	udev->ep0.desc.bLength = USB_DT_ENDPOINT_SIZE;
	udev->ep0.desc.bDescriptorType = USB_DT_ENDPOINT;
	udev->ep0.desc.bmAttributes = USB_ENDPOINT_XFER_CONTROL;
	udev->ep0.desc.wMaxPacketSize =
	    cpu_to_le16(udev->descriptor.bMaxPacketSize0);
	udev->ep0.enabled = 1;
	INIT_LIST_HEAD(&udev->ep0.urb_list);
	udev->ep_in[0] = udev->ep_out[0] = &udev->ep0;

	n = lkpi_hk_num_interfaces(hk);
	udev->lkpi_interfaces = kcalloc(max(n, 1), sizeof(struct usb_interface),
	    GFP_KERNEL);
	if (udev->lkpi_interfaces == NULL)
		goto fail;
	udev->lkpi_ninterfaces = n;
	udev->actconfig = &udev->lkpi_config;
	udev->lkpi_config.desc.bLength = USB_DT_CONFIG_SIZE;
	udev->lkpi_config.desc.bDescriptorType = USB_DT_CONFIG;
	udev->lkpi_config.desc.bNumInterfaces = n;
	udev->lkpi_config.desc.bConfigurationValue = lkpi_hk_config_value(hk);

	for (i = 0; i < n; i++) {
		struct usb_interface *intf = &udev->lkpi_interfaces[i];
		int nalt = lkpi_hk_num_alts(hk, i);

		intf->lkpi_udev = udev;
		intf->usb_dev = &udev->dev;
		intf->dev.parent = &udev->dev;
		intf->minor = -1;
		INIT_WORK(&intf->reset_ws, lkpi_usb_reset_work);
		intf->altsetting = kcalloc(max(nalt, 1),
		    sizeof(struct usb_host_interface), GFP_KERNEL);
		if (intf->altsetting == NULL)
			goto fail;
		intf->num_altsetting = nalt;	/* release frees this many */
		if (i < (int)ARRAY_SIZE(udev->lkpi_config.interface))
			udev->lkpi_config.interface[i] = intf;
		for (a = 0; a < nalt; a++) {
			struct usb_host_interface *alt = &intf->altsetting[a];
			size_t xl;

			d = lkpi_hk_alt_descriptor(hk, i, a);
			if (d == NULL)
				goto fail;
			memcpy(&alt->desc, d, USB_DT_INTERFACE_SIZE);
			/* trust what Haiku parsed over bNumEndpoints */
			alt->desc.bNumEndpoints = lkpi_hk_alt_num_endpoints(hk,
			    i, a);
			alt->endpoint = kcalloc(max((int)alt->desc.bNumEndpoints,
			    1), sizeof(struct usb_host_endpoint), GFP_KERNEL);
			if (alt->endpoint == NULL)
				goto fail;
			for (e = 0; e < alt->desc.bNumEndpoints; e++) {
				INIT_LIST_HEAD(&alt->endpoint[e].urb_list);
				d = lkpi_hk_ep_descriptor(hk, i, a, e);
				if (d == NULL)
					goto fail;
				memcpy(&alt->endpoint[e].desc, d,
				    USB_DT_ENDPOINT_SIZE);
			}
			xl = lkpi_hk_alt_extra(hk, i, a, NULL, 0);
			if (xl > 0) {
				alt->extra = kmalloc(xl, GFP_KERNEL);
				if (alt->extra != NULL)
					alt->extralen = lkpi_hk_alt_extra(hk, i,
					    a, alt->extra, xl);
			}
		}
		a = lkpi_hk_active_alt(hk, i);
		intf->cur_altsetting = &intf->altsetting[
		    (a >= 0 && a < nalt) ? a : 0];
		intf->dev.kobj.name = kasprintf(GFP_KERNEL, "%s:1.%d",
		    udev->dev.kobj.name,
		    intf->cur_altsetting->desc.bInterfaceNumber);
		lkpi_usb_map_endpoints(udev, intf, true);
	}
	udev->speed = lkpi_usb_speed(udev);
	return (udev);
fail:
	usb_put_dev(udev);
	return (NULL);
}

/* ---- matching ---------------------------------------------------------- */

static bool
lkpi_usb_match_device(struct usb_device *dev, const struct usb_device_id *id)
{
	const struct usb_device_descriptor *d = &dev->descriptor;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
	    id->idVendor != le16_to_cpu(d->idVendor))
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) &&
	    id->idProduct != le16_to_cpu(d->idProduct))
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_LO) &&
	    id->bcdDevice_lo > le16_to_cpu(d->bcdDevice))
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_HI) &&
	    id->bcdDevice_hi < le16_to_cpu(d->bcdDevice))
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) &&
	    id->bDeviceClass != d->bDeviceClass)
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) &&
	    id->bDeviceSubClass != d->bDeviceSubClass)
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) &&
	    id->bDeviceProtocol != d->bDeviceProtocol)
		return (false);
	return (true);
}

static bool
lkpi_usb_match_one(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	const struct usb_interface_descriptor *i = &intf->cur_altsetting->desc;

	if (!lkpi_usb_match_device(dev, id))
		return (false);
	/* Linux: no interface matching against vendor-specific devices
	 * unless the entry names the vendor. */
	if (dev->descriptor.bDeviceClass == USB_CLASS_VENDOR_SPEC &&
	    !(id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
	    (id->match_flags & (USB_DEVICE_ID_MATCH_INT_INFO |
	    USB_DEVICE_ID_MATCH_INT_NUMBER)))
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) &&
	    id->bInterfaceClass != i->bInterfaceClass)
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) &&
	    id->bInterfaceSubClass != i->bInterfaceSubClass)
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) &&
	    id->bInterfaceProtocol != i->bInterfaceProtocol)
		return (false);
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_NUMBER) &&
	    id->bInterfaceNumber != i->bInterfaceNumber)
		return (false);
	return (true);
}

const struct usb_device_id *
usb_match_id(struct usb_interface *intf, const struct usb_device_id *id)
{
	if (id == NULL)
		return (NULL);
	/* table ends with an all-zero entry; Linux tests these fields */
	for (; id->idVendor || id->idProduct || id->bDeviceClass ||
	    id->bInterfaceClass || id->driver_info; id++) {
		if (lkpi_usb_match_one(intf, id))
			return (id);
	}
	return (NULL);
}

/* ---- binding ------------------------------------------------------------ */

static void
lkpi_usb_unbind(struct usb_interface *intf)
{
	struct usb_driver *drv = intf->lkpi_driver;

	if (drv == NULL || intf->condition != USB_INTERFACE_BOUND)
		return;
	intf->condition = USB_INTERFACE_UNBINDING;
	if (drv->disconnect != NULL)
		drv->disconnect(intf);
	usb_set_intfdata(intf, NULL);
	intf->lkpi_driver = NULL;
	intf->needs_remote_wakeup = 0;
	intf->condition = USB_INTERFACE_UNBOUND;
}

/* lkpi_usb_lock held */
static void
lkpi_usb_probe_interfaces(struct usb_device *udev)
{
	struct usb_driver *drv;
	int i;

	for (i = 0; i < udev->lkpi_ninterfaces; i++) {
		struct usb_interface *intf = &udev->lkpi_interfaces[i];

		if (intf->lkpi_driver != NULL)	/* claimed meanwhile */
			continue;
		list_for_each_entry(drv, &lkpi_usb_drivers, lkpi_link) {
			const struct usb_device_id *id;
			int error;

			id = usb_match_id(intf, drv->id_table);
			if (id == NULL)
				continue;
			intf->lkpi_driver = drv;
			intf->condition = USB_INTERFACE_BINDING;
			error = drv->probe(intf, id);
			if (error == 0) {
				intf->condition = USB_INTERFACE_BOUND;
				dev_info(&intf->dev, "bound to %s\n",
				    drv->name);
				break;
			}
			usb_set_intfdata(intf, NULL);
			intf->lkpi_driver = NULL;
			intf->condition = USB_INTERFACE_UNBOUND;
			if (error != -ENODEV)
				dev_info(&intf->dev, "%s: probe failed: %d\n",
				    drv->name, error);
		}
	}
}

int
usb_driver_claim_interface(struct usb_driver *driver,
    struct usb_interface *intf, void *data)
{
	if (intf->lkpi_driver != NULL)
		return (-EBUSY);
	intf->lkpi_driver = driver;
	usb_set_intfdata(intf, data);
	intf->condition = USB_INTERFACE_BOUND;
	return (0);
}

void
usb_driver_release_interface(struct usb_driver *driver,
    struct usb_interface *intf)
{
	/* not ours, or called from within its own disconnect() */
	if (intf == NULL || intf->lkpi_driver != driver ||
	    intf->condition != USB_INTERFACE_BOUND)
		return;
	lkpi_usb_unbind(intf);
}

/* Haiku: a new device. Bind Linux drivers to its interfaces. */
int
lkpi_usb_device_added(struct lkpi_hk_dev *hk, void **ctx)
{
	struct usb_device *udev;
	struct usb_driver *drv;
	bool any = false;
	int i;

	udev = lkpi_usb_build(hk);
	if (udev == NULL)
		return (LKPI_HK_NOMEM);

	mutex_lock(&lkpi_usb_lock);
	/* any interest at all? (cheap: no driver code runs) */
	for (i = 0; i < udev->lkpi_ninterfaces && !any; i++) {
		list_for_each_entry(drv, &lkpi_usb_drivers, lkpi_link) {
			if (usb_match_id(&udev->lkpi_interfaces[i],
			    drv->id_table) != NULL) {
				any = true;
				break;
			}
		}
	}
	if (!any) {
		mutex_unlock(&lkpi_usb_lock);
		usb_put_dev(udev);
		return (LKPI_HK_UNSUPP);
	}
	if (lkpi_hk_activate(hk) != LKPI_HK_OK) {
		mutex_unlock(&lkpi_usb_lock);
		dev_info(&udev->dev, "cannot select configuration\n");
		usb_put_dev(udev);
		return (LKPI_HK_ERROR);
	}
	list_add_tail(&udev->lkpi_link, &lkpi_usb_devices);
	lkpi_usb_probe_interfaces(udev);

	any = false;
	for (i = 0; i < udev->lkpi_ninterfaces; i++)
		any |= udev->lkpi_interfaces[i].lkpi_driver != NULL;
	if (!any) {
		list_del_init(&udev->lkpi_link);
		mutex_unlock(&lkpi_usb_lock);
		usb_put_dev(udev);
		return (LKPI_HK_UNSUPP);
	}
	mutex_unlock(&lkpi_usb_lock);
	*ctx = udev;
	return (LKPI_HK_OK);
}

/* Cancel everything queued for udev (completions run asynchronously). */
static void
lkpi_usb_cancel_all(struct usb_device *udev)
{
	struct lkpi_hk_dev *hk = udev->lkpi_hk;
	int n;

	lkpi_hk_cancel_control(hk);
	for (n = 1; n < 16; n++) {
		if (udev->ep_in[n] != NULL)
			lkpi_hk_cancel(hk, lkpi_hk_pipe(hk,
			    udev->ep_in[n]->desc.bEndpointAddress));
		if (udev->ep_out[n] != NULL)
			lkpi_hk_cancel(hk, lkpi_hk_pipe(hk,
			    udev->ep_out[n]->desc.bEndpointAddress));
	}
}

/* Cancel everything still queued for udev and wait until it completed. */
static void
lkpi_usb_drain(struct usb_device *udev)
{
	int i;

	for (i = 0; atomic_read(&udev->lkpi_inflight) != 0; i++) {
		/* again each round: a call that pinned hk just before the
		 * device was marked gone may still have queued a transfer */
		lkpi_usb_cancel_all(udev);
		if (i == 500)
			dev_info(&udev->dev, "waiting for %d transfers to "
			    "finish\n", atomic_read(&udev->lkpi_inflight));
		wait_event_timeout(lkpi_usb_kill_wq,
		    atomic_read(&udev->lkpi_inflight) == 0, msecs_to_jiffies(10));
	}
}

/* Haiku: the device is gone (unplugged, reset, or driver unloading). */
void
lkpi_usb_device_removed(void *ctx)
{
	struct usb_device *udev = ctx;
	int i;

	mutex_lock(&lkpi_usb_lock);
	spin_lock(&lkpi_usb_state_lock);
	udev->state = USB_STATE_NOTATTACHED;
	udev->can_submit = 0;
	spin_unlock(&lkpi_usb_state_lock);
	/* Fail what is in flight now (-ESHUTDOWN, no resubmission), as a
	 * Linux host controller does for a vanished device, so disconnect()
	 * does not wait on transfers that cannot finish. Unlocked: Haiku
	 * may run the completions in this thread. */
	mutex_unlock(&lkpi_usb_lock);
	lkpi_usb_cancel_all(udev);
	mutex_lock(&lkpi_usb_lock);
	for (i = 0; i < udev->lkpi_ninterfaces; i++)
		lkpi_usb_unbind(&udev->lkpi_interfaces[i]);
	list_del_init(&udev->lkpi_link);
	mutex_unlock(&lkpi_usb_lock);

	lkpi_usb_drain(udev);
	/* nothing may use hk once we return: Haiku frees it. Queued reset
	 * work is cancelled; calls that pinned hk before the device was
	 * marked gone are waited for. */
	for (i = 0; i < udev->lkpi_ninterfaces; i++)
		cancel_work_sync(&udev->lkpi_interfaces[i].reset_ws);
	wait_event(lkpi_usb_kill_wq, READ_ONCE(udev->lkpi_hk_users) == 0);
	spin_lock(&lkpi_usb_state_lock);
	udev->lkpi_hk = NULL;
	spin_unlock(&lkpi_usb_state_lock);
	usb_put_dev(udev);
}

/* Linux id table entry -> Haiku match (a superset; see bridge header). */
static void
lkpi_usb_id_to_match(const struct usb_device_id *id, struct lkpi_hk_match *m)
{
	memset(m, 0, sizeof(*m));
	if (id->match_flags & USB_DEVICE_ID_MATCH_VENDOR)
		m->vendor = id->idVendor;
	if (id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT)
		m->product = id->idProduct;
	/* Haiku matches one class triple against the device and all
	 * interfaces; prefer the interface triple when the entry has one */
	if (id->match_flags & USB_DEVICE_ID_MATCH_INT_INFO) {
		if (id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS)
			m->dev_class = id->bInterfaceClass;
		if (id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS)
			m->dev_subclass = id->bInterfaceSubClass;
		if (id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL)
			m->dev_protocol = id->bInterfaceProtocol;
	} else {
		if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS)
			m->dev_class = id->bDeviceClass;
		if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS)
			m->dev_subclass = id->bDeviceSubClass;
		if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL)
			m->dev_protocol = id->bDeviceProtocol;
	}
}

static bool
lkpi_usb_id_end(const struct usb_device_id *id)
{
	return (!(id->idVendor || id->idProduct || id->bDeviceClass ||
	    id->bInterfaceClass || id->driver_info));
}

/* lkpi_usb_lock held. Returns a kmalloc'ed array, or NULL if empty. */
static struct lkpi_hk_match *
lkpi_usb_matches(size_t *count)
{
	struct lkpi_hk_match *m;
	struct usb_driver *drv;
	const struct usb_device_id *id;
	size_t n = 0;

	list_for_each_entry(drv, &lkpi_usb_drivers, lkpi_link)
		for (id = drv->id_table; id != NULL && !lkpi_usb_id_end(id); id++)
			n++;
	*count = 0;
	if (n == 0)
		return (NULL);
	m = kcalloc(n, sizeof(*m), GFP_KERNEL);
	if (m == NULL)
		return (NULL);
	list_for_each_entry(drv, &lkpi_usb_drivers, lkpi_link)
		for (id = drv->id_table; id != NULL && !lkpi_usb_id_end(id); id++)
			lkpi_usb_id_to_match(id, &m[(*count)++]);
	return (m);
}

int
usb_register_driver(struct usb_driver *driver, struct module *owner,
    const char *mod_name)
{
	mutex_lock(&lkpi_usb_lock);
	list_add_tail(&driver->lkpi_link, &lkpi_usb_drivers);
	if (lkpi_usb_started) {
		/*
		 * Drivers register from module_init(), before the start below.
		 * A later one is not added to Haiku's match list: replacing
		 * the list while the add-on runs races with Haiku's explore
		 * thread reading it (Stack::RegisterDriver frees the old
		 * list under another lock). It still binds devices Haiku
		 * reports for the other drivers' entries.
		 */
		printf("%s: LinuxKPI USB: %s registered after start; its ID "
		    "table does not make Haiku report new devices\n",
		    gDriverName, driver->name);
	}
	mutex_unlock(&lkpi_usb_lock);
	return (0);
}

void
usb_deregister(struct usb_driver *driver)
{
	struct usb_device *udev;
	bool last;
	int i;

	mutex_lock(&lkpi_usb_lock);
	list_for_each_entry(udev, &lkpi_usb_devices, lkpi_link) {
		for (i = 0; i < udev->lkpi_ninterfaces; i++) {
			if (udev->lkpi_interfaces[i].lkpi_driver == driver)
				lkpi_usb_unbind(&udev->lkpi_interfaces[i]);
		}
	}
	list_del(&driver->lkpi_link);
	last = list_empty(&lkpi_usb_drivers);
	/* Haiku's match list is never narrowed while running: it re-checks
	 * it on removal and would skip device_removed() for devices that no
	 * longer match (Device::ReportDevice). */
	mutex_unlock(&lkpi_usb_lock);

	if (last && lkpi_usb_started) {
		lkpi_usb_started = false;
		lkpi_hk_usb_stop();	/* removes the remaining devices */
	}
	if (last && lkpi_usb_wq != NULL) {
		destroy_workqueue(lkpi_usb_wq);	/* runs pending cancels */
		lkpi_usb_wq = NULL;
	}
}

/* After every module_init(): start hearing about devices. Haiku reports
 * the present ones from inside this call (install_notify). */
static void
lkpi_usb_start(void *arg __unused)
{
	struct lkpi_hk_match *m;
	size_t n;

	mutex_lock(&lkpi_usb_lock);
	m = lkpi_usb_matches(&n);
	mutex_unlock(&lkpi_usb_lock);
	if (m == NULL)
		return;
	lkpi_usb_wq = alloc_ordered_workqueue("lkpi_usb", 0);
	lkpi_usb_started = true;
	if (lkpi_hk_usb_start(gDriverName, m, n) != LKPI_HK_OK)
		lkpi_usb_started = false;
	kfree(m);
}
SYSINIT(lkpi_usb_start, SI_SUB_LAST, SI_ORDER_ANY, lkpi_usb_start, NULL);

/* ---- reset / alt settings ------------------------------------------------ */

/*
 * Not supported yet. Haiku's reset_port() only resets the port
 * (Hub::ResetPort): the device comes back at address 0 while Haiku keeps
 * using its old address, and a reset of a device that stays connected
 * raises no connection change, so nothing re-enumerates it. Resetting
 * would leave the controller unreachable until it is unplugged, so the
 * request fails instead, and the driver keeps a working device (btusb and
 * btmtk log the failure and carry on). Lifting this needs a verified way
 * to have Haiku re-enumerate a port (docs/BLUETOOTH.md, roadmap).
 */
int
usb_reset_device(struct usb_device *udev)
{
	if (udev->state == USB_STATE_NOTATTACHED)
		return (-ENODEV);
	dev_warn(&udev->dev, "USB device reset is not supported on Haiku "
	    "yet\n");
	return (-EIO);
}

static void
lkpi_usb_reset_work(struct work_struct *work)
{
	struct usb_interface *intf = container_of(work, struct usb_interface,
	    reset_ws);

	usb_reset_device(interface_to_usbdev(intf));
}

void
usb_queue_reset_device(struct usb_interface *intf)
{
	schedule_work(&intf->reset_ws);	/* INIT_WORK in lkpi_usb_build() */
}

/* Kill every URB submitted on the endpoints of intf's current alt
 * setting (Linux usb_disable_interface()); endpoints already disabled. */
static void
lkpi_usb_flush_alt(struct usb_interface *intf)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct urb *urb;
	int e;

	for (e = 0; e < alt->desc.bNumEndpoints; e++) {
		struct usb_host_endpoint *ep = &alt->endpoint[e];

		for (;;) {
			spin_lock(&lkpi_usb_ep_lock);
			urb = list_first_entry_or_null(&ep->urb_list,
			    struct urb, urb_list);
			if (urb != NULL)
				usb_get_urb(urb);
			spin_unlock(&lkpi_usb_ep_lock);
			if (urb == NULL)
				break;
			usb_kill_urb(urb);
			usb_put_urb(urb);
		}
	}
}

int
usb_set_interface(struct usb_device *udev, int ifnum, int alternate)
{
	struct usb_interface *intf = usb_ifnum_to_if(udev, ifnum);
	struct usb_host_interface *alt;
	struct lkpi_hk_dev *hk;
	int idx, r;

	if (intf == NULL)
		return (-EINVAL);
	alt = usb_altnum_to_altsetting(intf, alternate);
	if (alt == NULL)
		return (-EINVAL);
	idx = alt - intf->altsetting;
	hk = lkpi_usb_hk_get(udev);
	if (hk == NULL)
		return (-ENODEV);

	/* Haiku deletes the old alt's pipes and drops their transfers
	 * without completing them: finish ours first, as Linux does */
	lkpi_usb_map_endpoints(udev, intf, false);
	lkpi_usb_flush_alt(intf);
	r = lkpi_hk_set_alt(hk, (int)(intf - udev->lkpi_interfaces), idx);
	if (r == LKPI_HK_OK)
		intf->cur_altsetting = alt;
	lkpi_usb_map_endpoints(udev, intf, true);
	lkpi_usb_hk_put(udev);
	return (lkpi_usb_errno(r));
}

int
usb_clear_halt(struct usb_device *udev, int pipe)
{
	struct usb_host_endpoint *ep = usb_pipe_endpoint(udev, pipe);
	struct lkpi_hk_dev *hk;
	int r;

	if (ep == NULL)
		return (-EINVAL);
	hk = lkpi_usb_hk_get(udev);
	if (hk == NULL)
		return (-ENODEV);
	r = lkpi_hk_clear_halt(hk, lkpi_hk_pipe(hk,
	    ep->desc.bEndpointAddress));
	lkpi_usb_hk_put(udev);
	return (lkpi_usb_errno(r));
}

/* ---- URBs ----------------------------------------------------------------- */

void
usb_init_urb(struct urb *urb)
{
	if (urb == NULL)
		return;
	memset(urb, 0, sizeof(*urb));
	kref_init(&urb->kref);
	INIT_LIST_HEAD(&urb->urb_list);
	INIT_LIST_HEAD(&urb->anchor_list);
}

struct urb *
usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
	struct urb *urb;

	urb = kmalloc(struct_size(urb, iso_frame_desc, iso_packets),
	    mem_flags);
	if (urb == NULL)
		return (NULL);
	usb_init_urb(urb);
	return (urb);
}

static void
lkpi_urb_release(struct kref *kref)
{
	struct urb *urb = container_of(kref, struct urb, kref);

	if (urb->transfer_flags & URB_FREE_BUFFER)
		kfree(urb->transfer_buffer);
	kfree(urb);
}

void
usb_free_urb(struct urb *urb)
{
	if (urb != NULL)
		kref_put(&urb->kref, lkpi_urb_release);
}

struct urb *
usb_get_urb(struct urb *urb)
{
	if (urb != NULL)
		kref_get(&urb->kref);
	return (urb);
}

static void lkpi_usb_done(void *cookie, int hk, size_t actual);

/* Hand the URB's current stage to Haiku. */
static int
lkpi_usb_queue(struct urb *urb)
{
	struct usb_device *udev = urb->dev;
	struct lkpi_hk_dev *hk;
	void *pipe;
	int r;

	hk = lkpi_usb_hk_get(udev);
	if (hk == NULL)
		return (-ENODEV);
	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:
		if (urb->setup_packet == NULL) {
			r = LKPI_HK_INVAL;
			break;
		}
		r = lkpi_hk_queue_control(hk, urb->setup_packet,
		    urb->transfer_buffer, lkpi_usb_done, urb);
		break;
	case PIPE_BULK:
	case PIPE_INTERRUPT:
		pipe = lkpi_hk_pipe(hk, urb->ep->desc.bEndpointAddress);
		if (pipe == NULL) {
			lkpi_usb_hk_put(udev);
			return (-ENOENT);	/* not in the active alt */
		}
		if (urb->lkpi_xfer == LKPI_URB_ZLP)
			r = lkpi_hk_queue(hk, pipe, LKPI_HK_BULK, NULL, 0,
			    lkpi_usb_done, urb);
		else
			r = lkpi_hk_queue(hk, pipe,
			    usb_pipebulk(urb->pipe) ? LKPI_HK_BULK : LKPI_HK_INTR,
			    urb->transfer_buffer, urb->transfer_buffer_length,
			    lkpi_usb_done, urb);
		break;
	default:
		r = LKPI_HK_UNSUPP;	/* isochronous: see top of file */
		break;
	}
	lkpi_usb_hk_put(udev);
	if (r == LKPI_HK_GONE)
		return (-ENODEV);
	return (lkpi_usb_errno(r));
}

static void
lkpi_usb_anchor_suspend(struct usb_anchor *anchor)
{
	if (anchor != NULL)
		atomic_inc(&anchor->suspend_wakeups);
}

static void
lkpi_usb_anchor_resume(struct usb_anchor *anchor)
{
	if (anchor != NULL && atomic_dec_and_test(&anchor->suspend_wakeups) &&
	    list_empty(&anchor->urb_list))
		wake_up(&anchor->wait);
}

/* Haiku finisher thread: a stage of the URB finished. */
static void
lkpi_usb_done(void *cookie, int hk, size_t actual)
{
	struct urb *urb = cookie;
	struct usb_device *udev = urb->dev;
	struct usb_anchor *anchor;
	int status;

	/* sibling of an unlinked URB on the same pipe: IN is safe to queue
	 * again (no data was delivered to it) */
	if (hk == LKPI_HK_CANCELED && urb->unlinked == 0 &&
	    udev->state != USB_STATE_NOTATTACHED && usb_urb_dir_in(urb) &&
	    !usb_pipecontrol(urb->pipe) && atomic_read(&urb->reject) == 0 &&
	    lkpi_usb_queue(urb) == 0)
		return;

	if (urb->lkpi_xfer == LKPI_URB_ZLP) {
		/* the data stage's length stands; only errors count */
		urb->lkpi_xfer = LKPI_URB_DATA;
	} else {
		urb->actual_length = actual;
		/* bulk OUT ending on a packet boundary: send the ZLP */
		if (hk == LKPI_HK_OK && urb->unlinked == 0 &&
		    (urb->transfer_flags & URB_ZERO_PACKET) &&
		    usb_pipebulk(urb->pipe) && usb_pipeout(urb->pipe) &&
		    urb->transfer_buffer_length > 0 &&
		    urb->transfer_buffer_length %
		    usb_endpoint_maxp(&urb->ep->desc) == 0) {
			urb->lkpi_xfer = LKPI_URB_ZLP;
			if (lkpi_usb_queue(urb) == 0)
				return;
			urb->lkpi_xfer = LKPI_URB_DATA;
		}
	}

	if (urb->unlinked != 0)
		status = urb->unlinked;
	else if (udev->state == USB_STATE_NOTATTACHED &&
	    (hk == LKPI_HK_CANCELED || hk == LKPI_HK_GONE))
		status = -ESHUTDOWN;
	else if (hk == LKPI_HK_SHORT)
		status = 0;		/* short is an error only if asked: */
	else
		status = lkpi_usb_errno(hk);
	if (hk == LKPI_HK_CANCELED && urb->unlinked == 0 &&
	    udev->state != USB_STATE_NOTATTACHED)
		printf("%s: LinuxKPI USB: OUT/control transfer cancelled "
		    "with a sibling URB (pipe %#x)\n", gDriverName, urb->pipe);
	if (status == 0 && (urb->transfer_flags & URB_SHORT_NOT_OK) &&
	    urb->actual_length < urb->transfer_buffer_length)
		status = -EREMOTEIO;

	spin_lock(&lkpi_usb_ep_lock);
	list_del_init(&urb->urb_list);
	spin_unlock(&lkpi_usb_ep_lock);
	urb->hcpriv = NULL;
	anchor = urb->anchor;
	lkpi_usb_anchor_suspend(anchor);
	usb_unanchor_urb(urb);
	urb->status = status;
	(*urb->complete)(urb);	/* not complete(): LinuxKPI macro */
	lkpi_usb_anchor_resume(anchor);
	atomic_dec(&urb->use_count);
	if (atomic_dec_and_test(&udev->lkpi_inflight) ||
	    atomic_read(&urb->reject) != 0)
		wake_up_all(&lkpi_usb_kill_wq);
	usb_put_urb(urb);
}

static void lkpi_usb_cancel_later(struct urb *urb);

int
usb_submit_urb(struct urb *urb, gfp_t mem_flags)
{
	struct usb_device *udev;
	struct usb_host_endpoint *ep;
	static const int xfer_to_pipe[4] = {
		[USB_ENDPOINT_XFER_CONTROL] = PIPE_CONTROL,
		[USB_ENDPOINT_XFER_ISOC] = PIPE_ISOCHRONOUS,
		[USB_ENDPOINT_XFER_BULK] = PIPE_BULK,
		[USB_ENDPOINT_XFER_INT] = PIPE_INTERRUPT,
	};
	int error;

	if (urb == NULL || urb->complete == NULL)
		return (-EINVAL);
	/* hcpriv only: a completion handler resubmitting its own URB runs
	 * while use_count is still raised (Linux urb.c does the same) */
	if (urb->hcpriv != NULL)
		return (-EBUSY);
	udev = urb->dev;
	if (udev == NULL || udev->state == USB_STATE_NOTATTACHED)
		return (-ENODEV);
	ep = usb_pipe_endpoint(udev, urb->pipe);
	if (ep == NULL || !ep->enabled)
		return (-ENOENT);
	if (xfer_to_pipe[usb_endpoint_type(&ep->desc)] !=
	    (int)usb_pipetype(urb->pipe)) {
		/* Linux warns ("BOGUS urb xfer") and goes on for bulk vs
		 * interrupt; we cannot queue to the wrong pipe kind */
		if (!(usb_endpoint_xfer_int(&ep->desc) &&
		    usb_pipebulk(urb->pipe)))
			return (-EINVAL);
	}

	urb->ep = ep;
	urb->status = -EINPROGRESS;
	urb->actual_length = 0;
	urb->unlinked = 0;
	urb->error_count = 0;
	urb->lkpi_xfer = LKPI_URB_DATA;
	/* count first, then test reject: usb_kill_urb() raises reject, then
	 * waits for use_count, so one of the two always sees the other */
	usb_get_urb(urb);
	atomic_inc(&urb->use_count);
	atomic_inc(&udev->lkpi_inflight);
	urb->hcpriv = urb;
	spin_lock(&lkpi_usb_ep_lock);
	list_add_tail(&urb->urb_list, &ep->urb_list);
	spin_unlock(&lkpi_usb_ep_lock);

	if (atomic_read(&urb->reject) != 0)
		error = -EPERM;
	else
		error = lkpi_usb_queue(urb);
	if (error != 0) {
		spin_lock(&lkpi_usb_ep_lock);
		list_del_init(&urb->urb_list);
		spin_unlock(&lkpi_usb_ep_lock);
		urb->hcpriv = NULL;
		atomic_dec(&urb->use_count);
		if (atomic_dec_and_test(&udev->lkpi_inflight) ||
		    atomic_read(&urb->reject) != 0)
			wake_up_all(&lkpi_usb_kill_wq);
		usb_put_urb(urb);
		return (error);
	}
	/* a kill that started between the test and the queueing may have
	 * cancelled the pipe before the transfer reached it: cancel again */
	if (atomic_read(&urb->reject) != 0)
		lkpi_usb_cancel_later(urb);
	return (0);
}

/* Cancel urb's pipe (Haiku cancels per pipe; see top of file). */
static void
lkpi_usb_cancel_now(struct urb *urb)
{
	struct usb_device *udev = urb->dev;
	struct lkpi_hk_dev *hk;

	hk = lkpi_usb_hk_get(udev);
	if (hk == NULL)
		return;		/* gone: removal cancels everything */
	if (usb_pipecontrol(urb->pipe))
		lkpi_hk_cancel_control(hk);
	else if (urb->ep != NULL)
		lkpi_hk_cancel(hk, lkpi_hk_pipe(hk,
		    urb->ep->desc.bEndpointAddress));
	lkpi_usb_hk_put(udev);
}

/*
 * Haiku may run the cancelled transfers' completions in the cancelling
 * thread. usb_unlink_urb() is allowed in atomic context, under driver
 * locks and from another completion handler, where running completions
 * inline could deadlock or recurse; so it cancels from a work item.
 */
struct lkpi_usb_cancel {
	struct work_struct	work;
	struct urb		*urb;
	struct usb_device	*udev;
};

static void
lkpi_usb_cancel_work(struct work_struct *work)
{
	struct lkpi_usb_cancel *c = container_of(work, struct lkpi_usb_cancel,
	    work);

	if (atomic_read(&c->urb->use_count) != 0)
		lkpi_usb_cancel_now(c->urb);
	usb_put_urb(c->urb);
	usb_put_dev(c->udev);
	kfree(c);
}

static void
lkpi_usb_cancel_later(struct urb *urb)
{
	struct lkpi_usb_cancel *c;

	c = (lkpi_usb_wq != NULL) ? kmalloc(sizeof(*c), GFP_ATOMIC) : NULL;
	if (c == NULL) {
		lkpi_usb_cancel_now(urb);	/* best effort */
		return;
	}
	c->urb = usb_get_urb(urb);
	c->udev = usb_get_dev(urb->dev);
	INIT_WORK(&c->work, lkpi_usb_cancel_work);
	queue_work(lkpi_usb_wq, &c->work);
}

static int
lkpi_usb_unlink(struct urb *urb, int status, bool sync)
{
	if (atomic_read(&urb->use_count) == 0)
		return (-EIDRM);
	if (urb->unlinked != 0)
		return (-EBUSY);
	urb->unlinked = status;
	if (sync)
		lkpi_usb_cancel_now(urb);
	else
		lkpi_usb_cancel_later(urb);
	return (0);
}

int
usb_unlink_urb(struct urb *urb)
{
	if (urb == NULL)
		return (-EINVAL);
	if (urb->dev == NULL)
		return (-ENODEV);
	if (urb->ep == NULL)
		return (-EIDRM);
	return (lkpi_usb_unlink(urb, -ECONNRESET, false));
}

void
usb_kill_urb(struct urb *urb)
{
	might_sleep();
	if (urb == NULL || urb->dev == NULL || urb->ep == NULL)
		return;
	atomic_inc(&urb->reject);
	lkpi_usb_unlink(urb, -ENOENT, true);
	wait_event(lkpi_usb_kill_wq, atomic_read(&urb->use_count) == 0);
	atomic_dec(&urb->reject);
}

void
usb_poison_urb(struct urb *urb)
{
	might_sleep();
	if (urb == NULL)
		return;
	atomic_inc(&urb->reject);
	if (urb->dev == NULL || urb->ep == NULL)
		return;
	lkpi_usb_unlink(urb, -ENOENT, true);
	wait_event(lkpi_usb_kill_wq, atomic_read(&urb->use_count) == 0);
}

void
usb_unpoison_urb(struct urb *urb)
{
	if (urb != NULL)
		atomic_dec(&urb->reject);
}

/* ---- anchors -------------------------------------------------------------- */

void
usb_anchor_urb(struct urb *urb, struct usb_anchor *anchor)
{
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	usb_get_urb(urb);
	list_add_tail(&urb->anchor_list, &anchor->urb_list);
	urb->anchor = anchor;
	if (anchor->poisoned)
		atomic_inc(&urb->reject);
	spin_unlock_irqrestore(&anchor->lock, flags);
}

static bool
lkpi_usb_anchor_idle(struct usb_anchor *anchor)
{
	return (atomic_read(&anchor->suspend_wakeups) == 0 &&
	    list_empty(&anchor->urb_list));
}

/* anchor->lock held */
static void
lkpi_usb_unanchor_locked(struct urb *urb, struct usb_anchor *anchor)
{
	urb->anchor = NULL;
	list_del_init(&urb->anchor_list);
	usb_put_urb(urb);
	if (lkpi_usb_anchor_idle(anchor))
		wake_up(&anchor->wait);
}

void
usb_unanchor_urb(struct urb *urb)
{
	struct usb_anchor *anchor;
	unsigned long flags;

	if (urb == NULL || (anchor = urb->anchor) == NULL)
		return;
	spin_lock_irqsave(&anchor->lock, flags);
	if (urb->anchor == anchor)	/* not unanchored meanwhile */
		lkpi_usb_unanchor_locked(urb, anchor);
	spin_unlock_irqrestore(&anchor->lock, flags);
}

void
usb_kill_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	bool idle;

	do {
		spin_lock_irq(&anchor->lock);
		while (!list_empty(&anchor->urb_list)) {
			victim = list_last_entry(&anchor->urb_list,
			    struct urb, anchor_list);
			usb_get_urb(victim);
			spin_unlock_irq(&anchor->lock);
			/* not submitted: kill would not unanchor it */
			if (atomic_read(&victim->use_count) == 0)
				usb_unanchor_urb(victim);
			else
				usb_kill_urb(victim);	/* unanchors */
			usb_put_urb(victim);
			spin_lock_irq(&anchor->lock);
		}
		idle = lkpi_usb_anchor_idle(anchor);
		spin_unlock_irq(&anchor->lock);
		if (!idle)
			cpu_relax();
	} while (!idle);
}

void
usb_poison_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	bool idle;

	do {
		spin_lock_irq(&anchor->lock);
		anchor->poisoned = 1;
		while (!list_empty(&anchor->urb_list)) {
			victim = list_last_entry(&anchor->urb_list,
			    struct urb, anchor_list);
			usb_get_urb(victim);
			spin_unlock_irq(&anchor->lock);
			if (atomic_read(&victim->use_count) == 0) {
				atomic_inc(&victim->reject);
				usb_unanchor_urb(victim);
			} else
				usb_poison_urb(victim);
			usb_put_urb(victim);
			spin_lock_irq(&anchor->lock);
		}
		idle = lkpi_usb_anchor_idle(anchor);
		spin_unlock_irq(&anchor->lock);
		if (!idle)
			cpu_relax();
	} while (!idle);
}

void
usb_unpoison_anchored_urbs(struct usb_anchor *anchor)
{
	unsigned long flags;
	struct urb *lazarus;

	spin_lock_irqsave(&anchor->lock, flags);
	list_for_each_entry(lazarus, &anchor->urb_list, anchor_list)
		usb_unpoison_urb(lazarus);
	anchor->poisoned = 0;
	spin_unlock_irqrestore(&anchor->lock, flags);
}

void
usb_unlink_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;

	while ((victim = usb_get_from_anchor(anchor)) != NULL) {
		usb_unlink_urb(victim);
		usb_put_urb(victim);
	}
}

int
usb_wait_anchor_empty_timeout(struct usb_anchor *anchor, unsigned int timeout)
{
	return (wait_event_timeout(anchor->wait, lkpi_usb_anchor_idle(anchor),
	    msecs_to_jiffies(timeout)));
}

struct urb *
usb_get_from_anchor(struct usb_anchor *anchor)
{
	struct urb *victim = NULL;
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	if (!list_empty(&anchor->urb_list)) {
		victim = list_first_entry(&anchor->urb_list, struct urb,
		    anchor_list);
		usb_get_urb(victim);
		lkpi_usb_unanchor_locked(victim, anchor);
	}
	spin_unlock_irqrestore(&anchor->lock, flags);
	return (victim);
}

void
usb_scuttle_anchored_urbs(struct usb_anchor *anchor)
{
	unsigned long flags;
	bool idle;

	do {
		spin_lock_irqsave(&anchor->lock, flags);
		while (!list_empty(&anchor->urb_list))
			lkpi_usb_unanchor_locked(list_last_entry(
			    &anchor->urb_list, struct urb, anchor_list), anchor);
		idle = lkpi_usb_anchor_idle(anchor);
		spin_unlock_irqrestore(&anchor->lock, flags);
		if (!idle)
			cpu_relax();
	} while (!idle);
}

int
usb_anchor_empty(struct usb_anchor *anchor)
{
	return (list_empty(&anchor->urb_list));
}

/* ---- synchronous messages ------------------------------------------------- */

static void
lkpi_usb_blocking_done(struct urb *urb)
{
	complete((struct completion *)urb->context);
}

/* Submit and wait; returns 0 or a negative errno, *actual = bytes. */
static int
lkpi_usb_start_wait_urb(struct urb *urb, int timeout, int *actual)
{
	struct completion done;
	unsigned long expire;
	int r;

	init_completion(&done);
	urb->context = &done;
	urb->actual_length = 0;
	r = usb_submit_urb(urb, GFP_NOIO);
	if (r != 0)
		goto out;
	expire = timeout ? msecs_to_jiffies(timeout) : MAX_SCHEDULE_TIMEOUT;
	if (!wait_for_completion_timeout(&done, expire)) {
		usb_kill_urb(urb);
		r = (urb->status == -ENOENT) ? -ETIMEDOUT : urb->status;
	} else
		r = urb->status;
out:
	if (actual != NULL)
		*actual = urb->actual_length;
	usb_free_urb(urb);
	return (r);
}

int
usb_control_msg(struct usb_device *dev, unsigned int pipe, __u8 request,
    __u8 requesttype, __u16 value, __u16 index, void *data, __u16 size,
    int timeout)
{
	struct usb_ctrlrequest *dr;
	struct urb *urb;
	int r, len;

	dr = kmalloc(sizeof(*dr), GFP_NOIO);
	if (dr == NULL)
		return (-ENOMEM);
	dr->bRequestType = requesttype;
	dr->bRequest = request;
	dr->wValue = cpu_to_le16(value);
	dr->wIndex = cpu_to_le16(index);
	dr->wLength = cpu_to_le16(size);

	urb = usb_alloc_urb(0, GFP_NOIO);
	if (urb == NULL) {
		kfree(dr);
		return (-ENOMEM);
	}
	usb_fill_control_urb(urb, dev, pipe, (unsigned char *)dr, data, size,
	    lkpi_usb_blocking_done, NULL);
	r = lkpi_usb_start_wait_urb(urb, timeout, &len);
	kfree(dr);
	return (r < 0 ? r : len);
}

int
usb_control_msg_send(struct usb_device *dev, __u8 endpoint, __u8 request,
    __u8 requesttype, __u16 value, __u16 index, const void *driver_data,
    __u16 size, int timeout, gfp_t memflags)
{
	unsigned int pipe = usb_sndctrlpipe(dev, endpoint);
	void *data = NULL;
	int r;

	if (size > 0) {
		data = kmemdup(driver_data, size, memflags);
		if (data == NULL)
			return (-ENOMEM);
	}
	r = usb_control_msg(dev, pipe, request, requesttype, value, index,
	    data, size, timeout);
	kfree(data);
	return (r < 0 ? r : 0);
}

int
usb_control_msg_recv(struct usb_device *dev, __u8 endpoint, __u8 request,
    __u8 requesttype, __u16 value, __u16 index, void *driver_data,
    __u16 size, int timeout, gfp_t memflags)
{
	unsigned int pipe = usb_rcvctrlpipe(dev, endpoint);
	void *data;
	int r;

	if (size == 0 || driver_data == NULL)
		return (-EINVAL);
	data = kmalloc(size, memflags);
	if (data == NULL)
		return (-ENOMEM);
	r = usb_control_msg(dev, pipe, request, requesttype, value, index,
	    data, size, timeout);
	if (r == size) {
		memcpy(driver_data, data, size);
		r = 0;
	} else if (r >= 0)
		r = -EREMOTEIO;		/* short: Linux's contract */
	kfree(data);
	return (r);
}

static int
lkpi_usb_xfer_msg(struct usb_device *dev, unsigned int pipe, void *data,
    int len, int *actual, int timeout)
{
	struct usb_host_endpoint *ep = usb_pipe_endpoint(dev, pipe);
	struct urb *urb;

	if (ep == NULL)
		return (-EINVAL);
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (urb == NULL)
		return (-ENOMEM);
	if (usb_endpoint_xfer_int(&ep->desc)) {
		/* Linux: interrupt endpoints get interrupt URBs */
		pipe = (pipe & ~(3U << 30)) | (PIPE_INTERRUPT << 30);
		usb_fill_int_urb(urb, dev, pipe, data, len,
		    lkpi_usb_blocking_done, NULL, ep->desc.bInterval);
	} else
		usb_fill_bulk_urb(urb, dev, pipe, data, len,
		    lkpi_usb_blocking_done, NULL);
	return (lkpi_usb_start_wait_urb(urb, timeout, actual));
}

int
usb_bulk_msg(struct usb_device *dev, unsigned int pipe, void *data, int len,
    int *actual_length, int timeout)
{
	return (lkpi_usb_xfer_msg(dev, pipe, data, len, actual_length,
	    timeout));
}

int
usb_interrupt_msg(struct usb_device *dev, unsigned int pipe, void *data,
    int len, int *actual_length, int timeout)
{
	return (lkpi_usb_xfer_msg(dev, pipe, data, len, actual_length,
	    timeout));
}

int
usb_get_descriptor(struct usb_device *dev, unsigned char type,
    unsigned char index, void *buf, int size)
{
	int i, r = -EINVAL;

	memset(buf, 0, size);
	for (i = 0; i < 3; i++) {	/* Linux retries 3 times */
		r = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
		    USB_REQ_GET_DESCRIPTOR, USB_DIR_IN, (type << 8) + index, 0,
		    buf, size, USB_CTRL_GET_TIMEOUT);
		if (r <= 0 && r != -ETIMEDOUT)
			continue;
		if (r > 1 && ((u8 *)buf)[1] != type) {
			r = -ENODATA;
			continue;
		}
		break;
	}
	return (r);
}

int
usb_string(struct usb_device *dev, int index, char *buf, size_t size)
{
	u8 *tbuf;
	u16 langid;
	int r, i, n;

	if (size <= 0 || buf == NULL)
		return (-EINVAL);
	buf[0] = 0;
	if (index <= 0 || index >= 256)
		return (-EINVAL);
	tbuf = kmalloc(256, GFP_NOIO);
	if (tbuf == NULL)
		return (-ENOMEM);
	/* language list: index 0, first entry */
	r = usb_get_descriptor(dev, USB_DT_STRING, 0, tbuf, 255);
	if (r < 4) {
		kfree(tbuf);
		return (r < 0 ? r : -EINVAL);
	}
	langid = tbuf[2] | (tbuf[3] << 8);
	r = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), USB_REQ_GET_DESCRIPTOR,
	    USB_DIR_IN, (USB_DT_STRING << 8) + index, langid, tbuf, 255,
	    USB_CTRL_GET_TIMEOUT);
	if (r < 2) {
		kfree(tbuf);
		return (r < 0 ? r : -EINVAL);
	}
	/* UTF-16LE -> ASCII ('?' for the rest), as Linux's usb_string() */
	n = min_t(int, r, tbuf[0]);
	for (i = 0; 2 + 2 * i + 1 < n && (size_t)i < size - 1; i++) {
		u16 c = tbuf[2 + 2 * i] | (tbuf[3 + 2 * i] << 8);

		buf[i] = c < 0x80 ? (char)c : '?';
	}
	buf[i] = 0;
	kfree(tbuf);
	return (i);
}
