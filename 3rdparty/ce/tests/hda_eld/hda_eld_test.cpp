/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Tests the hda driver's ELD parser (src/add-ons/kernel/drivers/audio/hda/
	hda_eld.h), which reads what HDMI and DisplayPort monitors report about
	their audio. QEMU emulates no HDMI codec, so the ELDs here are built
	byte by byte after the layout of the HDA specification's ELD memory
	structure and CEA-861-D's Short Audio Descriptors, as three typical
	sinks report them:

	- a TV over HDMI: "SAMSUNG TV", 2-channel LPCM 32-48 kHz 16/20/24-bit,
	  AC-3 5.1, DTS 5.1, speaker allocation FL/FR, LFE, FC, RL/RR
	- a monitor over DisplayPort: "DELL U2415", LPCM 2 channels 44.1/48 kHz,
	  16-bit
	- an AV receiver: no name, 8-channel LPCM up to 192 kHz

	and malformed ones (wrong version, a name longer than 16 bytes, more
	SADs than fit, a baseline longer than the data, no LPCM), which must be
	refused, as Linux's snd_hdmi_parse_eld() refuses them.
*/


#include <SupportDefs.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "hda_eld.h"
#include "hda_hdmi.h"


#define SUITE "hda_eld"

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


struct Sad {
	uint8 format, channels, rates, extra;
};


/*!	An ELD as a codec returns it: header, baseline block with the name and
	the SADs, padded to whole 4-byte units (Baseline_ELD_Len).
*/
static std::vector<uint8>
make_eld(uint8 version, bool displayPort, const char* name,
	uint16 manufacturer, uint16 product, uint8 speakers,
	const std::vector<Sad>& sads, int nameLengthOverride = -1,
	int sadCountOverride = -1)
{
	std::vector<uint8> eld(HDA_ELD_FIXED_SIZE, 0);
	size_t nameLength = strlen(name);
	eld[0] = version << 3;
	eld[4] = (3 << 5) | (nameLengthOverride >= 0 ? nameLengthOverride
		: nameLength);
	eld[5] = ((sadCountOverride >= 0 ? sadCountOverride : sads.size()) << 4)
		| ((displayPort ? 1 : 0) << 2) | 0x2;
	eld[6] = 10;	// 20 ms
	eld[7] = speakers;
	eld[16] = manufacturer >> 8;
	eld[17] = manufacturer & 0xff;
	eld[18] = product & 0xff;
	eld[19] = product >> 8;
	for (size_t i = 0; i < nameLength; i++)
		eld.push_back(name[i]);
	for (size_t i = 0; i < sads.size(); i++) {
		eld.push_back(((sads[i].format & 0xf) << 3)
			| ((sads[i].channels - 1) & 0x7));
		eld.push_back(sads[i].rates);
		eld.push_back(sads[i].extra);
	}
	while (eld.size() % 4 != 0)
		eld.push_back(0);
	eld[2] = (eld.size() - HDA_ELD_BASELINE_OFFSET) / 4;
	return eld;
}


static uint16
pnp(const char* id)
{
	return ((id[0] - '@') << 10) | ((id[1] - '@') << 5) | (id[2] - '@');
}


