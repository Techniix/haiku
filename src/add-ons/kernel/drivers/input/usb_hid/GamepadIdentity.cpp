/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renga (roadmap F-42): what the evdev and hidraw nodes of a USB HID game
 * controller report about it. Like Linux's usbhid: the name is the
 * manufacturer string, a space and the product string ("HID vvvv:pppp"
 * without strings), the version is bcdDevice, uniq is the serial number,
 * phys names the USB device and interface. SDL builds its controller GUID
 * from bus, vendor, product and version, so the SDL mapping database
 * entries for Linux match these pads.
 */


#include "Driver.h"
#include "HIDDevice.h"
#include "GamepadEvdevBridge.h"

#include <stdio.h>
#include <string.h>

#include <linux/input.h>


static void
read_string(usb_device device, uint8 index, char* buffer, size_t size)
{
	buffer[0] = '\0';
	if (index == 0)
		return;

	uint8 raw[256];
	size_t actual = 0;
	if (gUSBModule->get_descriptor(device, USB_DESCRIPTOR_STRING, index,
			0x0409, raw, sizeof(raw), &actual) != B_OK || actual < 2)
		return;

	// UTF-16LE; characters outside ASCII become '?'
	size_t length = min_c(raw[0], actual);
	size_t out = 0;
	for (size_t i = 2; i + 1 < length && out + 1 < size; i += 2) {
		uint16 c = raw[i] | (raw[i + 1] << 8);
		buffer[out++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
	}
	buffer[out] = '\0';
	// trailing spaces, which some pads send
	while (out > 0 && buffer[out - 1] == ' ')
		buffer[--out] = '\0';
}


status_t
gamepad_hid_get_identity(HIDDevice* device, gamepad_hid_identity& identity)
{
	usb_device usbDevice = device->USBDevice();
	const usb_device_descriptor* descriptor
		= gUSBModule->get_device_descriptor(usbDevice);
	if (descriptor == NULL)
		return B_ERROR;

	identity.bus = BUS_USB;
	identity.vendor = descriptor->vendor_id;
	identity.product = descriptor->product_id;
	identity.version = descriptor->device_version;

	char manufacturer[64], product[64];
	read_string(usbDevice, descriptor->manufacturer, manufacturer,
		sizeof(manufacturer));
	read_string(usbDevice, descriptor->product, product, sizeof(product));
	read_string(usbDevice, descriptor->serial_number, identity.uniq,
		sizeof(identity.uniq));

	identity.name[0] = '\0';
	strlcpy(identity.name, manufacturer, sizeof(identity.name));
	if (product[0] != '\0') {
		if (identity.name[0] != '\0')
			strlcat(identity.name, " ", sizeof(identity.name));
		strlcat(identity.name, product, sizeof(identity.name));
	}
	if (identity.name[0] == '\0') {
		snprintf(identity.name, sizeof(identity.name), "HID %04x:%04x",
			identity.vendor, identity.product);
	}
	snprintf(identity.phys, sizeof(identity.phys),
		"usb-%" B_PRIu32 "/input%" B_PRIuSIZE, (uint32)usbDevice,
		device->InterfaceIndex());
	return B_OK;
}
