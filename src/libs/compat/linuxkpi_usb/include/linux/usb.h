/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux USB driver API (<linux/usb.h>) for LinuxKPI on Haiku.
 *
 * FreeBSD's LinuxKPI ships its own linux/usb.h, but it is the older
 * FreeBSD-flavoured API (usb_control_msg() takes an endpoint pointer, not a
 * pipe) built on FreeBSD's internal USB structures. Current Linux drivers
 * (btusb, mt76 USB, rtw88 USB, ...) use the Linux API, so this subsystem
 * provides that API directly on Haiku's public USB bus manager
 * (bus_managers/usb/v3, <USB3.h>): see linuxkpi_usb/lkpi_usb.c and
 * lkpi_usb_haiku.c. This header replaces FreeBSD's for drivers built with
 * "UseLinuxKPI <version> : usb ;".
 *
 * Coverage is driven by real drivers and recorded in docs/BLUETOOTH.md;
 * anything not implemented is absent (compile error), not a silent stub.
 */
#ifndef _LKPI_LINUX_USB_H_
#define _LKPI_LINUX_USB_H_

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/pm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/ch9.h>

struct usb_device;
struct usb_driver;
struct urb;

/* -------------------------------------------------------------------- */
/* Device ID matching (<linux/mod_devicetable.h> struct usb_device_id)   */

#define	USB_DEVICE_ID_MATCH_VENDOR		0x0001
#define	USB_DEVICE_ID_MATCH_PRODUCT		0x0002
#define	USB_DEVICE_ID_MATCH_DEV_LO		0x0004
#define	USB_DEVICE_ID_MATCH_DEV_HI		0x0008
#define	USB_DEVICE_ID_MATCH_DEV_CLASS		0x0010
#define	USB_DEVICE_ID_MATCH_DEV_SUBCLASS	0x0020
#define	USB_DEVICE_ID_MATCH_DEV_PROTOCOL	0x0040
#define	USB_DEVICE_ID_MATCH_INT_CLASS		0x0080
#define	USB_DEVICE_ID_MATCH_INT_SUBCLASS	0x0100
#define	USB_DEVICE_ID_MATCH_INT_PROTOCOL	0x0200
#define	USB_DEVICE_ID_MATCH_INT_NUMBER		0x0400

#define	USB_DEVICE_ID_MATCH_DEVICE					\
	(USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT)
#define	USB_DEVICE_ID_MATCH_DEV_RANGE					\
	(USB_DEVICE_ID_MATCH_DEV_LO | USB_DEVICE_ID_MATCH_DEV_HI)
#define	USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION				\
	(USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_DEV_RANGE)
#define	USB_DEVICE_ID_MATCH_DEV_INFO					\
	(USB_DEVICE_ID_MATCH_DEV_CLASS | USB_DEVICE_ID_MATCH_DEV_SUBCLASS | \
	 USB_DEVICE_ID_MATCH_DEV_PROTOCOL)
#define	USB_DEVICE_ID_MATCH_INT_INFO					\
	(USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS | \
	 USB_DEVICE_ID_MATCH_INT_PROTOCOL)

struct usb_device_id {
	__u16		match_flags;
	__u16		idVendor;
	__u16		idProduct;
	__u16		bcdDevice_lo;
	__u16		bcdDevice_hi;
	__u8		bDeviceClass;
	__u8		bDeviceSubClass;
	__u8		bDeviceProtocol;
	__u8		bInterfaceClass;
	__u8		bInterfaceSubClass;
	__u8		bInterfaceProtocol;
	__u8		bInterfaceNumber;
	unsigned long	driver_info;
};

#define	USB_DEVICE(vend, prod)						\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE,			\
	.idVendor = (vend), .idProduct = (prod)
#define	USB_DEVICE_VER(vend, prod, lo, hi)				\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION,		\
	.idVendor = (vend), .idProduct = (prod),			\
	.bcdDevice_lo = (lo), .bcdDevice_hi = (hi)
#define	USB_DEVICE_INTERFACE_CLASS(vend, prod, cl)			\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE |			\
	    USB_DEVICE_ID_MATCH_INT_CLASS,				\
	.idVendor = (vend), .idProduct = (prod), .bInterfaceClass = (cl)
