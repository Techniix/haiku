/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The gamepad protocol code (roadmap F-42) with recorded and constructed
	packets, since QEMU emulates no game controller:
	 - xinput's XInput parser and packet builder (XInputProtocol.cpp):
	   interface classes, the device table (the 8BitDo Ultimate 2C's name),
	   Xbox 360 input reports, wireless presence packets, GIP input, the
	   guide button and its acknowledgement, the GIP init sequence, rumble
	   and LED packets;
	 - usb_hid's HID-to-evdev mapping (GamepadEvdevBridge.cpp, built here
	   with USERLAND_HID) on the report descriptor of a typical D-input
	   pad: 16 buttons, a hat switch, four 8-bit axes;
	 - the SDL mapping code of libdevice on both, including the
	   database's entry for the 8BitDo Ultimate 2C and the default mapping
	   for pads not in the database.
*/


#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "XInputProtocol.h"

#include "GamepadEvdevBridge.h"
#include "HIDDevice.h"
#include "HIDParser.h"
#include "HIDReport.h"

#include <GamepadMapping.h>
#include <usb/USB_hid.h>


#define SUITE "gamepad_protocol"

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


struct Event {
	uint16	type;
	uint16	code;
	int32	value;
};

static std::vector<Event> sEvents;


static void
collect(void*, uint16 type, uint16 code, int32 value)
{
	Event event = { type, code, value };
	sEvents.push_back(event);
}


static bool
has_event(uint16 type, uint16 code, int32 value)
{
	for (size_t i = 0; i < sEvents.size(); i++) {
		if (sEvents[i].type == type && sEvents[i].code == code
			&& sEvents[i].value == value)
			return true;
	}
	return false;
}


// The gamepad core's state keeping, enough for the mapping checks.
static void
apply_events(BPrivate::GamepadDevice& device)
{
	for (size_t i = 0; i < sEvents.size(); i++) {
		const Event& event = sEvents[i];
		if (event.type == EV_KEY && event.code < KEY_CNT) {
			if (event.value)
				device.keyState[event.code / 8] |= 1 << (event.code % 8);
			else
				device.keyState[event.code / 8] &= ~(1 << (event.code % 8));
		} else if (event.type == EV_ABS && event.code < ABS_CNT)
			device.abs[event.code].value = event.value;
	}
}


static void
device_from_info(const gamepad_device_info& info,
	BPrivate::GamepadDevice& device)
{
	memset(&device, 0, sizeof(device));
	device.id = info.id;
	strlcpy(device.name, info.name, sizeof(device.name));
	memcpy(device.keyBits, info.key_bits, sizeof(device.keyBits));
	memcpy(device.absBits, info.abs_bits, sizeof(device.absBits));
	memcpy(device.abs, info.abs, sizeof(device.abs));
	device.Enumerate();
}


// #pragma mark - XInput


