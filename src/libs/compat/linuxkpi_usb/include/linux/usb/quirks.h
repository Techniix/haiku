/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * USB device quirk bits (<linux/usb/quirks.h>), same values as Linux so
 * drivers can test and set them in usb_device->quirks. Haiku's USB stack
 * applies its own quirks; LinuxKPI only stores these.
 */
#ifndef _LKPI_LINUX_USB_QUIRKS_H_
#define _LKPI_LINUX_USB_QUIRKS_H_

#include <linux/bits.h>

#define	USB_QUIRK_STRING_FETCH_255		BIT(0)
#define	USB_QUIRK_RESET_RESUME			BIT(1)
#define	USB_QUIRK_NO_SET_INTF			BIT(2)
#define	USB_QUIRK_CONFIG_INTF_STRINGS		BIT(3)
#define	USB_QUIRK_RESET				BIT(4)
#define	USB_QUIRK_HONOR_BNUMINTERFACES		BIT(5)
#define	USB_QUIRK_DELAY_INIT			BIT(6)
#define	USB_QUIRK_LINEAR_UFRAME_INTR_BINTERVAL	BIT(7)
#define	USB_QUIRK_DEVICE_QUALIFIER		BIT(8)
#define	USB_QUIRK_IGNORE_REMOTE_WAKEUP		BIT(9)
#define	USB_QUIRK_NO_LPM			BIT(10)
#define	USB_QUIRK_LINEAR_FRAME_INTR_BINTERVAL	BIT(11)
#define	USB_QUIRK_DISCONNECT_SUSPEND		BIT(12)
#define	USB_QUIRK_DELAY_CTRL_MSG		BIT(13)
#define	USB_QUIRK_HUB_SLOW_RESET		BIT(14)
#define	USB_QUIRK_ENDPOINT_IGNORE		BIT(15)

#endif
