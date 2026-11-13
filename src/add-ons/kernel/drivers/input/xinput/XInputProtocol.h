/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * The XInput protocols, without any USB or kernel code, so the harness can
 * test them in userland with recorded packets (tests/gamepad).
 *
 * Three protocols, told apart by the USB interface (class 0xff):
 *  - Xbox 360 wired (subclass 0x5d, protocol 0x01): 20-byte input reports,
 *    rumble and LED output reports. Third-party pads such as the 8BitDo
 *    Ultimate 2C additionally wait for a vendor control request (IN, request
 *    0x01, value 0x0100, 20 bytes) before they consider themselves set up;
 *    without it they switch themselves off after a while.
 *  - Xbox 360 wireless receiver (subclass 0x5d, protocol 0x81): one
 *    interface per pad slot; presence packets say when a pad connects or
 *    disconnects, so pads come and go while the receiver stays plugged in.
 *  - Xbox One/Series "GIP" (subclass 0x47, protocol 0xd0): a power-on packet
 *    and, for some pads, more init packets, before input; the guide button
 *    comes in its own packet that must be acknowledged.
 *
 * The packet layouts follow Linux's xpad driver's documented behaviour and
 * its device table; this is a separate implementation under the MIT
 * license.
 */
#ifndef XINPUT_PROTOCOL_H
#define XINPUT_PROTOCOL_H


#include <SupportDefs.h>

#include <gamepad_module.h>


enum xinput_type {
	XINPUT_TYPE_NONE = 0,
	XINPUT_TYPE_XBOX360,
	XINPUT_TYPE_XBOX360W,
	XINPUT_TYPE_XBOXONE
};

enum {
	XINPUT_MAP_DPAD_TO_BUTTONS		= 0x0001,
	XINPUT_MAP_TRIGGERS_TO_BUTTONS	= 0x0002,
	XINPUT_MAP_STICKS_TO_NULL		= 0x0004,
	XINPUT_MAP_SHARE_BUTTON			= 0x0008,
	XINPUT_MAP_PADDLES				= 0x0010,
	XINPUT_MAP_PROFILE_BUTTON		= 0x0020,
	XINPUT_MAP_SHARE_OFFSET			= 0x0040,
	XINPUT_DELAY_INIT				= 0x0100
};

#define XINPUT_DANCEPAD_MAP_CONFIG	(XINPUT_MAP_DPAD_TO_BUTTONS \
	| XINPUT_MAP_TRIGGERS_TO_BUTTONS | XINPUT_MAP_STICKS_TO_NULL)

struct xinput_known_device {
	uint16		vendor;
	uint16		product;
	uint8		type;
	uint16		mapping;
	const char*	name;
};

typedef void (*xinput_report_hook)(void* cookie, uint16 type, uint16 code,
	int32 value);

struct xinput_sink {
	xinput_report_hook	report;
	void*				cookie;
};

#define XINPUT_MAX_PACKET	64


xinput_type		xinput_type_for_interface(uint8 interfaceClass,
					uint8 subclass, uint8 protocol);
const xinput_known_device* xinput_find_device(uint16 vendor, uint16 product);

// Fills name, id and capabilities the way xpad sets them up.
void			xinput_fill_info(xinput_type type, uint16 vendor,
					uint16 product, uint16 version, uint32 mapping,
					const char* name, gamepad_device_info* info);

// Xbox 360 wired (and the pad part of wireless) input report.
bool			xinput_parse_360(const uint8* data, size_t length,
					xinput_type type, uint32 mapping, const xinput_sink& sink);

// Wireless receiver packet. Sets presenceChanged/present when the packet
// says a pad connected or disconnected; passes pad data to the sink if the
// pad is present (padReady).
bool			xinput_parse_360w(const uint8* data, size_t length,
					uint32 mapping, bool padReady, bool& presenceChanged,
					bool& present, const xinput_sink& sink);

// GIP packet. If the pad asks for an acknowledgement, ack/ackLength are
// filled in (ackLength 0 otherwise). announce is set for GIP_CMD_ANNOUNCE.
bool			xinput_parse_gip(const uint8* data, size_t length,
					uint32 mapping, const xinput_sink& sink, uint8* ack,
					size_t& ackLength, bool& announce);

// Output packets; return the length (0 = nothing to send).
size_t			xinput_build_rumble(xinput_type type, uint16 strong,
					uint16 weak, uint8 sequence, uint8* packet);
size_t			xinput_build_led(xinput_type type, int32 player,
					uint8* packet);
size_t			xinput_build_presence_query(uint8* packet);

// The GIP init packets for a pad, in order; index from 0 until it returns
// 0. The sequence number is filled in.
size_t			xinput_gip_init_packet(uint16 vendor, uint16 product,
					int32 index, uint8 sequence, uint8* packet);


#endif	// XINPUT_PROTOCOL_H
