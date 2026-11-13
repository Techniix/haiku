/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * XInput packet parsing and building; see XInputProtocol.h.
 */


#include "XInputProtocol.h"

#include <string.h>


#include "XInputDevices.h"


// GIP command bytes and options
enum {
	GIP_CMD_ACK			= 0x01,
	GIP_CMD_ANNOUNCE	= 0x02,
	GIP_CMD_IDENTIFY	= 0x04,
	GIP_CMD_POWER		= 0x05,
	GIP_CMD_AUTHENTICATE = 0x06,
	GIP_CMD_VIRTUAL_KEY	= 0x07,
	GIP_CMD_RUMBLE		= 0x09,
	GIP_CMD_LED			= 0x0a,
	GIP_CMD_FIRMWARE	= 0x0c,
	GIP_CMD_INPUT		= 0x20,

	GIP_OPT_ACK			= 0x10,
	GIP_OPT_INTERNAL	= 0x20,

	GIP_MOTOR_ALL		= 0x0f
};


static inline int16
le16(const uint8* data)
{
	return (int16)(data[0] | (data[1] << 8));
}


static inline uint16
ule16(const uint8* data)
{
	return (uint16)(data[0] | (data[1] << 8));
}


static inline void
report(const xinput_sink& sink, uint16 type, uint16 code, int32 value)
{
	sink.report(sink.cookie, type, code, value);
}


static inline void
key(const xinput_sink& sink, uint16 code, bool down)
{
	report(sink, EV_KEY, code, down ? 1 : 0);
}


xinput_type
xinput_type_for_interface(uint8 interfaceClass, uint8 subclass,
	uint8 protocol)
{
	if (interfaceClass != 0xff)
		return XINPUT_TYPE_NONE;
	if (subclass == 0x5d && protocol == 0x01)
		return XINPUT_TYPE_XBOX360;
	if (subclass == 0x5d && protocol == 0x81)
		return XINPUT_TYPE_XBOX360W;
	if (subclass == 0x47 && protocol == 0xd0)
		return XINPUT_TYPE_XBOXONE;
	return XINPUT_TYPE_NONE;
}


const xinput_known_device*
xinput_find_device(uint16 vendor, uint16 product)
{
	for (size_t i = 0; i < sizeof(kXInputDevices) / sizeof(kXInputDevices[0]);
			i++) {
		if (kXInputDevices[i].vendor == vendor
			&& kXInputDevices[i].product == product)
			return &kXInputDevices[i];
	}
	return NULL;
}


void
xinput_fill_info(xinput_type type, uint16 vendor, uint16 product,
	uint16 version, uint32 mapping, const char* name,
	gamepad_device_info* info)
{
	memset(info, 0, sizeof(*info));
	strlcpy(info->name, name != NULL ? name : "Generic X-Box pad",
		sizeof(info->name));
	info->id.bustype = BUS_USB;
	info->id.vendor = vendor;
	// like xpad: pads behind a wireless receiver have their own product id
	info->id.product = type == XINPUT_TYPE_XBOX360W ? 0x02a1 : product;
	info->id.version = version;

	if ((mapping & XINPUT_MAP_STICKS_TO_NULL) == 0) {
		gamepad_set_abs(info, ABS_X, -32768, 32767, 16, 128);
		gamepad_set_abs(info, ABS_Y, -32768, 32767, 16, 128);
		gamepad_set_abs(info, ABS_RX, -32768, 32767, 16, 128);
		gamepad_set_abs(info, ABS_RY, -32768, 32767, 16, 128);
	}

	static const uint16 kCommon[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_START,
		BTN_SELECT, BTN_THUMBL, BTN_THUMBR, BTN_TL, BTN_TR, BTN_MODE };
	for (size_t i = 0; i < sizeof(kCommon) / sizeof(kCommon[0]); i++)
		gamepad_set_key(info, kCommon[i]);
	if ((mapping & XINPUT_MAP_SHARE_BUTTON) != 0)
		gamepad_set_key(info, KEY_RECORD);

	if ((mapping & XINPUT_MAP_DPAD_TO_BUTTONS) != 0) {
		gamepad_set_key(info, BTN_DPAD_LEFT);
		gamepad_set_key(info, BTN_DPAD_RIGHT);
		gamepad_set_key(info, BTN_DPAD_UP);
		gamepad_set_key(info, BTN_DPAD_DOWN);
	}
	if ((mapping & XINPUT_MAP_PADDLES) != 0) {
		gamepad_set_key(info, BTN_GRIPR);
		gamepad_set_key(info, BTN_GRIPR2);
		gamepad_set_key(info, BTN_GRIPL);
		gamepad_set_key(info, BTN_GRIPL2);
	}
	if ((mapping & XINPUT_MAP_DPAD_TO_BUTTONS) == 0
		|| type == XINPUT_TYPE_XBOX360W) {
		gamepad_set_abs(info, ABS_HAT0X, -1, 1, 0, 0);
		gamepad_set_abs(info, ABS_HAT0Y, -1, 1, 0, 0);
	}
	if ((mapping & XINPUT_MAP_TRIGGERS_TO_BUTTONS) != 0) {
		gamepad_set_key(info, BTN_TL2);
		gamepad_set_key(info, BTN_TR2);
	} else {
		int32 maximum = type == XINPUT_TYPE_XBOXONE ? 1023 : 255;
		gamepad_set_abs(info, ABS_Z, 0, maximum, 0, 0);
		gamepad_set_abs(info, ABS_RZ, 0, maximum, 0, 0);
	}
	if ((mapping & XINPUT_MAP_PROFILE_BUTTON) != 0)
		gamepad_set_abs(info, ABS_PROFILE, 0, 4, 0, 0);

	gamepad_set_bit(info->ev_bits, EV_FF);
	gamepad_set_bit(info->ff_bits, FF_RUMBLE);
	gamepad_set_bit(info->ff_bits, FF_PERIODIC);
	gamepad_set_bit(info->ff_bits, FF_SINE);
	gamepad_set_bit(info->ff_bits, FF_CONSTANT);
	gamepad_set_bit(info->ff_bits, FF_GAIN);
}


