/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_AUDIO_UAC2_H_
#define _USB_AUDIO_UAC2_H_


/*!	Definitions from the USB Audio Device Class 2.0 specification (UAC2,
	"Universal Serial Bus Device Class Definition for Audio Devices",
	release 2.0, May 31, 2006) that <usb/USB_audio.h> does not have. They
	are kept here, private to the driver, so that the public header stays
	as it is. Names follow the specification (and FreeBSD's uaudioreg.h,
	Linux's include/linux/usb/audio-v2.h, which use the same values).
*/


#include <SupportDefs.h>


// A.14 Audio Class-Specific Request Codes (UAC2). The direction is in
// bmRequestType: CUR and RANGE are used both ways.
enum {
	UAC2_CS_CUR						= 0x01,
	UAC2_CS_RANGE					= 0x02,
	UAC2_CS_MEM						= 0x03
};

// A.17.1 Clock Source Control Selectors
enum {
	UAC2_CS_SAM_FREQ_CONTROL		= 0x01,
	UAC2_CS_CLOCK_VALID_CONTROL		= 0x02
};

// A.17.2 Clock Selector Control Selectors
enum {
	UAC2_CX_CLOCK_SELECTOR_CONTROL	= 0x01
};

// 4.7.2.1 Clock Source bmAttributes, D1..0
enum {
	UAC2_CLOCK_EXTERNAL				= 0x00,
	UAC2_CLOCK_INTERNAL_FIXED		= 0x01,
	UAC2_CLOCK_INTERNAL_VARIABLE	= 0x02,
	UAC2_CLOCK_INTERNAL_PROGRAMMABLE = 0x03
};

// bmControls fields are two bits per control: 01 read-only, 11 host
// programmable (UAC2 4.1).
#define UAC2_CONTROL_READABLE(bits, index)	\
	((((bits) >> ((index) * 2)) & 0x1) != 0)
#define UAC2_CONTROL_PROGRAMMABLE(bits, index)	\
	((((bits) >> ((index) * 2)) & 0x3) == 0x3)

// A.6 Audio Class-Specific AS Interface Descriptor Subtypes are the same
// as UAC1 (AS_GENERAL 0x01, FORMAT_TYPE 0x02).

// Frmts20 A.2.1 Audio Data Format Type I Bit Allocations (bmFormats of
// the class-specific AS interface descriptor)
enum {
	UAC2_FORMAT_PCM					= 1 << 0,
	UAC2_FORMAT_PCM8				= 1 << 1,
	UAC2_FORMAT_IEEE_FLOAT			= 1 << 2,
	UAC2_FORMAT_ALAW				= 1 << 3,
	UAC2_FORMAT_MULAW				= 1 << 4,
	UAC2_FORMAT_RAW_DATA			= 1u << 31
};


// 4.7.2.1 Clock Source Descriptor (the same layout as
// usb_audio_clocksource_descriptor in USB_audio.h)
typedef struct {
	uint8	length;
	uint8	descriptor_type;		// USB_AUDIO_CS_INTERFACE
	uint8	descriptor_subtype;		// USB_AUDIO_AC_CLOCK_SOURCE_R2
	uint8	clock_id;
	uint8	attributes;
	uint8	controls;
	uint8	assoc_terminal;
	uint8	clock_source_string;
} _PACKED uac2_clock_source_descriptor;

// 4.7.2.2 Clock Selector Descriptor: bNrInPins entries of baCSourceID
// follow bNrInPins, then bmControls and iClockSelector.
typedef struct {
	uint8	length;
	uint8	descriptor_type;
	uint8	descriptor_subtype;		// USB_AUDIO_AC_CLOCK_SELECTOR_R2
	uint8	clock_id;
	uint8	num_input_pins;
	uint8	input_pins[1];
} _PACKED uac2_clock_selector_descriptor;

// 4.7.2.3 Clock Multiplier Descriptor
typedef struct {
	uint8	length;
	uint8	descriptor_type;
	uint8	descriptor_subtype;		// USB_AUDIO_AC_CLOCK_MULTIPLIER_R2
	uint8	clock_id;
	uint8	clock_source_id;
	uint8	controls;
	uint8	clock_multiplier_string;
} _PACKED uac2_clock_multiplier_descriptor;

// Frmts20 2.3.1.6 Type I Format Type Descriptor
typedef struct {
	uint8	length;
	uint8	descriptor_type;
	uint8	descriptor_subtype;		// USB_AUDIO_AS_FORMAT_TYPE
	uint8	format_type;			// USB_AUDIO_FORMAT_TYPE_I
	uint8	subslot_size;
	uint8	bit_resolution;
} _PACKED uac2_format_type_i_descriptor;

// 5.2.3.1 Layout 2 (16-bit) and 5.2.3.3 Layout 3 (32-bit) parameter
// blocks of RANGE requests: a count, then that many (min, max, res).
typedef struct {
	uint16	num_subranges;
	struct {
		int16	min;
		int16	max;
		uint16	res;
	} _PACKED subranges[1];
} _PACKED uac2_range_layout2;

typedef struct {
	uint16	num_subranges;
	struct {
		uint32	min;
		uint32	max;
		uint32	res;
	} _PACKED subranges[1];
} _PACKED uac2_range_layout3;


// Endpoint bmAttributes (USB 2.0, 9.6.6)
#define USB_EP_SYNC_MASK		0x0c
#define USB_EP_SYNC_NONE		0x00
#define USB_EP_SYNC_ASYNC		0x04
#define USB_EP_SYNC_ADAPTIVE	0x08
#define USB_EP_SYNC_SYNC		0x0c
#define USB_EP_USAGE_MASK		0x30
#define USB_EP_USAGE_DATA		0x00
#define USB_EP_USAGE_FEEDBACK	0x10
#define USB_EP_USAGE_IMPLICIT	0x20


#endif	// _USB_AUDIO_UAC2_H_