#define	USB_DEVICE_INTERFACE_PROTOCOL(vend, prod, pr)			\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE |			\
	    USB_DEVICE_ID_MATCH_INT_PROTOCOL,				\
	.idVendor = (vend), .idProduct = (prod), .bInterfaceProtocol = (pr)
#define	USB_DEVICE_INTERFACE_NUMBER(vend, prod, num)			\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE |			\
	    USB_DEVICE_ID_MATCH_INT_NUMBER,				\
	.idVendor = (vend), .idProduct = (prod), .bInterfaceNumber = (num)
#define	USB_DEVICE_INFO(cl, sc, pr)					\
	.match_flags = USB_DEVICE_ID_MATCH_DEV_INFO,			\
	.bDeviceClass = (cl), .bDeviceSubClass = (sc),			\
	.bDeviceProtocol = (pr)
#define	USB_INTERFACE_INFO(cl, sc, pr)					\
	.match_flags = USB_DEVICE_ID_MATCH_INT_INFO,			\
	.bInterfaceClass = (cl), .bInterfaceSubClass = (sc),		\
	.bInterfaceProtocol = (pr)
#define	USB_DEVICE_AND_INTERFACE_INFO(vend, prod, cl, sc, pr)		\
	.match_flags = USB_DEVICE_ID_MATCH_INT_INFO |			\
	    USB_DEVICE_ID_MATCH_DEVICE,					\
	.idVendor = (vend), .idProduct = (prod),			\
	.bInterfaceClass = (cl), .bInterfaceSubClass = (sc),		\
	.bInterfaceProtocol = (pr)
#define	USB_VENDOR_AND_INTERFACE_INFO(vend, cl, sc, pr)			\
	.match_flags = USB_DEVICE_ID_MATCH_INT_INFO |			\
	    USB_DEVICE_ID_MATCH_VENDOR,					\
	.idVendor = (vend),						\
	.bInterfaceClass = (cl), .bInterfaceSubClass = (sc),		\
	.bInterfaceProtocol = (pr)

/* -------------------------------------------------------------------- */
/* Host-side descriptors                                                 */

struct usb_host_endpoint {
	struct usb_endpoint_descriptor	desc;
	unsigned char			*extra;
	int				extralen;
	int				enabled;
	void				*hcpriv;	/* Haiku usb_pipe */
	struct list_head		urb_list;	/* submitted URBs */
};

struct usb_host_interface {
	struct usb_interface_descriptor	desc;
	int				extralen;
	unsigned char			*extra;
	struct usb_host_endpoint	*endpoint;	/* desc.bNumEndpoints */
	char				*string;
};

enum usb_interface_condition {
	USB_INTERFACE_UNBOUND = 0,
	USB_INTERFACE_BINDING,
	USB_INTERFACE_BOUND,
	USB_INTERFACE_UNBINDING,
};

struct usb_interface {
	struct usb_host_interface	*altsetting;	/* num_altsetting */
	struct usb_host_interface	*cur_altsetting;
	unsigned			num_altsetting;
	int				minor;
	enum usb_interface_condition	condition;
	unsigned			needs_remote_wakeup:1;
	unsigned			needs_binding:1;
	struct device			dev;
	struct device			*usb_dev;
	struct work_struct		reset_ws;
	/* LinuxKPI private */
	struct usb_driver		*lkpi_driver;	/* bound driver */
	struct usb_device		*lkpi_udev;
};

struct usb_host_config {
	struct usb_config_descriptor	desc;
	struct usb_interface		*interface[32];	/* by index */
};

struct usb_bus {
	struct usb_device		*root_hub;
	int				busnum;
};