static void
report_dpad(const xinput_sink& sink, uint8 bits, xinput_type type,
	uint32 mapping)
{
	// bit 0 up, 1 down, 2 left, 3 right
	if ((mapping & XINPUT_MAP_DPAD_TO_BUTTONS) != 0) {
		key(sink, BTN_DPAD_LEFT, (bits & 0x04) != 0);
		key(sink, BTN_DPAD_RIGHT, (bits & 0x08) != 0);
		key(sink, BTN_DPAD_UP, (bits & 0x01) != 0);
		key(sink, BTN_DPAD_DOWN, (bits & 0x02) != 0);
	}
	if ((mapping & XINPUT_MAP_DPAD_TO_BUTTONS) == 0
		|| type == XINPUT_TYPE_XBOX360W) {
		report(sink, EV_ABS, ABS_HAT0X,
			!!(bits & 0x08) - !!(bits & 0x04));
		report(sink, EV_ABS, ABS_HAT0Y,
			!!(bits & 0x02) - !!(bits & 0x01));
	}
}


bool
xinput_parse_360(const uint8* data, size_t length, xinput_type type,
	uint32 mapping, const xinput_sink& sink)
{
	// byte 0: message type (0 = input), byte 1: length (0x14)
	if (length < 14 || data[0] != 0x00)
		return false;

	report_dpad(sink, data[2], type, mapping);
	key(sink, BTN_START, (data[2] & 0x10) != 0);
	key(sink, BTN_SELECT, (data[2] & 0x20) != 0);
	key(sink, BTN_THUMBL, (data[2] & 0x40) != 0);
	key(sink, BTN_THUMBR, (data[2] & 0x80) != 0);

	key(sink, BTN_A, (data[3] & 0x10) != 0);
	key(sink, BTN_B, (data[3] & 0x20) != 0);
	key(sink, BTN_X, (data[3] & 0x40) != 0);
	key(sink, BTN_Y, (data[3] & 0x80) != 0);
	key(sink, BTN_TL, (data[3] & 0x01) != 0);
	key(sink, BTN_TR, (data[3] & 0x02) != 0);
	key(sink, BTN_MODE, (data[3] & 0x04) != 0);

	if ((mapping & XINPUT_MAP_STICKS_TO_NULL) == 0) {
		// Y axes are up-positive on the wire; evdev wants down-positive
		report(sink, EV_ABS, ABS_X, le16(data + 6));
		report(sink, EV_ABS, ABS_Y, ~le16(data + 8));
		report(sink, EV_ABS, ABS_RX, le16(data + 10));
		report(sink, EV_ABS, ABS_RY, ~le16(data + 12));
	}

	if ((mapping & XINPUT_MAP_TRIGGERS_TO_BUTTONS) != 0) {
		key(sink, BTN_TL2, data[4] != 0);
		key(sink, BTN_TR2, data[5] != 0);
	} else {
		report(sink, EV_ABS, ABS_Z, data[4]);
		report(sink, EV_ABS, ABS_RZ, data[5]);
	}

	report(sink, EV_SYN, SYN_REPORT, 0);
	return true;
}


