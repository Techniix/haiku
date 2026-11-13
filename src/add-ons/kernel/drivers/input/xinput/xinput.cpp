/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * xinput (roadmap F-42): USB driver for Xbox 360, Xbox 360 wireless and
 * Xbox One/Series pads and the many third-party pads that speak the same
 * protocols (8BitDo in X-input mode, PowerA, PDP, Hori, Razer, ...).
 *
 * It replaces the fake HID report descriptor usb_hid used to build for
 * Xbox 360 pads (that path read the reports but never sent anything to the
 * pad: no set-up request, no LED, no rumble, and no Xbox One support). Each
 * pad becomes a Linux evdev device through the gamepad core
 * (<gamepad_module.h>, /dev/input/event<N>) with xpad's names, button and
 * axis codes, so SDL and the SDL mapping database treat it as on Linux; the
 * core also gives it a BJoystick node so the old joystick API keeps seeing
 * these pads.
 *
 * Start-up per protocol (XInputProtocol.h):
 *  - Xbox 360 wired: the vendor control request third-party pads need
 *    (8BitDo pads switch themselves off without it), then the player LED.
 *  - Xbox 360 wireless receiver: a presence query; pads are registered and
 *    unregistered as the receiver reports them connecting and leaving.
 *  - Xbox One/Series: the audio interface is switched off where there is
 *    one, then the GIP init packets for the pad are sent in order; pads
 *    marked "delay init" get them after their announce packet.
 *
 * The interrupt IN pipe is read for as long as the pad is attached: XInput
 * pads send reports only when something changes, so this costs nothing
 * while the pad is idle, and the set-up never depends on a program having
 * the device open.
 *
 * Output (rumble, LED, GIP acknowledgements and init) goes through one
 * interrupt OUT transfer at a time; newer rumble and LED requests replace
 * older ones that have not been sent yet, like xpad's output slots.
 */


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <USB3.h>

#include <lock.h>
#include <util/AutoLock.h>

#include <gamepad_module.h>

#include "XInputProtocol.h"


#define DRIVER_NAME	"xinput"
#define ERROR(x...)	dprintf(DRIVER_NAME ": " x)
//#define TRACE_XINPUT
#ifdef TRACE_XINPUT
#	define TRACE(x...) dprintf(DRIVER_NAME ": " x)
#else
#	define TRACE(x...)
#endif

#define MAX_PADS	16

int32 api_version = B_CUR_DRIVER_API_VERSION;

static usb_module_info* sUSB = NULL;
static gamepad_module_info* sGamepad = NULL;
static mutex sDriverLock = MUTEX_INITIALIZER("xinput driver");


enum {
	OUT_COMMAND = 0,	// acknowledgements, presence queries
	OUT_LED,
	OUT_RUMBLE,
	OUT_SLOTS
};


struct xinput_pad {
	int32					index;
	usb_device				device;
	uint8					interfaceNumber;
	xinput_type				type;
	uint32					mapping;
	uint16					vendor;
	uint16					product;
	uint16					version;
	char					name[80];
	char					publishPath[32];

	usb_pipe				inPipe;
	usb_pipe				outPipe;
	uint8					inBuffer[XINPUT_MAX_PACKET];
	size_t					inSize;
	bool					removed;

	gamepad_device*			gamepad;
	bool					present;		// wireless: a pad is connected

	mutex					outLock;
	bool					outBusy;
	uint8					outBuffer[XINPUT_MAX_PACKET];
	uint8					pending[OUT_SLOTS][XINPUT_MAX_PACKET];
	size_t					pendingLength[OUT_SLOTS];
	int32					initIndex;		// next GIP init packet, -1 done
	uint8					sequence;
	bool					delayedInitDone;
};


static xinput_pad* sPads[MAX_PADS];
static const char* sPublishList[MAX_PADS + 1];


// #pragma mark - output


static void out_callback(void* cookie, status_t status, void* data,
	size_t actualLength);


