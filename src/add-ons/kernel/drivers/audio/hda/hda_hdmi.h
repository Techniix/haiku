/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HDA_HDMI_H_
#define _HDA_HDMI_H_


/*!	Playback on HDMI and DisplayPort pins (roadmap F-31 phase 2): the codec
	set-up a digital display pin and its converter need besides what the
	hda driver does for analog outputs, as a list of verbs, so that it can
	be tested without hardware (3rdparty/ce/tests/hda_eld). Header-only and
	free of kernel headers, like hda_eld.h.

	Sources: the Intel High Definition Audio specification 1.0a with the
	HDMI/DisplayPort ECRs (verbs 72Dh converter channel count, 734h ASP
	channel mapping, 730h-732h data island packets), CEA-861-D section 6.6
	(the audio InfoFrame), the DisplayPort 1.2 audio InfoFrame SDP, and
	Linux's sound/pci/hda/patch_hdmi.c and hda_chmap (hdmi_setup_audio_
	infoframe(), hdmi_std_setup_channel_mapping(), hdmi_setup_stream()),
	which does the same in this order. FreeBSD's hdaa.c (hdaa_audio_setup)
	sends the same verbs.

	Scope of the first version: two channels (LPCM stereo, speaker
	allocation 0, front left and right), one monitor per codec at a time
	(the pin with a sink that reported an ELD, else one with a sink, else
	the first). Multichannel layouts and one stream per monitor on codecs
	with several pins are the next steps.
*/


#include "hda_eld.h"


// Verb identifiers (12-bit "set" verbs, as in hda_codec_defs.h)
#define HDA_HDMI_VERB_SET_PIN_WIDGET_CONTROL	0x707
#define HDA_HDMI_VERB_SET_DIGITAL_CONTROL1		0x70d
#define HDA_HDMI_VERB_SET_CHANNEL_COUNT			0x72d
#define HDA_HDMI_VERB_SET_DIP_INDEX				0x730
#define HDA_HDMI_VERB_SET_DIP_DATA				0x731
#define HDA_HDMI_VERB_SET_DIP_XMIT				0x732
#define HDA_HDMI_VERB_SET_ASP_MAPPING			0x734

#define HDA_HDMI_PIN_OUT_ENABLE		0x40
#define HDA_HDMI_DIGITAL_ENABLE		0x01
#define HDA_HDMI_DIP_XMIT_DISABLE	0x00
#define HDA_HDMI_DIP_XMIT_BEST		0xc0
#define HDA_HDMI_SLOTS				8
#define HDA_HDMI_MAX_INFOFRAME		16
#define HDA_HDMI_MAX_VERBS			48


struct hda_hdmi_verb {
	uint32	node;
	uint32	verb;		// 12-bit verb identifier
	uint32	payload;	// 8-bit payload
};


/*!	The audio InfoFrame for \a channels LPCM channels with the speaker
	allocation \a allocation: for HDMI the CEA-861-D frame (type 84h,
	version 1, length 10, checksum making all bytes add up to zero, then
	the payload), for DisplayPort the InfoFrame SDP header Linux uses
	(type 84h, length 1Bh, version 11h << 2) and the same payload without
	a checksum. Returns the byte count.
*/
static inline size_t
hda_hdmi_audio_infoframe(bool displayPort, uint8 channels, uint8 allocation,
	uint8* frame)
{
	memset(frame, 0, HDA_HDMI_MAX_INFOFRAME);
	uint8 count = channels > 0 ? (uint8)((channels - 1) & 0x7) : 0;
	if (displayPort) {
		frame[0] = 0x84;
		frame[1] = 0x1b;
		frame[2] = 0x11 << 2;
		frame[3] = count;			// CC (CT 0: refer to stream header)
		frame[4] = 0;				// SF, SS: refer to stream header
		frame[5] = 0;
		frame[6] = allocation;		// CA
		frame[7] = 0;				// LSV, DM_INH
		return 8;
	}

	frame[0] = 0x84;
	frame[1] = 0x01;
	frame[2] = 0x0a;
	frame[3] = 0;					// checksum, below
	frame[4] = count;				// PB1: CC, CT
	frame[5] = 0;					// PB2: SF, SS
	frame[6] = 0;					// PB3
	frame[7] = allocation;			// PB4: CA
	frame[8] = 0;					// PB5: LSV, DM_INH
	// PB6-PB10 reserved (zero)
	uint8 sum = 0;
	for (size_t i = 0; i < 14; i++)
		sum += frame[i];
	frame[3] = (uint8)(0x100 - sum);
	return 14;
}


/*!	The ASP channel mapping payloads (converter channel << 4 | slot) for
	\a channels channels in the standard order: converter channel n to
	slot n, unused slots get converter channel Fh (no channel).
*/
static inline void
hda_hdmi_channel_mapping(uint8 channels, uint8* payloads)
{
	for (uint8 slot = 0; slot < HDA_HDMI_SLOTS; slot++) {
		uint8 channel = slot < channels ? slot : 0xf;
		payloads[slot] = (uint8)(channel << 4 | slot);
	}
}