bool
xinput_parse_360w(const uint8* data, size_t length, uint32 mapping,
	bool padReady, bool& presenceChanged, bool& present,
	const xinput_sink& sink)
{
	presenceChanged = false;
	if (length < 2)
		return false;

	// byte 0 bit 3: presence change; byte 1 bit 7: pad present
	if ((data[0] & 0x08) != 0) {
		presenceChanged = true;
		present = (data[1] & 0x80) != 0;
	}

	// byte 1 == 1: pad state follows from byte 4
	if (data[1] != 0x01 || !padReady || length < 4 + 14)
		return presenceChanged;

	return xinput_parse_360(data + 4, length - 4, XINPUT_TYPE_XBOX360W,
		mapping, sink);
}


bool
xinput_parse_gip(const uint8* data, size_t length, uint32 mapping,
	const xinput_sink& sink, uint8* ack, size_t& ackLength, bool& announce)
{
	ackLength = 0;
	announce = false;
	if (length < 4)
		return false;

	switch (data[0]) {
		case GIP_CMD_VIRTUAL_KEY:
		{
			if (length < 5)
				return false;
			if (data[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL)) {
				// the Xbox One S repeats this until it is acknowledged
				static const uint8 kAck[] = {
					GIP_CMD_ACK, GIP_OPT_INTERNAL, 0x00, 9,
					0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02, 0x00,
					0x00, 0x00, 0x00, 0x00
				};
				memcpy(ack, kAck, sizeof(kAck));
				ack[2] = data[2];
				ackLength = sizeof(kAck);
			}
			key(sink, BTN_MODE, (data[4] & 0x03) != 0);
			report(sink, EV_SYN, SYN_REPORT, 0);
			return true;
		}

		case GIP_CMD_ANNOUNCE:
			announce = true;
			return true;

		case GIP_CMD_INPUT:
		{
			if (length < 18)
				return false;
			key(sink, BTN_START, (data[4] & 0x04) != 0);
			key(sink, BTN_SELECT, (data[4] & 0x08) != 0);
			if ((mapping & XINPUT_MAP_SHARE_BUTTON) != 0) {
				size_t offset = (mapping & XINPUT_MAP_SHARE_OFFSET) != 0
					? 26 : 18;
				if (length >= offset)
					key(sink, KEY_RECORD, (data[length - offset] & 0x01) != 0);
			}
			key(sink, BTN_A, (data[4] & 0x10) != 0);
			key(sink, BTN_B, (data[4] & 0x20) != 0);
			key(sink, BTN_X, (data[4] & 0x40) != 0);
			key(sink, BTN_Y, (data[4] & 0x80) != 0);

			report_dpad(sink, data[5], XINPUT_TYPE_XBOXONE, mapping);
			key(sink, BTN_TL, (data[5] & 0x10) != 0);
			key(sink, BTN_TR, (data[5] & 0x20) != 0);
			key(sink, BTN_THUMBL, (data[5] & 0x40) != 0);
			key(sink, BTN_THUMBR, (data[5] & 0x80) != 0);

			if ((mapping & XINPUT_MAP_STICKS_TO_NULL) == 0) {
				report(sink, EV_ABS, ABS_X, le16(data + 10));
				report(sink, EV_ABS, ABS_Y, ~le16(data + 12));
				report(sink, EV_ABS, ABS_RX, le16(data + 14));
				report(sink, EV_ABS, ABS_RY, ~le16(data + 16));
			}

			if ((mapping & XINPUT_MAP_TRIGGERS_TO_BUTTONS) != 0) {
				key(sink, BTN_TL2, ule16(data + 6) != 0);
				key(sink, BTN_TR2, ule16(data + 8) != 0);
			} else {
				report(sink, EV_ABS, ABS_Z, ule16(data + 6));
				report(sink, EV_ABS, ABS_RZ, ule16(data + 8));
			}

			if ((mapping & XINPUT_MAP_PROFILE_BUTTON) != 0 && length > 34)
				report(sink, EV_ABS, ABS_PROFILE, data[34]);

			if ((mapping & XINPUT_MAP_PADDLES) != 0 && length > 33) {
				// first Elite firmware layout; later firmware sends the
				// paddles elsewhere (not handled yet)
				key(sink, BTN_GRIPR, (data[32] & 0x02) != 0);
				key(sink, BTN_GRIPR2, (data[32] & 0x08) != 0);
				key(sink, BTN_GRIPL, (data[32] & 0x01) != 0);
				key(sink, BTN_GRIPL2, (data[32] & 0x04) != 0);
			}

			report(sink, EV_SYN, SYN_REPORT, 0);
			return true;
		}
	}
	return false;
}