// Called with outLock held.
static void
send_next(xinput_pad* pad)
{
	if (pad->outBusy || pad->removed || pad->outPipe == 0)
		return;

	size_t length = 0;
	if (pad->type == XINPUT_TYPE_XBOXONE && pad->initIndex >= 0) {
		length = xinput_gip_init_packet(pad->vendor, pad->product,
			pad->initIndex, pad->sequence, pad->outBuffer);
		if (length == 0)
			pad->initIndex = -1;
		else {
			pad->initIndex++;
			pad->sequence++;
		}
	}
	for (int32 slot = 0; length == 0 && slot < OUT_SLOTS; slot++) {
		if (pad->pendingLength[slot] == 0)
			continue;
		length = pad->pendingLength[slot];
		memcpy(pad->outBuffer, pad->pending[slot], length);
		pad->pendingLength[slot] = 0;
	}
	if (length == 0)
		return;

	status_t status = sUSB->queue_interrupt(pad->outPipe, pad->outBuffer,
		length, out_callback, pad);
	if (status == B_OK)
		pad->outBusy = true;
	else
		ERROR("could not send %" B_PRIuSIZE " bytes: %s\n", length,
			strerror(status));
}


static void
out_callback(void* cookie, status_t status, void* data, size_t actualLength)
{
	xinput_pad* pad = (xinput_pad*)cookie;
	MutexLocker locker(pad->outLock);
	pad->outBusy = false;
	if (status != B_OK && status != B_CANCELED)
		TRACE("output transfer: %s\n", strerror(status));
	if (status != B_CANCELED)
		send_next(pad);
}


static void
queue_output(xinput_pad* pad, int32 slot, const uint8* packet, size_t length)
{
	if (length == 0 || length > XINPUT_MAX_PACKET)
		return;
	MutexLocker locker(pad->outLock);
	memcpy(pad->pending[slot], packet, length);
	pad->pendingLength[slot] = length;
	send_next(pad);
}


// #pragma mark - gamepad core hooks


static void
pad_rumble(void* cookie, uint16 strong, uint16 weak)
{
	xinput_pad* pad = (xinput_pad*)cookie;
	uint8 packet[XINPUT_MAX_PACKET];
	size_t length;
	{
		MutexLocker locker(pad->outLock);
		length = xinput_build_rumble(pad->type, strong, weak, pad->sequence++,
			packet);
	}
	queue_output(pad, OUT_RUMBLE, packet, length);
}


static void
pad_set_player(void* cookie, int32 player)
{
	xinput_pad* pad = (xinput_pad*)cookie;
	uint8 packet[XINPUT_MAX_PACKET];
	size_t length = xinput_build_led(pad->type, player, packet);
	queue_output(pad, OUT_LED, packet, length);
}


static gamepad_device_hooks sPadHooks = {
	NULL,
	NULL,
	pad_rumble,
	pad_set_player,
	NULL
};


static void
report_event(void* cookie, uint16 type, uint16 code, int32 value)
{
	xinput_pad* pad = (xinput_pad*)cookie;
	if (pad->gamepad != NULL)
		sGamepad->report(pad->gamepad, type, code, value);
}


static void
register_pad(xinput_pad* pad)
{
	if (pad->gamepad != NULL)
		return;

	gamepad_device_info* info
		= (gamepad_device_info*)malloc(sizeof(gamepad_device_info));
	if (info == NULL)
		return;
	xinput_fill_info(pad->type, pad->vendor, pad->product, pad->version,
		pad->mapping, pad->name, info);
	snprintf(info->phys, sizeof(info->phys), "usb-%" B_PRIu32 "/input%u",
		(uint32)pad->device, pad->interfaceNumber);
	info->flags = GAMEPAD_FLAG_JOYSTICK_NODE;

	gamepad_device* gamepad;
	if (sGamepad->register_device(info, &sPadHooks, pad, &gamepad) == B_OK)
		pad->gamepad = gamepad;
	free(info);

	// pads on the cable report no battery: say so
	if (pad->gamepad != NULL && pad->type != XINPUT_TYPE_XBOX360W)
		sGamepad->set_battery(pad->gamepad, -1, false, true);
}


static void
unregister_pad(xinput_pad* pad)
{
	gamepad_device* gamepad = pad->gamepad;
	pad->gamepad = NULL;
	if (gamepad != NULL)
		sGamepad->unregister_device(gamepad);
}


// #pragma mark - input


static void in_callback(void* cookie, status_t status, void* data,
	size_t actualLength);


static void
queue_input(xinput_pad* pad)
{
	if (pad->removed)
		return;
	status_t status = sUSB->queue_interrupt(pad->inPipe, pad->inBuffer,
		pad->inSize, in_callback, pad);
	if (status != B_OK)
		ERROR("could not queue input transfer: %s\n", strerror(status));
}


