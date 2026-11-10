/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HDA_ELD_H_
#define _HDA_ELD_H_


/*!	EDID-Like Data (ELD) of HDMI and DisplayPort sinks, as an HD Audio codec
	reports it for a digital pin (High Definition Audio specification 1.0a,
	7.3.3.34 "Get HDMI ELD Data", and the ELD memory structure of the HDA
	"ELD" ECR / version 2), with the CEA-861 Short Audio Descriptors it
	carries (CEA-861-D 7.5.2). Layout and checks follow Linux's
	sound/pci/hda/hda_eld.c (snd_hdmi_parse_eld()) and FreeBSD's hdaa.c.

	Header-only and free of kernel dependencies, so that the hda driver and
	the harness's guest test (3rdparty/ce/tests/hda_eld) use the same code.

	Header block:      byte 0  ELD_Ver (7:3)
	                   byte 2  Baseline_ELD_Len, in 4-byte units
	Baseline block (offsets from the start of the ELD):
	                   byte 4  CEA_EDID_Ver (7:5), MNL (4:0)
	                   byte 5  SAD_Count (7:4), Conn_Type (3:2), S_AI, HDCP
	                   byte 6  Aud_Synch_Delay (in 2 ms units)
	                   byte 7  Speaker allocation (CEA-861 7.5.3, byte 1)
	                   8-15    Port_ID
	                   16-17   Manufacturer name (EDID PNP ID)
	                   18-19   Product code
	                   20...   Monitor name (MNL bytes), then SAD_Count SADs
*/


#include <SupportDefs.h>
#include <stdio.h>
#include <string.h>


#define HDA_ELD_MAX_SADS		15
#define HDA_ELD_MAX_NAME		16
#define HDA_ELD_BASELINE_OFFSET	4
#define HDA_ELD_FIXED_SIZE		20

// CEA-861 audio format codes (SAD byte 0, bits 6..3)
enum {
	HDA_ELD_FORMAT_LPCM		= 1,
	HDA_ELD_FORMAT_AC3		= 2,
	HDA_ELD_FORMAT_MPEG1	= 3,
	HDA_ELD_FORMAT_MP3		= 4,
	HDA_ELD_FORMAT_MPEG2	= 5,
	HDA_ELD_FORMAT_AAC		= 6,
	HDA_ELD_FORMAT_DTS		= 7,
	HDA_ELD_FORMAT_ATRAC	= 8,
	HDA_ELD_FORMAT_DSD		= 9,
	HDA_ELD_FORMAT_EAC3		= 10,
	HDA_ELD_FORMAT_DTS_HD	= 11,
	HDA_ELD_FORMAT_MLP		= 12,
	HDA_ELD_FORMAT_DST		= 13,
	HDA_ELD_FORMAT_WMA_PRO	= 14
};

// SAD byte 1: sample rates
enum {
	HDA_ELD_RATE_32000		= 1 << 0,
	HDA_ELD_RATE_44100		= 1 << 1,
	HDA_ELD_RATE_48000		= 1 << 2,
	HDA_ELD_RATE_88200		= 1 << 3,
	HDA_ELD_RATE_96000		= 1 << 4,
	HDA_ELD_RATE_176400		= 1 << 5,
	HDA_ELD_RATE_192000		= 1 << 6
};

// SAD byte 2 for LPCM: sample sizes
enum {
	HDA_ELD_BITS_16			= 1 << 0,
	HDA_ELD_BITS_20			= 1 << 1,
	HDA_ELD_BITS_24			= 1 << 2
};

enum hda_eld_error {
	HDA_ELD_OK = 0,
	HDA_ELD_TOO_SHORT,
	HDA_ELD_BAD_VERSION,
	HDA_ELD_BAD_BASELINE,
	HDA_ELD_BAD_NAME,
	HDA_ELD_BAD_SAD_COUNT,
	HDA_ELD_NO_LPCM
};

struct hda_eld_sad {
	uint8	format;			// HDA_ELD_FORMAT_*
	uint8	channels;		// 1..8
	uint8	rates;			// HDA_ELD_RATE_*
	uint8	extra;			// LPCM: HDA_ELD_BITS_*; compressed: max kbit/s / 8
};

struct hda_eld {
	uint8			version;
	uint8			cea_version;
	bool			display_port;	// Conn_Type 1; 0 is HDMI
	bool			supports_ai;
	bool			hdcp;
	uint16			audio_sync_delay;	// ms
	uint8			speakers;			// CEA-861 speaker allocation byte
	uint16			manufacturer;		// EDID PNP ID, big-endian as in EDID
	uint16			product;
	char			monitor_name[HDA_ELD_MAX_NAME + 1];
	uint8			sad_count;
	hda_eld_sad		sads[HDA_ELD_MAX_SADS];
	// LPCM, from all LPCM descriptors together
	uint8			lpcm_channels;
	uint8			lpcm_rates;
	uint8			lpcm_bits;
};