static void
test_xinput()
{
	xinput_sink sink = { collect, NULL };

	report("interface_types",
		xinput_type_for_interface(0xff, 0x5d, 0x01) == XINPUT_TYPE_XBOX360
		&& xinput_type_for_interface(0xff, 0x5d, 0x81)
			== XINPUT_TYPE_XBOX360W
		&& xinput_type_for_interface(0xff, 0x47, 0xd0) == XINPUT_TYPE_XBOXONE
		&& xinput_type_for_interface(0x03, 0x00, 0x00) == XINPUT_TYPE_NONE,
		"360, 360 wireless, One, HID");

	const xinput_known_device* known = xinput_find_device(0x2dc8, 0x310a);
	report("device_table_8bitdo", known != NULL
		&& known->type == XINPUT_TYPE_XBOX360
		&& strcmp(known->name, "8BitDo Ultimate 2C Wireless Controller") == 0,
		"%s", known != NULL ? known->name : "not found");

	// Xbox 360 report: D-pad up + start, A + LB, triggers 0x40/0xff, left
	// stick (1000, 2000 up-positive), right stick (-32768, 0)
	const uint8 report360[20] = { 0x00, 0x14, 0x11, 0x11, 0x40, 0xff,
		0xe8, 0x03, 0xd0, 0x07, 0x00, 0x80, 0x00, 0x00, 0, 0, 0, 0, 0, 0 };
	sEvents.clear();
	bool parsed = xinput_parse_360(report360, sizeof(report360),
		XINPUT_TYPE_XBOX360, 0, sink);
	report("x360_report", parsed && has_event(EV_KEY, BTN_A, 1)
		&& has_event(EV_KEY, BTN_TL, 1) && has_event(EV_KEY, BTN_START, 1)
		&& has_event(EV_KEY, BTN_B, 0) && has_event(EV_ABS, ABS_HAT0Y, -1)
		&& has_event(EV_ABS, ABS_HAT0X, 0) && has_event(EV_ABS, ABS_X, 1000)
		&& has_event(EV_ABS, ABS_Y, -2001) && has_event(EV_ABS, ABS_RX, -32768)
		&& has_event(EV_ABS, ABS_Z, 0x40) && has_event(EV_ABS, ABS_RZ, 0xff)
		&& sEvents.back().type == EV_SYN,
		"%zu events", sEvents.size());

	// The mapping the database has for the 8BitDo Ultimate 2C in X-input
	// mode, on the evdev device xinput makes of it
	gamepad_device_info info;
	xinput_fill_info(XINPUT_TYPE_XBOX360, 0x2dc8, 0x310a, 0x0114, 0,
		known != NULL ? known->name : NULL, &info);
	BPrivate::GamepadDevice device;
	device_from_info(info, device);
	char guid[33];
	device.GUID(guid);
	BString line;
	status_t found = BPrivate::gamepad_find_mapping(guid, line);
	BPrivate::GamepadMapping mapping;
	bool mapped = found == B_OK && mapping.Parse(line.String());
	apply_events(device);
	gamepad_state state;
	memset(&state, 0, sizeof(state));
	if (mapped)
		mapping.Apply(device, state);
	report("x360_database_mapping", mapped
		&& strcmp(guid, "03000000c82d00000a31000014010000") == 0
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_A)) != 0
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_LEFT_SHOULDER)) != 0
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_START)) != 0
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_DPAD_UP)) != 0
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_B)) == 0
		&& state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER] == 32767
		&& state.axes[B_GAMEPAD_AXIS_LEFT_TRIGGER] > 8000
		&& state.axes[B_GAMEPAD_AXIS_LEFT_TRIGGER] < 8300
		&& state.axes[B_GAMEPAD_AXIS_RIGHT_X] == -32768,
		"GUID %s, %s, buttons %#" B_PRIx32 ", triggers %d/%d", guid,
		found == B_OK ? "in the database" : "not in the database",
		state.buttons, state.axes[B_GAMEPAD_AXIS_LEFT_TRIGGER],
		state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER]);

	// A pad that is not in the database: the default mapping
	xinput_fill_info(XINPUT_TYPE_XBOX360, 0x1234, 0x5678, 0x0001, 0,
		"Unknown pad", &info);
	device_from_info(info, device);
	apply_events(device);
	BPrivate::GamepadMapping fallback;
	bool defaulted = fallback.SetDefault(device);
	memset(&state, 0, sizeof(state));
	fallback.Apply(device, state);
	report("default_mapping", defaulted
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_A)) != 0
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_DPAD_UP)) != 0
		&& state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER] == 32767,
		"buttons %#" B_PRIx32, state.buttons);

	// wireless receiver: a pad connects, sends data, leaves
	const uint8 connect[2] = { 0x08, 0x80 };
	const uint8 leave[2] = { 0x08, 0x00 };
	uint8 data360w[29] = { 0x00, 0x01, 0x00, 0xf0 };
	memcpy(data360w + 4, report360, 20);
	bool changed = false, present = false;
	sEvents.clear();
	xinput_parse_360w(connect, sizeof(connect), 0, false, changed, present,
		sink);
	bool connected = changed && present;
	xinput_parse_360w(data360w, sizeof(data360w), 0, true, changed, present,
		sink);
	bool data = !changed && has_event(EV_KEY, BTN_A, 1);
	xinput_parse_360w(leave, sizeof(leave), 0, true, changed, present, sink);
	report("x360w_presence", connected && data && changed && !present,
		"connect %d, data %d, leave %d", connected, data, changed && !present);

	// GIP input: A, menu, LB, D-pad right, triggers 1023/512
	uint8 gip[18] = { 0x20, 0x00, 0x05, 0x0e, 0x14, 0x18, 0xff, 0x03,
		0x00, 0x02, 0x10, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8 ack[XINPUT_MAX_PACKET];
	size_t ackLength;
	bool announce;
	sEvents.clear();
	bool gipParsed = xinput_parse_gip(gip, sizeof(gip), 0, sink, ack,
		ackLength, announce);
	report("gip_input", gipParsed && ackLength == 0
		&& has_event(EV_KEY, BTN_A, 1) && has_event(EV_KEY, BTN_START, 1)
		&& has_event(EV_KEY, BTN_TL, 1) && has_event(EV_ABS, ABS_HAT0X, 1)
		&& has_event(EV_ABS, ABS_Z, 1023) && has_event(EV_ABS, ABS_RZ, 512)
		&& has_event(EV_ABS, ABS_X, 16) && has_event(EV_ABS, ABS_Y, -33),
		"%zu events", sEvents.size());

	// GIP guide button with an acknowledgement request (Xbox One S)
	const uint8 guide[6] = { 0x07, 0x30, 0x2a, 0x02, 0x01, 0x5b };
	sEvents.clear();
	xinput_parse_gip(guide, sizeof(guide), 0, sink, ack, ackLength,
		announce);
	report("gip_guide_ack", has_event(EV_KEY, BTN_MODE, 1)
		&& ackLength == 13 && ack[0] == 0x01 && ack[2] == 0x2a
		&& ack[5] == 0x07, "ack of %zu bytes, sequence %#x", ackLength,
		ack[2]);

	// init sequences: every pad gets power on, LED on and "auth done";
	// the Elite 2 also gets the One S packet and the extra-input packet
	uint8 packet[XINPUT_MAX_PACKET];
	int32 generic = 0;
	while (xinput_gip_init_packet(0x045e, 0x0b12, generic, generic, packet)
			> 0)
		generic++;
	xinput_gip_init_packet(0x045e, 0x0b12, 0, 7, packet);
	bool powerFirst = packet[0] == 0x05 && packet[2] == 7;
	int32 elite = 0;
	while (xinput_gip_init_packet(0x045e, 0x0b00, elite, 0, packet) > 0)
		elite++;
	report("gip_init_sequence", generic == 3 && powerFirst && elite == 5,
		"%d packets for a Series pad (power on first: %d), %d for an Elite 2",
		(int)generic, powerFirst, (int)elite);

	// output packets
	size_t rumble360 = xinput_build_rumble(XINPUT_TYPE_XBOX360, 0xff00,
		0x8000, 0, packet);
	bool rumbleOK = rumble360 == 8 && packet[1] == 0x08 && packet[3] == 0xff
		&& packet[4] == 0x80;
	size_t rumbleOne = xinput_build_rumble(XINPUT_TYPE_XBOXONE, 0xffff,
		0x8000, 9, packet);
	rumbleOK = rumbleOK && rumbleOne == 13 && packet[0] == 0x09
		&& packet[2] == 9 && packet[8] == 0x7f && packet[9] == 0x40;
	size_t led = xinput_build_led(XINPUT_TYPE_XBOX360, 2, packet);
	bool ledOK = led == 3 && packet[0] == 0x01 && packet[2] == 3;
	led = xinput_build_led(XINPUT_TYPE_XBOX360W, 1, packet);
	ledOK = ledOK && led == 12 && packet[3] == 0x42;
	report("output_packets", rumbleOK && ledOK
		&& xinput_build_led(XINPUT_TYPE_XBOXONE, 1, packet) == 0,
		"rumble %d, LEDs %d", rumbleOK, ledOK);
}