/*!	The verbs that make \a converter play \a channels LPCM channels on the
	display pin \a pin, after the converter's format and stream were set as
	for any output (hda_stream_setup_buffers()): pin output on, digital
	output on (and "non-audio" off), the converter's channel count, the
	channel mapping, then the audio InfoFrame (transmission off, index 0,
	the bytes, transmission on, best effort). Returns the count.
*/
static inline size_t
hda_hdmi_setup_verbs(uint32 pin, uint32 converter, bool displayPort,
	uint8 channels, hda_hdmi_verb* verbs, size_t maxVerbs)
{
	size_t count = 0;
#define HDA_HDMI_ADD(_node, _verb, _payload) \
	do { \
		if (count < maxVerbs) { \
			verbs[count].node = (_node); \
			verbs[count].verb = (_verb); \
			verbs[count].payload = (_payload); \
		} \
		count++; \
	} while (false)

	HDA_HDMI_ADD(pin, HDA_HDMI_VERB_SET_PIN_WIDGET_CONTROL,
		HDA_HDMI_PIN_OUT_ENABLE);
	HDA_HDMI_ADD(converter, HDA_HDMI_VERB_SET_DIGITAL_CONTROL1,
		HDA_HDMI_DIGITAL_ENABLE);
	HDA_HDMI_ADD(converter, HDA_HDMI_VERB_SET_CHANNEL_COUNT,
		channels > 0 ? channels - 1 : 0);

	uint8 mapping[HDA_HDMI_SLOTS];
	hda_hdmi_channel_mapping(channels, mapping);
	for (uint8 slot = 0; slot < HDA_HDMI_SLOTS; slot++)
		HDA_HDMI_ADD(converter, HDA_HDMI_VERB_SET_ASP_MAPPING, mapping[slot]);

	uint8 frame[HDA_HDMI_MAX_INFOFRAME];
	size_t size = hda_hdmi_audio_infoframe(displayPort, channels, 0, frame);
	HDA_HDMI_ADD(pin, HDA_HDMI_VERB_SET_DIP_XMIT, HDA_HDMI_DIP_XMIT_DISABLE);
	HDA_HDMI_ADD(pin, HDA_HDMI_VERB_SET_DIP_INDEX, 0);
		// packet 0 (the audio InfoFrame), byte 0
	for (size_t i = 0; i < size; i++)
		HDA_HDMI_ADD(pin, HDA_HDMI_VERB_SET_DIP_DATA, frame[i]);
	HDA_HDMI_ADD(pin, HDA_HDMI_VERB_SET_DIP_XMIT, HDA_HDMI_DIP_XMIT_BEST);
#undef HDA_HDMI_ADD
	return count;
}


//!	What is known about a display pin when one is chosen.
struct hda_hdmi_pin_state {
	uint32	node;
	bool	present;	// pin sense: presence detect
	bool	eld_valid;	// pin sense: ELD valid, and it parsed
};


/*!	The pin to play on: the first with a sink that reported a valid ELD,
	else the first with a sink, else the first. -1 for none.
*/
static inline int32
hda_hdmi_choose_pin(const hda_hdmi_pin_state* pins, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		if (pins[i].present && pins[i].eld_valid)
			return (int32)i;
	}
	for (uint32 i = 0; i < count; i++) {
		if (pins[i].present)
			return (int32)i;
	}
	return count > 0 ? 0 : -1;
}


/*!	The rates and sample formats (multi_audio B_SR_* and B_FMT_* bits, as
	their values) a converter offers that the sink takes too, from its
	ELD's LPCM descriptors. Without a usable ELD (none read, or nothing in
	common), what every HDMI sink must take: 32, 44.1 and 48 kHz, 16-bit
	(CEA-861-D basic audio), as far as the converter offers it.
*/
static inline void
hda_hdmi_limit_formats(const hda_eld* eld, uint32 converterRates,
	uint32 converterFormats, uint32* _rates, uint32* _formats)
{
	const uint32 kBasicRates = 0x40 | 0x80 | 0x100;
		// B_SR_32000 | B_SR_44100 | B_SR_48000
	const uint32 kFormat16 = 0x10, kFormat20 = 0x40, kFormat24 = 0x100;
		// B_FMT_16BIT, B_FMT_20BIT, B_FMT_24BIT
	uint32 rates = converterRates & kBasicRates;
	uint32 formats = converterFormats & kFormat16;
	if (eld != NULL) {
		uint32 eldRates = hda_eld_hmulti_rates(*eld) & converterRates;
		uint32 eldFormats = 0;
		if ((eld->lpcm_bits & HDA_ELD_BITS_16) != 0)
			eldFormats |= kFormat16;
		if ((eld->lpcm_bits & HDA_ELD_BITS_20) != 0)
			eldFormats |= kFormat20;
		if ((eld->lpcm_bits & HDA_ELD_BITS_24) != 0)
			eldFormats |= kFormat24;
		eldFormats &= converterFormats;
		if (eldRates != 0 && eldFormats != 0) {
			rates = eldRates;
			formats = eldFormats;
		}
	}
	*_rates = rates;
	*_formats = formats;
}


#endif	// _HDA_HDMI_H_