/*!	Parses \a size bytes of ELD. Rejects what Linux rejects (wrong ELD
	version, a baseline block longer than the data, a monitor name longer
	than 16 bytes, more than 15 SADs or SADs that do not fit) and an ELD
	without any LPCM descriptor, which a sink must have (CEA-861-D 7.5.2:
	"the basic audio" is 2-channel LPCM, signalled separately, but every
	sink that reports SADs lists LPCM).
*/
static inline hda_eld_error
hda_eld_parse(const uint8* data, size_t size, hda_eld& eld)
{
	memset(&eld, 0, sizeof(eld));
	if (data == NULL || size < (size_t)HDA_ELD_FIXED_SIZE)
		return HDA_ELD_TOO_SHORT;

	eld.version = data[0] >> 3;
	// 2: ELD version 2 (CEA-861-D); 31: "vendor specific" which some
	// codecs report for DisplayPort and which Linux accepts too.
	if (eld.version != 2 && eld.version != 31)
		return HDA_ELD_BAD_VERSION;

	size_t baseline = (size_t)data[2] * 4;
	if (baseline == 0 || HDA_ELD_BASELINE_OFFSET + baseline > size)
		return HDA_ELD_BAD_BASELINE;
	size_t end = HDA_ELD_BASELINE_OFFSET + baseline;

	eld.cea_version = data[4] >> 5;
	uint8 nameLength = data[4] & 0x1f;
	eld.sad_count = data[5] >> 4;
	eld.display_port = ((data[5] >> 2) & 0x3) == 1;
	eld.supports_ai = (data[5] & 0x2) != 0;
	eld.hdcp = (data[5] & 0x1) != 0;
	eld.audio_sync_delay = data[6] * 2;
	eld.speakers = data[7];
	eld.manufacturer = (uint16)(data[16] << 8 | data[17]);
	eld.product = (uint16)(data[18] | data[19] << 8);

	if (nameLength > HDA_ELD_MAX_NAME)
		return HDA_ELD_BAD_NAME;
	if ((size_t)HDA_ELD_FIXED_SIZE + nameLength > end)
		return HDA_ELD_BAD_BASELINE;
	memcpy(eld.monitor_name, data + HDA_ELD_FIXED_SIZE, nameLength);
	eld.monitor_name[nameLength] = '\0';
	// some sinks pad with blanks or end early with a NUL
	for (int i = nameLength - 1; i >= 0
			&& (eld.monitor_name[i] == ' ' || eld.monitor_name[i] == '\0');
			i--) {
		eld.monitor_name[i] = '\0';
	}
	for (char* c = eld.monitor_name; *c != '\0'; c++) {
		if ((uint8)*c < 0x20 || (uint8)*c > 0x7e)
			*c = '?';
	}

	if (eld.sad_count > HDA_ELD_MAX_SADS)
		return HDA_ELD_BAD_SAD_COUNT;
	size_t sadStart = HDA_ELD_FIXED_SIZE + nameLength;
	if (sadStart + 3 * (size_t)eld.sad_count > end)
		return HDA_ELD_BAD_SAD_COUNT;

	for (uint8 i = 0; i < eld.sad_count; i++) {
		const uint8* sad = data + sadStart + 3 * i;
		hda_eld_sad& out = eld.sads[i];
		out.format = (sad[0] >> 3) & 0xf;
		out.channels = (sad[0] & 0x7) + 1;
		out.rates = sad[1] & 0x7f;
		out.extra = sad[2];
		if (out.format == HDA_ELD_FORMAT_LPCM) {
			if (out.channels > eld.lpcm_channels)
				eld.lpcm_channels = out.channels;
			eld.lpcm_rates |= out.rates;
			eld.lpcm_bits |= out.extra & 0x7;
		}
	}
	if (eld.lpcm_channels == 0)
		return HDA_ELD_NO_LPCM;
	return HDA_ELD_OK;
}


//!	The EDID PNP manufacturer ID ("DEL", "SAM") in \a buffer (4 bytes).
static inline void
hda_eld_manufacturer(const hda_eld& eld, char* buffer)
{
	buffer[0] = '@' + ((eld.manufacturer >> 10) & 0x1f);
	buffer[1] = '@' + ((eld.manufacturer >> 5) & 0x1f);
	buffer[2] = '@' + (eld.manufacturer & 0x1f);
	buffer[3] = '\0';
}


/*!	The name to show for the output: "HDMI: <monitor>" or "DisplayPort:
	<monitor>", the manufacturer ID if the sink gives no name.
*/
static inline void
hda_eld_output_name(const hda_eld& eld, char* buffer, size_t size)
{
	const char* kind = eld.display_port ? "DisplayPort" : "HDMI";
	if (eld.monitor_name[0] != '\0') {
		snprintf(buffer, size, "%s: %s", kind, eld.monitor_name);
		return;
	}
	char manufacturer[4];
	hda_eld_manufacturer(eld, manufacturer);
	snprintf(buffer, size, "%s: %s %04x", kind, manufacturer, eld.product);
}


/*!	The LPCM rates as multi_audio rates (B_SR_* from hmulti_audio.h, given
	as their values here so that this header needs nothing else).
*/
static inline uint32
hda_eld_hmulti_rates(const hda_eld& eld)
{
	static const struct { uint8 eld; uint32 hmulti; } kRates[] = {
		{ HDA_ELD_RATE_32000, 0x40 },		// B_SR_32000
		{ HDA_ELD_RATE_44100, 0x80 },		// B_SR_44100
		{ HDA_ELD_RATE_48000, 0x100 },		// B_SR_48000
		{ HDA_ELD_RATE_88200, 0x400 },		// B_SR_88200
		{ HDA_ELD_RATE_96000, 0x800 },		// B_SR_96000
		{ HDA_ELD_RATE_176400, 0x1000 },	// B_SR_176400
		{ HDA_ELD_RATE_192000, 0x2000 }		// B_SR_192000
	};
	uint32 rates = 0;
	for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
		if ((eld.lpcm_rates & kRates[i].eld) != 0)
			rates |= kRates[i].hmulti;
	}
	return rates;
}


#endif	// _HDA_ELD_H_