// #pragma mark - HID


// A typical D-input pad (the descriptor shape of 8BitDo, Logitech F310 in
// D mode and many generic pads): gamepad application, 16 buttons, hat,
// X, Y, Z, Rz as 8-bit values.
static const uint8 kDInputDescriptor[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00,
	0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
	0x65, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
	0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
	0xc0
};


status_t
gamepad_hid_get_identity(HIDDevice*, gamepad_hid_identity& identity)
{
	identity.bus = BUS_USB;
	identity.vendor = 0x2dc8;
	identity.product = 0x6012;
	identity.version = 0x0100;
	strlcpy(identity.name, "Test D-input pad", sizeof(identity.name));
	return B_OK;
}


static void
test_hid()
{
	HIDParser parser(NULL);
	status_t status = parser.ParseReportDescriptor(kDInputDescriptor,
		sizeof(kDInputDescriptor));
	HIDReport* input = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	// the bridge only hands the device to gamepad_hid_get_identity()
	static uint64 deviceStorage[(sizeof(HIDDevice) + 7) / 8];
	HIDDevice* device = (HIDDevice*)deviceStorage;
	GamepadEvdevBridge* bridge = status == B_OK && input != NULL
		? GamepadEvdevBridge::Create(*device, *input,
			B_HID_UID_GD_GAMEPAD) : NULL;
	if (bridge == NULL) {
		report("hid_mapping", false, "descriptor %s, no bridge",
			strerror(status));
		return;
	}

	const gamepad_device_info& info = bridge->Info();
	bool buttons = gamepad_test_bit(info.key_bits, BTN_SOUTH)
		&& gamepad_test_bit(info.key_bits, BTN_THUMBR)
		&& gamepad_test_bit(info.key_bits, BTN_GAMEPAD + 15)
		&& !gamepad_test_bit(info.key_bits, BTN_TRIGGER_HAPPY1)
		&& !gamepad_test_bit(info.key_bits, BTN_TRIGGER);
	bool axes = gamepad_test_bit(info.abs_bits, ABS_X)
		&& gamepad_test_bit(info.abs_bits, ABS_Z)
		&& gamepad_test_bit(info.abs_bits, ABS_RZ)
		&& gamepad_test_bit(info.abs_bits, ABS_HAT0X)
		&& info.abs[ABS_X].maximum == 255;
	report("hid_capabilities", buttons && axes,
		"buttons as BTN_GAMEPAD %d, axes and hat %d", buttons, axes);

	// button 1 and 4, hat 2 (east), X 255, Y 0, Z 128, Rz 64
	uint8 data[7] = { 0x09, 0x00, 0x02, 0xff, 0x00, 0x80, 0x40 };
	input->SetReport(B_OK, data, sizeof(data));
	sEvents.clear();
	bridge->SetReportHook(collect, NULL);
	bridge->ProcessReport();
	report("hid_report", has_event(EV_KEY, BTN_SOUTH, 1)
		&& has_event(EV_KEY, BTN_EAST, 0) && has_event(EV_KEY, BTN_NORTH, 1)
		&& has_event(EV_ABS, ABS_HAT0X, 1) && has_event(EV_ABS, ABS_HAT0Y, 0)
		&& has_event(EV_ABS, ABS_X, 255) && has_event(EV_ABS, ABS_RZ, 64),
		"%zu events", sEvents.size());

	// null hat state: centred
	data[2] = 0x08;
	input->SetReport(B_OK, data, sizeof(data));
	sEvents.clear();
	bridge->ProcessReport();
	report("hid_hat_centred", has_event(EV_ABS, ABS_HAT0X, 0)
		&& has_event(EV_ABS, ABS_HAT0Y, 0), "%zu events", sEvents.size());

	delete bridge;
}