static void
in_callback(void* cookie, status_t status, void* data, size_t actualLength)
{
	xinput_pad* pad = (xinput_pad*)cookie;
	if (status == B_CANCELED || pad->removed)
		return;

	if (status != B_OK) {
		if (status == B_DEV_STALLED)
			sUSB->clear_feature(pad->inPipe, USB_FEATURE_ENDPOINT_HALT);
		snooze(1000);
		queue_input(pad);
		return;
	}

	xinput_sink sink = { report_event, pad };
	switch (pad->type) {
		case XINPUT_TYPE_XBOX360:
			xinput_parse_360(pad->inBuffer, actualLength, pad->type,
				pad->mapping, sink);
			break;

		case XINPUT_TYPE_XBOX360W:
		{
			bool changed, present = pad->present;
			xinput_parse_360w(pad->inBuffer, actualLength, pad->mapping,
				pad->gamepad != NULL, changed, present, sink);
			if (changed && present != pad->present) {
				pad->present = present;
				if (present)
					register_pad(pad);
				else
					unregister_pad(pad);
			}
			break;
		}

		case XINPUT_TYPE_XBOXONE:
		{
			uint8 ack[XINPUT_MAX_PACKET];
			size_t ackLength;
			bool announce;
			xinput_parse_gip(pad->inBuffer, actualLength, pad->mapping, sink,
				ack, ackLength, announce);
			if (ackLength > 0)
				queue_output(pad, OUT_COMMAND, ack, ackLength);
			if (announce && (pad->mapping & XINPUT_DELAY_INIT) != 0
				&& !pad->delayedInitDone) {
				pad->delayedInitDone = true;
				MutexLocker locker(pad->outLock);
				pad->initIndex = 0;
				send_next(pad);
			}
			break;
		}

		default:
			break;
	}

	queue_input(pad);
}


// #pragma mark - USB


static void
rebuild_publish_list()
{
	int32 count = 0;
	for (int32 i = 0; i < MAX_PADS; i++) {
		if (sPads[i] != NULL)
			sPublishList[count++] = sPads[i]->publishPath;
	}
	sPublishList[count] = NULL;
}


static status_t
attach_interface(usb_device device, const usb_device_descriptor* descriptor,
	const usb_interface_info* interface, xinput_type type, int32 parent,
	xinput_pad** _pad)
{
	usb_pipe inPipe = 0, outPipe = 0;
	size_t inSize = 32;
	for (size_t i = 0; i < interface->endpoint_count; i++) {
		usb_endpoint_descriptor* endpoint = interface->endpoint[i].descr;
		if ((endpoint->attributes & USB_ENDPOINT_ATTR_MASK)
				!= USB_ENDPOINT_ATTR_INTERRUPT)
			continue;
		if ((endpoint->endpoint_address & USB_ENDPOINT_ADDR_DIR_IN) != 0) {
			if (inPipe == 0) {
				inPipe = interface->endpoint[i].handle;
				inSize = endpoint->max_packet_size;
			}
		} else if (outPipe == 0)
			outPipe = interface->endpoint[i].handle;
	}
	if (inPipe == 0)
		return B_ERROR;
	if (inSize > XINPUT_MAX_PACKET || inSize == 0)
		inSize = XINPUT_MAX_PACKET;

	xinput_pad* pad = new(std::nothrow) xinput_pad;
	if (pad == NULL)
		return B_NO_MEMORY;
	memset((void*)pad, 0, sizeof(*pad));
	pad->device = device;
	pad->interfaceNumber = interface->descr->interface_number;
	pad->type = type;
	pad->vendor = descriptor->vendor_id;
	pad->product = descriptor->product_id;
	pad->version = descriptor->device_version;
	pad->inPipe = inPipe;
	pad->outPipe = outPipe;
	pad->inSize = inSize;
	pad->initIndex = type == XINPUT_TYPE_XBOXONE ? 0 : -1;
	mutex_init(&pad->outLock, "xinput output");

	const xinput_known_device* known = xinput_find_device(pad->vendor,
		pad->product);
	if (known != NULL) {
		pad->mapping = known->mapping;
		strlcpy(pad->name, known->name, sizeof(pad->name));
	} else
		strlcpy(pad->name, "Generic X-Box pad", sizeof(pad->name));
	if ((pad->mapping & XINPUT_DELAY_INIT) != 0)
		pad->initIndex = -1;

	*_pad = pad;
	return B_OK;
}