struct usb_device {
	int				devnum;
	char				devpath[16];
	enum usb_device_state		state;
	enum usb_device_speed		speed;
	struct usb_device		*parent;	/* NULL: no Linux view of hubs */
	struct usb_bus			*bus;
	struct usb_host_endpoint	ep0;
	struct device			dev;
	struct usb_device_descriptor	descriptor;
	struct usb_host_config		*actconfig;
	struct usb_host_endpoint	*ep_in[16];
	struct usb_host_endpoint	*ep_out[16];
	unsigned			can_submit:1;
	unsigned			do_remote_wakeup:1;
	unsigned			reset_resume:1;
	unsigned			persist_enabled:1;
	int				maxchild;
	u32				quirks;
	/* LinuxKPI private (lkpi_usb.c) */
	struct kref			lkpi_ref;
	struct list_head		lkpi_link;	/* all devices */
	void				*lkpi_hk;	/* Haiku side */
	atomic_t			lkpi_inflight;	/* URBs queued */
	int				lkpi_hk_users;	/* calls using lkpi_hk */
	int				lkpi_ninterfaces;
	struct usb_interface		*lkpi_interfaces;
	struct usb_host_config		lkpi_config;
};

#define	to_usb_device(d)	container_of(d, struct usb_device, dev)
#define	to_usb_interface(d)	container_of(d, struct usb_interface, dev)

static inline struct usb_device *
interface_to_usbdev(struct usb_interface *intf)
{
	return (intf->lkpi_udev);
}

static inline void *
usb_get_intfdata(struct usb_interface *intf)
{
	return (dev_get_drvdata(&intf->dev));
}

static inline void
usb_set_intfdata(struct usb_interface *intf, void *data)
{
	dev_set_drvdata(&intf->dev, data);
}

struct usb_device *usb_get_dev(struct usb_device *dev);
void	usb_put_dev(struct usb_device *dev);
struct usb_interface *usb_ifnum_to_if(const struct usb_device *dev,
	    unsigned ifnum);
struct usb_host_interface *usb_altnum_to_altsetting(
	    const struct usb_interface *intf, unsigned int altnum);

int	usb_find_common_endpoints(struct usb_host_interface *alt,
	    struct usb_endpoint_descriptor **bulk_in,
	    struct usb_endpoint_descriptor **bulk_out,
	    struct usb_endpoint_descriptor **int_in,
	    struct usb_endpoint_descriptor **int_out);

static inline int
usb_find_bulk_in_endpoint(struct usb_host_interface *alt,
    struct usb_endpoint_descriptor **bulk_in)
{
	return (usb_find_common_endpoints(alt, bulk_in, NULL, NULL, NULL));
}

static inline int
usb_find_bulk_out_endpoint(struct usb_host_interface *alt,
    struct usb_endpoint_descriptor **bulk_out)
{
	return (usb_find_common_endpoints(alt, NULL, bulk_out, NULL, NULL));
}

static inline int
usb_find_int_in_endpoint(struct usb_host_interface *alt,
    struct usb_endpoint_descriptor **int_in)
{
	return (usb_find_common_endpoints(alt, NULL, NULL, int_in, NULL));
}

static inline int
usb_find_int_out_endpoint(struct usb_host_interface *alt,
    struct usb_endpoint_descriptor **int_out)
{
	return (usb_find_common_endpoints(alt, NULL, NULL, NULL, int_out));
}

/* -------------------------------------------------------------------- */
/* Drivers                                                               */

struct usb_driver {
	const char	*name;
	int		(*probe)(struct usb_interface *intf,
			    const struct usb_device_id *id);
	void		(*disconnect)(struct usb_interface *intf);
	int		(*unlocked_ioctl)(struct usb_interface *intf,
			    unsigned int code, void *buf);
	int		(*suspend)(struct usb_interface *intf,
			    pm_message_t message);
	int		(*resume)(struct usb_interface *intf);
	int		(*reset_resume)(struct usb_interface *intf);
	int		(*pre_reset)(struct usb_interface *intf);
	int		(*post_reset)(struct usb_interface *intf);
	void		(*shutdown)(struct usb_interface *intf);
	const struct usb_device_id *id_table;
	struct device_driver	driver;
	unsigned int	no_dynamic_id:1;
	unsigned int	supports_autosuspend:1;
	unsigned int	disable_hub_initiated_lpm:1;
	unsigned int	soft_unbind:1;
	/* LinuxKPI private */
	struct list_head lkpi_link;
};