size_t
xinput_build_rumble(xinput_type type, uint16 strong, uint16 weak,
	uint8 sequence, uint8* packet)
{
	switch (type) {
		case XINPUT_TYPE_XBOX360:
			memset(packet, 0, 8);
			packet[1] = 0x08;
			packet[3] = strong / 256;
			packet[4] = weak / 256;
			return 8;

		case XINPUT_TYPE_XBOX360W:
			memset(packet, 0, 12);
			packet[1] = 0x01;
			packet[2] = 0x0f;
			packet[3] = 0xc0;
			packet[5] = strong / 256;
			packet[6] = weak / 256;
			return 12;

		case XINPUT_TYPE_XBOXONE:
			memset(packet, 0, 13);
			packet[0] = GIP_CMD_RUMBLE;
			packet[2] = sequence;
			packet[3] = 9;
			packet[5] = GIP_MOTOR_ALL;
			packet[8] = strong / 512;
			packet[9] = weak / 512;
			packet[10] = 0xff;	// on period
			packet[12] = 0xff;	// repeat count
			return 13;

		default:
			return 0;
	}
}


size_t
xinput_build_led(xinput_type type, int32 player, uint8* packet)
{
	// LED patterns: 0 off, 2-5 "flash, then quadrant 1-4 on"
	uint8 command = player > 0 ? (uint8)(((player - 1) % 4) + 2) : 0;
	switch (type) {
		case XINPUT_TYPE_XBOX360:
			packet[0] = 0x01;
			packet[1] = 0x03;
			packet[2] = command;
			return 3;

		case XINPUT_TYPE_XBOX360W:
			memset(packet, 0, 12);
			packet[2] = 0x08;
			packet[3] = 0x40 + command;
			return 12;

		default:
			// Xbox One pads have no player LEDs
			return 0;
	}
}


size_t
xinput_build_presence_query(uint8* packet)
{
	memset(packet, 0, 12);
	packet[0] = 0x08;
	packet[2] = 0x0f;
	packet[3] = 0xc0;
	return 12;
}


// #pragma mark - GIP init sequence


static const uint8 kPowerOn[] = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 1,
	0x00 };
static const uint8 kXboxOneSInit[] = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0,
	0x0f, 0x06 };
static const uint8 kExtraInput[] = { 0x4d, 0x10, 0x01, 0x02, 0x07, 0x00 };
static const uint8 kHoriAckId[] = { GIP_CMD_ACK, GIP_OPT_INTERNAL, 0, 9,
	0x00, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0x3a, 0x00, 0x00, 0x00, 0x80,
	0x00 };
static const uint8 kLedOn[] = { GIP_CMD_LED, GIP_OPT_INTERNAL, 0, 3, 0x00,
	0x01, 0x14 };
static const uint8 kAuthDone[] = { GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, 0,
	2, 0x01, 0x00 };
static const uint8 kRumbleBegin[] = { GIP_CMD_RUMBLE, 0x00, 0, 9, 0x00,
	GIP_MOTOR_ALL, 0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00 };
static const uint8 kRumbleEnd[] = { GIP_CMD_RUMBLE, 0x00, 0, 9, 0x00,
	GIP_MOTOR_ALL, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

struct gip_init_packet {
	uint16			vendor;		// 0 = every pad
	uint16			product;
	const uint8*	data;
	size_t			length;
};

#define GIP_INIT(vendor, product, data) \
	{ vendor, product, data, sizeof(data) }

static const gip_init_packet kGipInitPackets[] = {
	GIP_INIT(0x0e6f, 0x0165, kHoriAckId),
	GIP_INIT(0x0f0d, 0x0067, kHoriAckId),
	GIP_INIT(0x0000, 0x0000, kPowerOn),
	GIP_INIT(0x045e, 0x02ea, kXboxOneSInit),
	GIP_INIT(0x045e, 0x0b00, kXboxOneSInit),
	GIP_INIT(0x045e, 0x0b00, kExtraInput),
	GIP_INIT(0x0000, 0x0000, kLedOn),
	GIP_INIT(0x0000, 0x0000, kAuthDone),
	GIP_INIT(0x24c6, 0x541a, kRumbleBegin),
	GIP_INIT(0x24c6, 0x542a, kRumbleBegin),
	GIP_INIT(0x24c6, 0x543a, kRumbleBegin),
	GIP_INIT(0x24c6, 0x541a, kRumbleEnd),
	GIP_INIT(0x24c6, 0x542a, kRumbleEnd),
	GIP_INIT(0x24c6, 0x543a, kRumbleEnd),
};


size_t
xinput_gip_init_packet(uint16 vendor, uint16 product, int32 index,
	uint8 sequence, uint8* packet)
{
	int32 found = 0;
	for (size_t i = 0;
			i < sizeof(kGipInitPackets) / sizeof(kGipInitPackets[0]); i++) {
		const gip_init_packet& init = kGipInitPackets[i];
		if (init.vendor != 0
			&& (init.vendor != vendor || init.product != product))
			continue;
		if (found++ != index)
			continue;
		memcpy(packet, init.data, init.length);
		if (init.data != kExtraInput)
			packet[2] = sequence;
		return init.length;
	}
	return 0;
}