// #pragma mark - mapping format


static void
test_mapping_format()
{
	// half axes, inverted axes, buttons on axes, hats
	BPrivate::GamepadDevice device;
	memset(&device, 0, sizeof(device));
	device.keyBits[BTN_SOUTH / 8] |= 1 << (BTN_SOUTH % 8);
	device.keyBits[BTN_EAST / 8] |= 1 << (BTN_EAST % 8);
	uint32 codes[] = { ABS_X, ABS_Y, ABS_Z };
	for (int i = 0; i < 3; i++) {
		device.absBits[codes[i] / 8] |= 1 << (codes[i] % 8);
		device.abs[codes[i]].minimum = -100;
		device.abs[codes[i]].maximum = 100;
	}
	device.Enumerate();
	device.abs[ABS_X].value = 100;
	device.abs[ABS_Y].value = -100;
	device.abs[ABS_Z].value = 100;
	device.keyState[BTN_EAST / 8] |= 1 << (BTN_EAST % 8);

	BPrivate::GamepadMapping mapping;
	bool parsed = mapping.Parse("0300000001000000020000000300000000,"
		"Format test,a:+a2,-leftx:b1,+leftx:b0,lefty:a1~,"
		"righttrigger:-a1,platform:Linux,");
	// the GUID above is 34 digits: invalid
	bool rejected = !parsed;
	parsed = mapping.Parse("03000000010000000200000003000000,"
		"Format test,a:+a2,-leftx:b1,+leftx:b0,lefty:a1~,"
		"righttrigger:-a1,platform:Linux,");
	gamepad_state state;
	memset(&state, 0, sizeof(state));
	mapping.Apply(device, state);
	report("mapping_format", rejected && parsed
		&& (state.buttons & (1 << B_GAMEPAD_BUTTON_A)) != 0
		&& state.axes[B_GAMEPAD_AXIS_LEFT_X] == -32767
		&& state.axes[B_GAMEPAD_AXIS_LEFT_Y] == 32767
		&& state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER] == 32767,
		"a %d, left %d/%d, right trigger %d",
		(state.buttons & 1) != 0, state.axes[0], state.axes[1],
		state.axes[B_GAMEPAD_AXIS_RIGHT_TRIGGER]);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_xinput();
	test_hid();
	test_mapping_format();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