int	usb_register_driver(struct usb_driver *driver, struct module *owner,
	    const char *mod_name);
#define	usb_register(driver)	usb_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
void	usb_deregister(struct usb_driver *driver);

/* Linux's generic module_driver() is not in LinuxKPI; same expansion. */
#define	module_usb_driver(__usb_driver)					\
static int __init __usb_driver##_lkpi_init(void)			\
{									\
	return (usb_register(&(__usb_driver)));				\
}									\
module_init(__usb_driver##_lkpi_init);					\
static void __exit __usb_driver##_lkpi_exit(void)			\
{									\
	usb_deregister(&(__usb_driver));				\
}									\
module_exit(__usb_driver##_lkpi_exit)

const struct usb_device_id *usb_match_id(struct usb_interface *intf,
	    const struct usb_device_id *id);
int	usb_driver_claim_interface(struct usb_driver *driver,
	    struct usb_interface *intf, void *data);
void	usb_driver_release_interface(struct usb_driver *driver,
	    struct usb_interface *intf);

/* Power management: Haiku's USB stack does not suspend devices; these keep
 * the reference-count semantics drivers rely on and never sleep. */
static inline int usb_autopm_get_interface(struct usb_interface *i) { return (0); }
static inline int usb_autopm_get_interface_async(struct usb_interface *i) { return (0); }
static inline void usb_autopm_get_interface_no_resume(struct usb_interface *i) { }
static inline void usb_autopm_put_interface(struct usb_interface *i) { }
static inline void usb_autopm_put_interface_async(struct usb_interface *i) { }
static inline void usb_autopm_put_interface_no_suspend(struct usb_interface *i) { }
static inline void usb_enable_autosuspend(struct usb_device *d) { }
static inline void usb_disable_autosuspend(struct usb_device *d) { }
static inline void usb_mark_last_busy(struct usb_device *d) { }

int	usb_reset_device(struct usb_device *dev);
void	usb_queue_reset_device(struct usb_interface *intf);
int	usb_set_interface(struct usb_device *dev, int ifnum, int alternate);
int	usb_clear_halt(struct usb_device *dev, int pipe);

/* -------------------------------------------------------------------- */
/* Pipes (same encoding as Linux: type 31..30, endpoint 18..15,           */
/* device 14..8, direction bit 7)                                        */

#define	PIPE_ISOCHRONOUS	0
#define	PIPE_INTERRUPT		1
#define	PIPE_CONTROL		2
#define	PIPE_BULK		3

#define	usb_pipein(pipe)	((pipe) & USB_DIR_IN)
#define	usb_pipeout(pipe)	(!usb_pipein(pipe))
#define	usb_pipedevice(pipe)	(((pipe) >> 8) & 0x7f)
#define	usb_pipeendpoint(pipe)	(((pipe) >> 15) & 0xf)
#define	usb_pipetype(pipe)	(((pipe) >> 30) & 3)
#define	usb_pipeisoc(pipe)	(usb_pipetype((pipe)) == PIPE_ISOCHRONOUS)
#define	usb_pipeint(pipe)	(usb_pipetype((pipe)) == PIPE_INTERRUPT)
#define	usb_pipecontrol(pipe)	(usb_pipetype((pipe)) == PIPE_CONTROL)
#define	usb_pipebulk(pipe)	(usb_pipetype((pipe)) == PIPE_BULK)

static inline unsigned int
__create_pipe(struct usb_device *dev, unsigned int endpoint)
{
	return ((dev->devnum << 8) | (endpoint << 15));
}

#define	usb_sndctrlpipe(dev, ep)	((PIPE_CONTROL << 30) | __create_pipe(dev, ep))
#define	usb_rcvctrlpipe(dev, ep)	((PIPE_CONTROL << 30) | __create_pipe(dev, ep) | USB_DIR_IN)
#define	usb_sndisocpipe(dev, ep)	((PIPE_ISOCHRONOUS << 30) | __create_pipe(dev, ep))
#define	usb_rcvisocpipe(dev, ep)	((PIPE_ISOCHRONOUS << 30) | __create_pipe(dev, ep) | USB_DIR_IN)
#define	usb_sndbulkpipe(dev, ep)	((PIPE_BULK << 30) | __create_pipe(dev, ep))
#define	usb_rcvbulkpipe(dev, ep)	((PIPE_BULK << 30) | __create_pipe(dev, ep) | USB_DIR_IN)
#define	usb_sndintpipe(dev, ep)		((PIPE_INTERRUPT << 30) | __create_pipe(dev, ep))
#define	usb_rcvintpipe(dev, ep)		((PIPE_INTERRUPT << 30) | __create_pipe(dev, ep) | USB_DIR_IN)

static inline struct usb_host_endpoint *
usb_pipe_endpoint(struct usb_device *dev, unsigned int pipe)
{
	struct usb_host_endpoint **eps;

	eps = usb_pipein(pipe) ? dev->ep_in : dev->ep_out;
	return (eps[usb_pipeendpoint(pipe)]);
}

static inline u16
usb_maxpacket(struct usb_device *udev, int pipe)
{
	struct usb_host_endpoint *ep = usb_pipe_endpoint(udev, pipe);

	return (ep != NULL ? usb_endpoint_maxp(&ep->desc) : 0);
}

/* -------------------------------------------------------------------- */
/* URBs                                                                  */

#define	URB_SHORT_NOT_OK	0x0001
#define	URB_ISO_ASAP		0x0002
#define	URB_NO_TRANSFER_DMA_MAP	0x0004
#define	URB_ZERO_PACKET		0x0040
#define	URB_NO_INTERRUPT	0x0080
#define	URB_FREE_BUFFER		0x0100
#define	URB_DIR_IN		0x0200
#define	URB_DIR_OUT		0
#define	URB_DIR_MASK		URB_DIR_IN

struct usb_iso_packet_descriptor {
	unsigned int	offset;
	unsigned int	length;
	unsigned int	actual_length;
	int		status;
};

struct usb_anchor {
	struct list_head	urb_list;
	wait_queue_head_t	wait;
	spinlock_t		lock;
	atomic_t		suspend_wakeups;
	unsigned int		poisoned:1;
};

static inline void
init_usb_anchor(struct usb_anchor *anchor)
{
	memset(anchor, 0, sizeof(*anchor));
	INIT_LIST_HEAD(&anchor->urb_list);
	init_waitqueue_head(&anchor->wait);
	spin_lock_init(&anchor->lock);
}

typedef void (*usb_complete_t)(struct urb *);

struct urb {
	struct kref		kref;
	int			unlinked;
	void			*hcpriv;
	atomic_t		use_count;
	atomic_t		reject;
	struct list_head	urb_list;
	struct list_head	anchor_list;
	struct usb_anchor	*anchor;
	struct usb_device	*dev;
	struct usb_host_endpoint *ep;
	unsigned int		pipe;
	unsigned int		stream_id;
	int			status;
	unsigned int		transfer_flags;
	void			*transfer_buffer;
	dma_addr_t		transfer_dma;
	u32			transfer_buffer_length;
	u32			actual_length;
	unsigned char		*setup_packet;
	dma_addr_t		setup_dma;
	int			start_frame;
	int			number_of_packets;
	int			interval;
	int			error_count;
	void			*context;
	usb_complete_t		complete;
	/* LinuxKPI private: Haiku transfer state (lkpi_usb.c) */
	void			*lkpi_xfer;
	struct usb_iso_packet_descriptor iso_frame_desc[];
};

static inline void
usb_fill_control_urb(struct urb *urb, struct usb_device *dev,
    unsigned int pipe, unsigned char *setup_packet, void *transfer_buffer,
    int buffer_length, usb_complete_t complete_fn, void *context)
{
	urb->dev = dev;
	urb->pipe = pipe;
	urb->setup_packet = setup_packet;
	urb->transfer_buffer = transfer_buffer;
	urb->transfer_buffer_length = buffer_length;
	urb->complete = complete_fn;
	urb->context = context;
}

static inline void
usb_fill_bulk_urb(struct urb *urb, struct usb_device *dev,
    unsigned int pipe, void *transfer_buffer, int buffer_length,
    usb_complete_t complete_fn, void *context)
{
	urb->dev = dev;
	urb->pipe = pipe;
	urb->transfer_buffer = transfer_buffer;
	urb->transfer_buffer_length = buffer_length;
	urb->complete = complete_fn;
	urb->context = context;
}

static inline void
usb_fill_int_urb(struct urb *urb, struct usb_device *dev,
    unsigned int pipe, void *transfer_buffer, int buffer_length,
    usb_complete_t complete_fn, void *context, int interval)
{
	urb->dev = dev;
	urb->pipe = pipe;
	urb->transfer_buffer = transfer_buffer;
	urb->transfer_buffer_length = buffer_length;
	urb->complete = complete_fn;
	urb->context = context;
	/* Linux: high/super speed intervals are 2^(n-1) microframes */
	if (dev->speed == USB_SPEED_HIGH || dev->speed >= USB_SPEED_SUPER) {
		interval = clamp(interval, 1, 16);
		urb->interval = 1 << (interval - 1);
	} else
		urb->interval = interval;
	urb->start_frame = -1;
}

void	usb_init_urb(struct urb *urb);
struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags);
void	usb_free_urb(struct urb *urb);
struct urb *usb_get_urb(struct urb *urb);
int	usb_submit_urb(struct urb *urb, gfp_t mem_flags);
int	usb_unlink_urb(struct urb *urb);
void	usb_kill_urb(struct urb *urb);
void	usb_poison_urb(struct urb *urb);
void	usb_unpoison_urb(struct urb *urb);
#define	usb_put_urb	usb_free_urb

void	usb_anchor_urb(struct urb *urb, struct usb_anchor *anchor);
void	usb_unanchor_urb(struct urb *urb);
void	usb_kill_anchored_urbs(struct usb_anchor *anchor);
void	usb_poison_anchored_urbs(struct usb_anchor *anchor);
void	usb_unpoison_anchored_urbs(struct usb_anchor *anchor);
void	usb_unlink_anchored_urbs(struct usb_anchor *anchor);
int	usb_wait_anchor_empty_timeout(struct usb_anchor *anchor,
	    unsigned int timeout);
struct urb *usb_get_from_anchor(struct usb_anchor *anchor);
void	usb_scuttle_anchored_urbs(struct usb_anchor *anchor);
int	usb_anchor_empty(struct usb_anchor *anchor);

static inline int
usb_urb_dir_in(struct urb *urb)
{
	return (usb_pipein(urb->pipe) != 0);
}

static inline int
usb_urb_dir_out(struct urb *urb)
{
	return (!usb_urb_dir_in(urb));
}

/* -------------------------------------------------------------------- */
/* Synchronous messages (sleep; not from completion handlers)            */

#define	USB_CTRL_GET_TIMEOUT	5000
#define	USB_CTRL_SET_TIMEOUT	5000

int	usb_control_msg(struct usb_device *dev, unsigned int pipe,
	    __u8 request, __u8 requesttype, __u16 value, __u16 index,
	    void *data, __u16 size, int timeout);
int	usb_control_msg_send(struct usb_device *dev, __u8 endpoint,
	    __u8 request, __u8 requesttype, __u16 value, __u16 index,
	    const void *data, __u16 size, int timeout, gfp_t memflags);
int	usb_control_msg_recv(struct usb_device *dev, __u8 endpoint,
	    __u8 request, __u8 requesttype, __u16 value, __u16 index,
	    void *data, __u16 size, int timeout, gfp_t memflags);
int	usb_bulk_msg(struct usb_device *dev, unsigned int pipe, void *data,
	    int len, int *actual_length, int timeout);
int	usb_interrupt_msg(struct usb_device *dev, unsigned int pipe,
	    void *data, int len, int *actual_length, int timeout);
int	usb_get_descriptor(struct usb_device *dev, unsigned char desctype,
	    unsigned char descindex, void *buf, int size);
int	usb_string(struct usb_device *dev, int index, char *buf, size_t size);

#endif /* _LKPI_LINUX_USB_H_ */