static status_t
xinput_device_added(usb_device device, void** cookie)
{
	const usb_device_descriptor* descriptor
		= sUSB->get_device_descriptor(device);
	const usb_configuration_info* config = sUSB->get_configuration(device);
	if (config == NULL) {
		config = sUSB->get_nth_configuration(device, 0);
		if (config == NULL || sUSB->set_configuration(device, config) != B_OK)
			return B_ERROR;
		config = sUSB->get_configuration(device);
		if (config == NULL)
			return B_ERROR;
	}

	MutexLocker locker(sDriverLock);
	bool found = false;
	for (size_t i = 0; i < config->interface_count; i++) {
		const usb_interface_info* interface = config->interface[i].active;
		if (interface == NULL || interface->descr == NULL)
			continue;
		xinput_type type = xinput_type_for_interface(
			interface->descr->interface_class,
			interface->descr->interface_subclass,
			interface->descr->interface_protocol);
		if (type == XINPUT_TYPE_NONE)
			continue;
		// Xbox One pads: only the first (input) interface is the pad
		if (type == XINPUT_TYPE_XBOXONE
			&& interface->descr->interface_number != 0)
			continue;

		int32 index = -1;
		for (int32 j = 0; j < MAX_PADS; j++) {
			if (sPads[j] == NULL) {
				index = j;
				break;
			}
		}
		if (index < 0)
			break;

		xinput_pad* pad;
		if (attach_interface(device, descriptor, interface, type, index, &pad)
				!= B_OK)
			continue;
		pad->index = index;
		snprintf(pad->publishPath, sizeof(pad->publishPath),
			"input/xinput/%" B_PRId32, index);
		sPads[index] = pad;
		found = true;

		dprintf(DRIVER_NAME ": %04x:%04x \"%s\" (%s), interface %u\n",
			pad->vendor, pad->product, pad->name,
			type == XINPUT_TYPE_XBOX360 ? "Xbox 360"
				: type == XINPUT_TYPE_XBOX360W ? "Xbox 360 wireless"
				: "Xbox One", pad->interfaceNumber);

		if (type == XINPUT_TYPE_XBOX360) {
			// Third-party pads finish their set-up with this request (and
			// some switch themselves off without it); first-party pads
			// answer it too.
			uint8 dummy[20];
			size_t actual;
			status_t status = sUSB->send_request(device,
				USB_REQTYPE_VENDOR | USB_REQTYPE_INTERFACE_IN, 0x01, 0x0100,
				0x0000, sizeof(dummy), dummy, &actual);
			if (status != B_OK)
				TRACE("set-up request: %s\n", strerror(status));
			register_pad(pad);
		} else if (type == XINPUT_TYPE_XBOXONE) {
			// switching the audio interface off makes some pads (PowerA)
			// report the guide button
			for (size_t k = 0; k < config->interface_count; k++) {
				const usb_interface_info* other = config->interface[k].active;
				if (other != NULL && other->descr != NULL
					&& other->descr->interface_number == 1) {
					sUSB->set_alt_interface(device,
						&config->interface[k].alt[0]);
				}
			}
			register_pad(pad);
		}

		queue_input(pad);

		if (type == XINPUT_TYPE_XBOX360W) {
			uint8 packet[XINPUT_MAX_PACKET];
			size_t length = xinput_build_presence_query(packet);
			queue_output(pad, OUT_COMMAND, packet, length);
		} else if (type == XINPUT_TYPE_XBOXONE) {
			MutexLocker outLocker(pad->outLock);
			send_next(pad);
		}
	}

	if (!found)
		return B_ERROR;

	rebuild_publish_list();
	*cookie = (void*)(addr_t)device;
	return B_OK;
}


static status_t
xinput_device_removed(void* cookie)
{
	usb_device device = (usb_device)(addr_t)cookie;

	xinput_pad* removed[MAX_PADS];
	int32 count = 0;
	{
		MutexLocker locker(sDriverLock);
		for (int32 i = 0; i < MAX_PADS; i++) {
			if (sPads[i] == NULL || sPads[i]->device != device)
				continue;
			sPads[i]->removed = true;
			removed[count++] = sPads[i];
			sPads[i] = NULL;
		}
		rebuild_publish_list();
	}

	for (int32 i = 0; i < count; i++) {
		xinput_pad* pad = removed[i];
		unregister_pad(pad);
		sUSB->cancel_queued_transfers(pad->inPipe);
		if (pad->outPipe != 0)
			sUSB->cancel_queued_transfers(pad->outPipe);
		mutex_destroy(&pad->outLock);
		delete pad;
	}
	return B_OK;
}


