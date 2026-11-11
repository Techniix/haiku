/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * USB 2.0 specification, chapter 9 ("USB Device Framework"): request
 * codes, descriptor layouts and helpers, with the names Linux drivers use
 * (<linux/usb/ch9.h>). Written for LinuxKPI_HaikuOS from the specification;
 * descriptor structures are byte-for-byte the wire format, so they can be
 * copied from Haiku's USB stack, which keeps the same layout (USB_spec.h).
 */
#ifndef _LKPI_LINUX_USB_CH9_H_
#define _LKPI_LINUX_USB_CH9_H_

#include <linux/types.h>
#include <asm/byteorder.h>

/* bmRequestType (USB 2.0 table 9-2) */
#define	USB_DIR_OUT			0x00
#define	USB_DIR_IN			0x80

#define	USB_TYPE_MASK			(0x03 << 5)
#define	USB_TYPE_STANDARD		(0x00 << 5)
#define	USB_TYPE_CLASS			(0x01 << 5)
#define	USB_TYPE_VENDOR			(0x02 << 5)
#define	USB_TYPE_RESERVED		(0x03 << 5)

#define	USB_RECIP_MASK			0x1f
#define	USB_RECIP_DEVICE		0x00
#define	USB_RECIP_INTERFACE		0x01
#define	USB_RECIP_ENDPOINT		0x02
#define	USB_RECIP_OTHER			0x03

/* bRequest (table 9-4) */
#define	USB_REQ_GET_STATUS		0x00
#define	USB_REQ_CLEAR_FEATURE		0x01
#define	USB_REQ_SET_FEATURE		0x03
#define	USB_REQ_SET_ADDRESS		0x05
#define	USB_REQ_GET_DESCRIPTOR		0x06
#define	USB_REQ_SET_DESCRIPTOR		0x07
#define	USB_REQ_GET_CONFIGURATION	0x08
#define	USB_REQ_SET_CONFIGURATION	0x09
#define	USB_REQ_GET_INTERFACE		0x0a
#define	USB_REQ_SET_INTERFACE		0x0b
#define	USB_REQ_SYNCH_FRAME		0x0c

/* feature selectors (table 9-6) */
#define	USB_DEVICE_REMOTE_WAKEUP	1
#define	USB_ENDPOINT_HALT		0

/* descriptor types (table 9-5) */
#define	USB_DT_DEVICE			0x01
#define	USB_DT_CONFIG			0x02
#define	USB_DT_STRING			0x03
#define	USB_DT_INTERFACE		0x04
#define	USB_DT_ENDPOINT			0x05
#define	USB_DT_DEVICE_QUALIFIER		0x06
#define	USB_DT_INTERFACE_ASSOCIATION	0x0b
#define	USB_DT_BOS			0x0f

/* class codes (usb.org) */
#define	USB_CLASS_PER_INTERFACE		0x00
#define	USB_CLASS_AUDIO			0x01
#define	USB_CLASS_COMM			0x02
#define	USB_CLASS_HID			0x03
#define	USB_CLASS_MASS_STORAGE		0x08
#define	USB_CLASS_HUB			0x09
#define	USB_CLASS_WIRELESS_CONTROLLER	0xe0
#define	USB_CLASS_MISC			0xef
#define	USB_CLASS_APP_SPEC		0xfe
#define	USB_CLASS_VENDOR_SPEC		0xff

/* SETUP packet (table 9-2) */
struct usb_ctrlrequest {
	__u8	bRequestType;
	__u8	bRequest;
	__le16	wValue;
	__le16	wIndex;
	__le16	wLength;
} __packed;

/* device descriptor (table 9-8) */
struct usb_device_descriptor {
	__u8	bLength;
	__u8	bDescriptorType;
	__le16	bcdUSB;
	__u8	bDeviceClass;
	__u8	bDeviceSubClass;
	__u8	bDeviceProtocol;
	__u8	bMaxPacketSize0;
	__le16	idVendor;
	__le16	idProduct;
	__le16	bcdDevice;
	__u8	iManufacturer;
	__u8	iProduct;
	__u8	iSerialNumber;
	__u8	bNumConfigurations;
} __packed;
#define	USB_DT_DEVICE_SIZE		18

/* configuration descriptor (table 9-10) */
struct usb_config_descriptor {
	__u8	bLength;
	__u8	bDescriptorType;
	__le16	wTotalLength;
	__u8	bNumInterfaces;
	__u8	bConfigurationValue;
	__u8	iConfiguration;
	__u8	bmAttributes;
	__u8	bMaxPower;
} __packed;
#define	USB_DT_CONFIG_SIZE		9