/*!	The HDMI/DisplayPort playback set-up (hda_hdmi.h): the audio InfoFrame
	bytes after CEA-861-D (HDMI) and the DisplayPort InfoFrame SDP as Linux
	sends them, the channel mapping, the verb sequence for a pin and its
	converter, the formats a sink takes, and which pin is chosen.
*/
static void
test_hdmi_playback(const std::vector<uint8>& tvEld)
{
	uint8 frame[HDA_HDMI_MAX_INFOFRAME];
	size_t size = hda_hdmi_audio_infoframe(false, 2, 0, frame);
	uint8 sum = 0;
	for (size_t i = 0; i < size; i++)
		sum += frame[i];
	// type 84h, version 1, length 10, checksum, CC = 1 (2 channels)
	static const uint8 kHdmiStereo[14] = { 0x84, 0x01, 0x0a, 0x70, 0x01, 0,
		0, 0, 0, 0, 0, 0, 0, 0 };
	report("hdmi_infoframe", size == 14 && sum == 0
		&& memcmp(frame, kHdmiStereo, 14) == 0,
		"%" B_PRIuSIZE " bytes, sum %#x, checksum %#x (want 14, 0, 0x70)",
		size, sum, frame[3]);

	size = hda_hdmi_audio_infoframe(false, 8, 0x13, frame);
	sum = 0;
	for (size_t i = 0; i < size; i++)
		sum += frame[i];
	report("hdmi_infoframe_8ch", size == 14 && sum == 0 && frame[4] == 7
		&& frame[7] == 0x13, "CC %u, CA %#x, sum %#x", frame[4], frame[7],
		sum);

	size = hda_hdmi_audio_infoframe(true, 2, 0, frame);
	static const uint8 kDpStereo[8] = { 0x84, 0x1b, 0x44, 0x01, 0, 0, 0, 0 };
	report("dp_infoframe", size == 8 && memcmp(frame, kDpStereo, 8) == 0,
		"%" B_PRIuSIZE " bytes: %02x %02x %02x %02x", size, frame[0], frame[1],
		frame[2], frame[3]);

	uint8 mapping[HDA_HDMI_SLOTS];
	hda_hdmi_channel_mapping(2, mapping);
	static const uint8 kStereoMapping[8] = { 0x00, 0x11, 0xf2, 0xf3, 0xf4,
		0xf5, 0xf6, 0xf7 };
	report("channel_mapping", memcmp(mapping, kStereoMapping, 8) == 0,
		"%02x %02x %02x ... %02x", mapping[0], mapping[1], mapping[2],
		mapping[7]);

	// Pin 5 with converter 2, HDMI, stereo: the order Linux's
	// hdmi_setup_stream()/hdmi_setup_audio_infoframe() use.
	hda_hdmi_verb verbs[HDA_HDMI_MAX_VERBS];
	size_t count = hda_hdmi_setup_verbs(5, 2, false, 2, verbs,
		HDA_HDMI_MAX_VERBS);
	bool ok = count == 3 + 8 + 2 + 14 + 1 && count <= HDA_HDMI_MAX_VERBS;
	if (ok) {
		ok = verbs[0].node == 5 && verbs[0].verb == 0x707
				&& verbs[0].payload == 0x40
			&& verbs[1].node == 2 && verbs[1].verb == 0x70d
				&& verbs[1].payload == 0x01
			&& verbs[2].node == 2 && verbs[2].verb == 0x72d
				&& verbs[2].payload == 1;
		for (int i = 0; i < 8 && ok; i++) {
			ok = verbs[3 + i].node == 2 && verbs[3 + i].verb == 0x734
				&& verbs[3 + i].payload == kStereoMapping[i];
		}
		ok = ok && verbs[11].node == 5 && verbs[11].verb == 0x732
			&& verbs[11].payload == 0 && verbs[12].verb == 0x730
			&& verbs[12].payload == 0;
		for (int i = 0; i < 14 && ok; i++) {
			ok = verbs[13 + i].node == 5 && verbs[13 + i].verb == 0x731
				&& verbs[13 + i].payload == kHdmiStereo[i];
		}
		ok = ok && verbs[27].node == 5 && verbs[27].verb == 0x732
			&& verbs[27].payload == 0xc0;
	}
	report("setup_verbs", ok, "%" B_PRIuSIZE " verbs (want 28: pin out, "
		"digital enable, channel count, 8 slots, InfoFrame off, index, 14 "
		"bytes, on)", count);
	count = hda_hdmi_setup_verbs(5, 2, true, 2, verbs, HDA_HDMI_MAX_VERBS);
	report("setup_verbs_dp", count == 3 + 8 + 2 + 8 + 1, "%" B_PRIuSIZE
		" verbs for DisplayPort (want 22)", count);

	hda_eld eld;
	hda_eld_parse(tvEld.data(), tvEld.size(), eld);
	uint32 rates, formats;
	// converter: 32-192 kHz, 16/20/24-bit
	hda_hdmi_limit_formats(&eld, 0x40 | 0x80 | 0x100 | 0x800 | 0x2000,
		0x10 | 0x40 | 0x100, &rates, &formats);
	report("formats_from_eld", rates == (0x40 | 0x80 | 0x100)
		&& formats == (0x10 | 0x40 | 0x100), "rates %#" B_PRIx32 ", formats %#"
		B_PRIx32 " (the TV: 32-48 kHz, 16/20/24-bit)", rates, formats);
	hda_hdmi_limit_formats(NULL, 0x40 | 0x80 | 0x100 | 0x800,
		0x10 | 0x100, &rates, &formats);
	report("formats_without_eld", rates == (0x40 | 0x80 | 0x100)
		&& formats == 0x10, "rates %#" B_PRIx32 ", formats %#" B_PRIx32
		" (basic audio: 32-48 kHz, 16-bit)", rates, formats);

	hda_hdmi_pin_state pins[3] = { { 3, false, false }, { 4, true, false },
		{ 5, true, true } };
	int32 chosen = hda_hdmi_choose_pin(pins, 3);
	pins[2].present = false;
	pins[2].eld_valid = false;
	int32 presentOnly = hda_hdmi_choose_pin(pins, 3);
	pins[1].present = false;
	int32 none = hda_hdmi_choose_pin(pins, 3);
	report("choose_pin", chosen == 2 && presentOnly == 1 && none == 0
		&& hda_hdmi_choose_pin(pins, 0) == -1, "ELD %" B_PRId32 ", sink only %"
		B_PRId32 ", nothing %" B_PRId32 " (want 2, 1, 0)", chosen, presentOnly,
		none);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	// HDMI TV
	std::vector<Sad> tvSads = {
		{ HDA_ELD_FORMAT_LPCM, 2, HDA_ELD_RATE_32000 | HDA_ELD_RATE_44100
			| HDA_ELD_RATE_48000, HDA_ELD_BITS_16 | HDA_ELD_BITS_20
			| HDA_ELD_BITS_24 },
		{ HDA_ELD_FORMAT_AC3, 6, HDA_ELD_RATE_32000 | HDA_ELD_RATE_44100
			| HDA_ELD_RATE_48000, 80 },
		{ HDA_ELD_FORMAT_DTS, 6, HDA_ELD_RATE_44100 | HDA_ELD_RATE_48000,
			192 },
	};
	std::vector<uint8> tv = make_eld(2, false, "SAMSUNG TV", pnp("SAM"),
		0x0e5d, 0x0f, tvSads);
	hda_eld eld;
	hda_eld_error error = hda_eld_parse(tv.data(), tv.size(), eld);
	char name[64] = "";
	hda_eld_output_name(eld, name, sizeof(name));
	report("hdmi_tv", error == HDA_ELD_OK && !eld.display_port
			&& strcmp(eld.monitor_name, "SAMSUNG TV") == 0
			&& strcmp(name, "HDMI: SAMSUNG TV") == 0
			&& eld.sad_count == 3 && eld.lpcm_channels == 2
			&& eld.lpcm_rates == 0x07 && eld.lpcm_bits == 0x7
			&& eld.sads[1].format == HDA_ELD_FORMAT_AC3
			&& eld.sads[1].channels == 6 && eld.speakers == 0x0f
			&& eld.audio_sync_delay == 20 && eld.supports_ai,
		"error %d, \"%s\", %u SADs, LPCM %u ch rates %#x bits %#x, "
		"speakers %#x", error, name, eld.sad_count, eld.lpcm_channels,
		eld.lpcm_rates, eld.lpcm_bits, eld.speakers);
	report("hdmi_tv_rates", hda_eld_hmulti_rates(eld)
			== (0x40u | 0x80u | 0x100u),
		"multi_audio rates %#x (32, 44.1, 48 kHz)",
		(unsigned)hda_eld_hmulti_rates(eld));

	// DisplayPort monitor, name padded with blanks
	std::vector<Sad> dpSads = {
		{ HDA_ELD_FORMAT_LPCM, 2, HDA_ELD_RATE_44100 | HDA_ELD_RATE_48000,
			HDA_ELD_BITS_16 },
	};
	std::vector<uint8> dp = make_eld(2, true, "DELL U2415  ", pnp("DEL"),
		0xa0b7, 0x01, dpSads);
	error = hda_eld_parse(dp.data(), dp.size(), eld);
	hda_eld_output_name(eld, name, sizeof(name));
	char manufacturer[4];
	hda_eld_manufacturer(eld, manufacturer);
	report("displayport_monitor", error == HDA_ELD_OK && eld.display_port
			&& strcmp(name, "DisplayPort: DELL U2415") == 0
			&& strcmp(manufacturer, "DEL") == 0 && eld.product == 0xa0b7
			&& eld.lpcm_rates == 0x06 && eld.lpcm_bits == HDA_ELD_BITS_16,
		"error %d, \"%s\" from %s %04x", error, name, manufacturer,
		eld.product);

	// AV receiver without a name: 8 channels up to 192 kHz
	std::vector<Sad> avrSads = {
		{ HDA_ELD_FORMAT_LPCM, 8, 0x7f, HDA_ELD_BITS_16 | HDA_ELD_BITS_24 },
		{ HDA_ELD_FORMAT_EAC3, 8, HDA_ELD_RATE_48000, 0 },
	};
	std::vector<uint8> avr = make_eld(2, false, "", pnp("ONK"), 0x1234, 0x7f,
		avrSads);
	error = hda_eld_parse(avr.data(), avr.size(), eld);
	hda_eld_output_name(eld, name, sizeof(name));
	report("receiver_no_name", error == HDA_ELD_OK
			&& strcmp(name, "HDMI: ONK 1234") == 0 && eld.lpcm_channels == 8
			&& hda_eld_hmulti_rates(eld) == (0x40u | 0x80u | 0x100u | 0x400u
				| 0x800u | 0x1000u | 0x2000u),
		"error %d, \"%s\", %u channels, rates %#x", error, name,
		eld.lpcm_channels, (unsigned)hda_eld_hmulti_rates(eld));

	// Known bad ones
	std::vector<uint8> bad = tv;
	bad[0] = 1 << 3;
	report("rejects_version", hda_eld_parse(bad.data(), bad.size(), eld)
		== HDA_ELD_BAD_VERSION, "ELD version 1");

	bad = make_eld(2, false, "A NAME THAT IS TOO LONG", pnp("SAM"), 1, 3,
		tvSads);
	report("rejects_long_name", hda_eld_parse(bad.data(), bad.size(), eld)
		== HDA_ELD_BAD_NAME, "a 23-byte monitor name");

	bad = make_eld(2, false, "TV", pnp("SAM"), 1, 3, tvSads, -1, 15);
	report("rejects_sad_count", hda_eld_parse(bad.data(), bad.size(), eld)
		== HDA_ELD_BAD_SAD_COUNT, "15 SADs announced, 3 there");

	bad = tv;
	bad[2] += 4;
	report("rejects_baseline", hda_eld_parse(bad.data(), bad.size(), eld)
		== HDA_ELD_BAD_BASELINE, "baseline 16 bytes longer than the data");

	report("rejects_short", hda_eld_parse(tv.data(), 12, eld)
		== HDA_ELD_TOO_SHORT, "12 bytes");

	std::vector<Sad> noLpcm = { { HDA_ELD_FORMAT_AC3, 6, 0x7, 80 } };
	bad = make_eld(2, false, "TV", pnp("SAM"), 1, 3, noLpcm);
	report("rejects_no_lpcm", hda_eld_parse(bad.data(), bad.size(), eld)
		== HDA_ELD_NO_LPCM, "only AC-3");

	test_hdmi_playback(tv);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