// #pragma mark - info nodes


// Each attached pad (or receiver slot) has an info node, input/xinput/<N>.
// Besides keeping the driver loaded while pads are attached (legacy drivers
// without nodes are unloaded), it tells which evdev device the pad is.


static status_t
info_open(const char* name, uint32 flags, void** cookie)
{
	MutexLocker locker(sDriverLock);
	for (int32 i = 0; i < MAX_PADS; i++) {
		if (sPads[i] != NULL && strcmp(sPads[i]->publishPath, name) == 0) {
			*cookie = (void*)(addr_t)i;
			return B_OK;
		}
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
info_close(void*)
{
	return B_OK;
}


static status_t
info_free(void*)
{
	return B_OK;
}


static status_t
info_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	char text[256];
	{
		MutexLocker locker(sDriverLock);
		xinput_pad* pad = sPads[(addr_t)cookie];
		if (pad == NULL) {
			*_length = 0;
			return B_DEV_NOT_READY;
		}
		int32 event = pad->gamepad != NULL
			? sGamepad->index(pad->gamepad) : -1;
		char evdev[32] = "none";
		if (event >= 0) {
			snprintf(evdev, sizeof(evdev), "/dev/input/event%" B_PRId32,
				event);
		}
		snprintf(text, sizeof(text),
			"name: %s\nusb: %04x:%04x\nprotocol: %s\nevdev: %s\n",
			pad->name, pad->vendor, pad->product,
			pad->type == XINPUT_TYPE_XBOX360 ? "xbox360"
				: pad->type == XINPUT_TYPE_XBOX360W ? "xbox360w" : "xboxone",
			evdev);
	}
	size_t length = strlen(text);
	if (position >= (off_t)length) {
		*_length = 0;
		return B_OK;
	}
	length = min_c(length - position, *_length);
	status_t status = user_memcpy(buffer, text + position, length);
	*_length = status == B_OK ? length : 0;
	return status;
}


static status_t
info_write(void*, off_t, const void*, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
info_control(void*, uint32, void*, size_t)
{
	return B_DEV_INVALID_IOCTL;
}


static device_hooks sInfoHooks = {
	info_open,
	info_close,
	info_free,
	info_control,
	info_read,
	info_write,
	NULL,
	NULL,
	NULL,
	NULL
};


// #pragma mark - driver API


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	if (get_module(B_USB_MODULE_NAME, (module_info**)&sUSB) != B_OK)
		return B_ERROR;
	if (get_module(GAMEPAD_MODULE_NAME, (module_info**)&sGamepad) != B_OK) {
		put_module(B_USB_MODULE_NAME);
		return B_ERROR;
	}

	memset(sPads, 0, sizeof(sPads));
	sPublishList[0] = NULL;

	static usb_support_descriptor supportDescriptors[] = {
		{ 0xff, 0x5d, 0x01, 0, 0 },	// Xbox 360
		{ 0xff, 0x5d, 0x81, 0, 0 },	// Xbox 360 wireless receiver
		{ 0xff, 0x47, 0xd0, 0, 0 }	// Xbox One/Series (GIP)
	};
	static usb_notify_hooks notifyHooks = {
		&xinput_device_added,
		&xinput_device_removed
	};

	sUSB->register_driver(DRIVER_NAME, supportDescriptors,
		sizeof(supportDescriptors) / sizeof(supportDescriptors[0]), NULL);
	sUSB->install_notify(DRIVER_NAME, &notifyHooks);
	return B_OK;
}


void
uninit_driver()
{
	sUSB->uninstall_notify(DRIVER_NAME);
	put_module(GAMEPAD_MODULE_NAME);
	put_module(B_USB_MODULE_NAME);
}


const char**
publish_devices()
{
	MutexLocker locker(sDriverLock);
	return sPublishList;
}


device_hooks*
find_device(const char* name)
{
	return &sInfoHooks;
}