/* interface descriptor (table 9-12) */
struct usb_interface_descriptor {
	__u8	bLength;
	__u8	bDescriptorType;
	__u8	bInterfaceNumber;
	__u8	bAlternateSetting;
	__u8	bNumEndpoints;
	__u8	bInterfaceClass;
	__u8	bInterfaceSubClass;
	__u8	bInterfaceProtocol;
	__u8	iInterface;
} __packed;
#define	USB_DT_INTERFACE_SIZE		9

/*
 * Endpoint descriptor (table 9-13). bRefresh/bSynchAddress exist only in
 * audio endpoint descriptors (USB_DT_ENDPOINT_AUDIO_SIZE); a standard
 * descriptor is USB_DT_ENDPOINT_SIZE bytes and leaves them 0.
 */
struct usb_endpoint_descriptor {
	__u8	bLength;
	__u8	bDescriptorType;
	__u8	bEndpointAddress;
	__u8	bmAttributes;
	__le16	wMaxPacketSize;
	__u8	bInterval;
	__u8	bRefresh;
	__u8	bSynchAddress;
} __packed;
#define	USB_DT_ENDPOINT_SIZE		7
#define	USB_DT_ENDPOINT_AUDIO_SIZE	9

#define	USB_ENDPOINT_NUMBER_MASK	0x0f
#define	USB_ENDPOINT_DIR_MASK		0x80
#define	USB_ENDPOINT_XFERTYPE_MASK	0x03
#define	USB_ENDPOINT_XFER_CONTROL	0
#define	USB_ENDPOINT_XFER_ISOC		1
#define	USB_ENDPOINT_XFER_BULK		2
#define	USB_ENDPOINT_XFER_INT		3
#define	USB_ENDPOINT_MAXP_MASK		0x07ff

enum usb_device_speed {
	USB_SPEED_UNKNOWN = 0,
	USB_SPEED_LOW,
	USB_SPEED_FULL,
	USB_SPEED_HIGH,
	USB_SPEED_WIRELESS,
	USB_SPEED_SUPER,
	USB_SPEED_SUPER_PLUS,
};

enum usb_device_state {
	USB_STATE_NOTATTACHED = 0,
	USB_STATE_ATTACHED,
	USB_STATE_POWERED,
	USB_STATE_RECONNECTING,
	USB_STATE_UNAUTHENTICATED,
	USB_STATE_DEFAULT,
	USB_STATE_ADDRESS,
	USB_STATE_CONFIGURED,
	USB_STATE_SUSPENDED,
};

static inline int
usb_endpoint_num(const struct usb_endpoint_descriptor *epd)
{
	return (epd->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
}

static inline int
usb_endpoint_type(const struct usb_endpoint_descriptor *epd)
{
	return (epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK);
}

static inline int
usb_endpoint_dir_in(const struct usb_endpoint_descriptor *epd)
{
	return ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN);
}

static inline int
usb_endpoint_dir_out(const struct usb_endpoint_descriptor *epd)
{
	return ((epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT);
}

static inline int
usb_endpoint_xfer_bulk(const struct usb_endpoint_descriptor *epd)
{
	return (usb_endpoint_type(epd) == USB_ENDPOINT_XFER_BULK);
}

static inline int
usb_endpoint_xfer_control(const struct usb_endpoint_descriptor *epd)
{
	return (usb_endpoint_type(epd) == USB_ENDPOINT_XFER_CONTROL);
}

static inline int
usb_endpoint_xfer_int(const struct usb_endpoint_descriptor *epd)
{
	return (usb_endpoint_type(epd) == USB_ENDPOINT_XFER_INT);
}

static inline int
usb_endpoint_xfer_isoc(const struct usb_endpoint_descriptor *epd)
{
	return (usb_endpoint_type(epd) == USB_ENDPOINT_XFER_ISOC);
}

#define	__LKPI_EP_IS(kind, dir)						\
static inline int							\
usb_endpoint_is_##kind##_##dir(const struct usb_endpoint_descriptor *epd) \
{									\
	return (usb_endpoint_xfer_##kind(epd) &&			\
	    usb_endpoint_dir_##dir(epd));				\
}
__LKPI_EP_IS(bulk, in)
__LKPI_EP_IS(bulk, out)
__LKPI_EP_IS(int, in)
__LKPI_EP_IS(int, out)
__LKPI_EP_IS(isoc, in)
__LKPI_EP_IS(isoc, out)
#undef __LKPI_EP_IS

static inline int
usb_endpoint_maxp(const struct usb_endpoint_descriptor *epd)
{
	return (le16_to_cpu(epd->wMaxPacketSize) & USB_ENDPOINT_MAXP_MASK);
}

/* high-bandwidth multiplier, USB 2.0 9.6.6: bits 12..11 + 1 */
static inline int
usb_endpoint_maxp_mult(const struct usb_endpoint_descriptor *epd)
{
	return (((le16_to_cpu(epd->wMaxPacketSize) >> 11) & 3) + 1);
}

#endif /* _LKPI_LINUX_USB_CH9_H_ */
