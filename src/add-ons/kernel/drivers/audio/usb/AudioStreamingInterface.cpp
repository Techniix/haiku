/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Copyright 2026, Renga contributors.
 *	Distributed under the tems of the MIT license.
 *
 */


#include "AudioStreamingInterface.h"

#include <usb/USB_audio.h>

#include "Driver.h"
#include "Settings.h"
#include "UAC2.h"


static struct RatePair {
	uint32 rate;
	uint32 rateId;
} ratesMap[] = {
	{ 8000, B_SR_8000 },
	{ 11025, B_SR_11025 },
	{ 12000, B_SR_12000 },
	{ 16000, B_SR_16000 },
	{ 22050, B_SR_22050 },
	{ 24000, B_SR_24000 },
	{ 32000, B_SR_32000 },
	{ 44100, B_SR_44100 },
	{ 48000, B_SR_48000 },
	{ 64000, B_SR_64000 },
	{ 88200, B_SR_88200 },
	{ 96000, B_SR_96000 },
	{ 176400, B_SR_176400 },
	{ 192000, B_SR_192000 },
	{ 384000, B_SR_384000 },
	{ 1536000, B_SR_1536000 }
};


//	#pragma mark - AudioStreamAlternate


/*!	Parses one alternate setting. For UAC1 the class-specific AS interface
	descriptor (AS_GENERAL) gives the terminal link and the format tag, the
	format type descriptor the channels, subframe size, bit resolution and
	the rates. For UAC2 AS_GENERAL also carries the channel cluster
	(bNrChannels, bmChannelConfig) and a bitmap of formats; the format
	type descriptor only the subslot size and resolution. A Type I format
	with PCM, PCM8 or IEEE float samples is usable; everything else
	(Type II/III, compressed or raw data alternates) is skipped, as are
	alternates without an isochronous data endpoint (alternate 0, the zero
	bandwidth setting every streaming interface has).
*/
AudioStreamAlternate::AudioStreamAlternate(size_t alternate,
		uint16 specRelease, usb_interface_info* interface)
	:
	fAlternate(alternate),
	fUsable(false),
	fSpecRelease(specRelease),
	fTerminalLink(0),
	fFormatTag(0),
	fFormatType(USB_AUDIO_FORMAT_TYPE_UNDEFINED),
	fChannels(0),
	fChannelConfig(0),
	fSubslotSize(0),
	fBitResolution(0),
	fContinuous(false),
	fRateMin(0),
	fRateMax(0),
	fEndpointAddress(0),
	fEndpointAttributes(0),
	fMaxPacketSize(0),
	fInterval(0),
	fCSAttributes(0),
	fFeedbackAddress(0),
	fFeedbackMaxPacketSize(0),
	fFeedbackInterval(0)
{
	bool haveGeneral = false;
	bool haveFormat = false;
	bool isUAC2 = specRelease >= USB_AUDIO_CLASS_VERSION_2;

	for (size_t i = 0; i < interface->generic_count; i++) {
		uint8* data = (uint8*)interface->generic[i];
		uint8 length = data[0];
		uint8 type = data[1];

		if (type == USB_AUDIO_CS_ENDPOINT) {
			// UAC1 4.6.1.2 / UAC2 4.10.1.2: bmAttributes is the same byte
			// in both (D0 sampling frequency control in UAC1, D7
			// MaxPacketsOnly in both).
			if (length >= 4)
				fCSAttributes = data[3];
			continue;
		}
		if (type != USB_AUDIO_CS_INTERFACE || length < 3)
			continue;

		switch (data[2]) {
			case USB_AUDIO_AS_GENERAL:
			{
				if (haveGeneral)
					break;
				usb_audio_streaming_interface_descriptor* general
					= (usb_audio_streaming_interface_descriptor*)data;
				if (!isUAC2 && length >= 7) {
					fTerminalLink = general->terminal_link;
					fFormatTag = general->r1.format_tag;
					haveGeneral = true;
				} else if (isUAC2 && length >= 16) {
					fTerminalLink = general->terminal_link;
					fFormatType = general->r2.format_type;
					uint32 formats = general->r2.bm_formats;
					// The UAC1 format tag for the first (preferred)
					// format, so that the rest of the driver needs to
					// know only one way to describe it.
					if ((formats & UAC2_FORMAT_PCM) != 0)
						fFormatTag = USB_AUDIO_FORMAT_PCM;
					else if ((formats & UAC2_FORMAT_PCM8) != 0)
						fFormatTag = USB_AUDIO_FORMAT_PCM8;
					else if ((formats & UAC2_FORMAT_IEEE_FLOAT) != 0)
						fFormatTag = USB_AUDIO_FORMAT_IEEE_FLOAT;
					fChannels = general->r2.num_output_pins;
					fChannelConfig = general->r2.channel_config;
					haveGeneral = true;
				}
				break;
			}

			case USB_AUDIO_AS_FORMAT_TYPE:
			{
				if (haveFormat || length < 4)
					break;
				fFormatType = data[3];
				if (fFormatType != USB_AUDIO_FORMAT_TYPE_I)
					break;
				if (isUAC2) {
					if (length < 6)
						break;
					uac2_format_type_i_descriptor* format
						= (uac2_format_type_i_descriptor*)data;
					fSubslotSize = format->subslot_size;
					fBitResolution = format->bit_resolution;
					haveFormat = true;
					break;
				}

				if (length < 8)
					break;
				usb_audio_format_descriptor* format
					= (usb_audio_format_descriptor*)data;
				fChannels = format->typeI.nr_channels;
				fSubslotSize = format->typeI.subframe_size;
				fBitResolution = format->typeI.bit_resolution;
				uint8 count = format->typeI.sam_freq_type;
				// Never read past the descriptor, whatever the count says.
				size_t available = (length - 8) / 3;
				if (count == 0) {
					if (available >= 2) {
						uint32 a = GetSamFreq(format->typeI.sam_freqs[0]);
						uint32 b = GetSamFreq(format->typeI.sam_freqs[1]);
						fContinuous = true;
						fRateMin = min_c(a, b);
						fRateMax = max_c(a, b);
					}
				} else {
					for (size_t r = 0; r < count && r < available; r++) {
						fRates.PushBack(
							GetSamFreq(format->typeI.sam_freqs[r]));
					}
				}
				haveFormat = true;
				break;
			}

			default:
				break;
		}
	}

	_ParseEndpoints(interface);

	TRACE(UAC, "alt %d: link %d tag %#x type %d, %d ch (%#x), subslot %d, "
		"%d bits, ep %#x attr %#x max %d interval %d, feedback %#x, "
		"%s rates\n", (int)fAlternate, fTerminalLink, fFormatTag, fFormatType,
		fChannels, fChannelConfig, fSubslotSize, fBitResolution,
		fEndpointAddress, fEndpointAttributes, fMaxPacketSize, fInterval,
		fFeedbackAddress, fContinuous ? "continuous" : "discrete");

	if (!haveGeneral || !haveFormat || fEndpointAddress == 0)
		return;
	if (fFormatType != USB_AUDIO_FORMAT_TYPE_I) {
		TRACE(INF, "alternate %d: format type %d not supported\n",
			(int)fAlternate, fFormatType);
		return;
	}
	switch (fFormatTag) {
		case USB_AUDIO_FORMAT_PCM:
		case USB_AUDIO_FORMAT_PCM8:
		case USB_AUDIO_FORMAT_IEEE_FLOAT:
			break;
		default:
			TRACE(INF, "alternate %d: format %#x not supported\n",
				(int)fAlternate, fFormatTag);
			return;
	}
	if (fChannels == 0 || fChannels > 8) {
		TRACE(INF, "alternate %d: %d channels not supported\n",
			(int)fAlternate, fChannels);
		return;
	}
	// 3-byte subslots are converted to and from 4-byte samples in the
	// driver (Stream), so that the multi_audio add-on, which writes 4 bytes
	// for 24-bit samples, never sees them (Haiku ticket #20267).
	switch (fSubslotSize) {
		case 1: case 2: case 3: case 4:
			break;
		default:
			TRACE(INF, "alternate %d: subslot size %d not supported\n",
				(int)fAlternate, fSubslotSize);
			return;
	}
	if (fBitResolution == 0 || fBitResolution > fSubslotSize * 8)
		fBitResolution = fSubslotSize * 8;
	if (FormatId() == 0)
		return;

	fUsable = true;
}


AudioStreamAlternate::~AudioStreamAlternate()
{
}


/*!	Finds the isochronous data endpoint and, if there is one, the explicit
	feedback endpoint. UAC2 marks feedback endpoints in the usage bits of
	bmAttributes; UAC1 ones look like data endpoints but are the IN
	endpoint named by the data endpoint's bSynchAddress (the 9-byte audio
	endpoint descriptor, UAC1 4.6.1.1). As a fallback for devices that get
	this wrong (Linux does the same), an asynchronous OUT endpoint's
	companion is any other isochronous IN endpoint of the alternate.
*/
void
AudioStreamAlternate::_ParseEndpoints(usb_interface_info* interface)
{
	uint8 syncAddress = 0;
	usb_endpoint_descriptor* data = NULL;

	for (size_t i = 0; i < interface->endpoint_count; i++) {
		usb_endpoint_descriptor* endpoint = interface->endpoint[i].descr;
		if (endpoint == NULL || (endpoint->attributes & 0x03) != 0x01)
			continue;
		if ((endpoint->attributes & USB_EP_USAGE_MASK)
				== USB_EP_USAGE_FEEDBACK) {
			continue;
		}
		if (data == NULL) {
			data = endpoint;
			if (endpoint->length >= 9)
				syncAddress = ((uint8*)endpoint)[8];
		}
	}
	if (data == NULL)
		return;

	fEndpointAddress = data->endpoint_address;
	fEndpointAttributes = data->attributes;
	fMaxPacketSize = data->max_packet_size;
	fInterval = data->interval;

	bool outAsync = (fEndpointAddress & USB_ENDPOINT_ADDR_DIR_IN) == 0
		&& (fEndpointAttributes & USB_EP_SYNC_MASK) == USB_EP_SYNC_ASYNC;
	for (size_t i = 0; i < interface->endpoint_count; i++) {
		usb_endpoint_descriptor* endpoint = interface->endpoint[i].descr;
		if (endpoint == NULL || endpoint == data
			|| (endpoint->attributes & 0x03) != 0x01
			|| (endpoint->endpoint_address & USB_ENDPOINT_ADDR_DIR_IN) == 0) {
			continue;
		}
		bool feedback = (endpoint->attributes & USB_EP_USAGE_MASK)
				== USB_EP_USAGE_FEEDBACK
			|| (syncAddress != 0
				&& (syncAddress | USB_ENDPOINT_ADDR_DIR_IN)
					== endpoint->endpoint_address)
			|| outAsync;
		if (!feedback)
			continue;
		fFeedbackAddress = endpoint->endpoint_address;
		fFeedbackMaxPacketSize = endpoint->max_packet_size & 0x7ff;
		fFeedbackInterval = endpoint->interval;
		break;
	}
}


bool
AudioStreamAlternate::IsInput() const
{
	return (fEndpointAddress & USB_ENDPOINT_ADDR_DIR_IN) != 0;
}


void
AudioStreamAlternate::SetRates(const Vector<uint32>& rates)
{
	fRates.MakeEmpty();
	for (int32 i = 0; i < rates.Count(); i++)
		fRates.PushBack(rates[i]);
	fContinuous = false;
}


bool
AudioStreamAlternate::SupportsRate(uint32 rate) const
{
	if (fContinuous)
		return rate >= fRateMin && rate <= fRateMax;
	for (int32 i = 0; i < fRates.Count(); i++) {
		if (fRates[i] == rate)
			return true;
	}
	return false;
}


uint32
AudioStreamAlternate::FormatId() const
{
	switch (fFormatTag) {
		case USB_AUDIO_FORMAT_PCM8:
			return fSubslotSize == 1 ? B_FMT_8BIT_U : 0;
		case USB_AUDIO_FORMAT_IEEE_FLOAT:
			return fSubslotSize == 4 ? B_FMT_FLOAT : 0;
		case USB_AUDIO_FORMAT_PCM:
			// Samples are left-justified in their subslot (UAC1 Frmts
			// 2.2.4.2, UAC2 Frmts20 2.3.1.6), so a 20-bit sample in a 4-byte
			// subslot is a 32-bit sample to the multi_audio add-on; only
			// the subslot matters, the resolution is information.
			switch (fSubslotSize) {
				case 1: return B_FMT_8BIT_S;
				case 2: return B_FMT_16BIT;
				case 3: return B_FMT_24BIT;
				case 4: return fBitResolution <= 24 && fBitResolution > 16
					? B_FMT_24BIT : B_FMT_32BIT;
			}
			return 0;
	}
	return 0;
}


uint32
AudioStreamAlternate::SamplingRateId(uint32 rate)
{
	for (size_t i = 0; i < B_COUNT_OF(ratesMap); i++) {
		if (ratesMap[i].rate == rate)
			return ratesMap[i].rateId;
	}
	return 0;
}


uint32
AudioStreamAlternate::SamplingRateFromId(uint32 id)
{
	for (size_t i = 0; i < B_COUNT_OF(ratesMap); i++) {
		if (ratesMap[i].rateId == id)
			return ratesMap[i].rate;
	}
	return 0;
}


uint32
AudioStreamAlternate::GetSamFreq(const usb_audio_sampling_freq& freq)
{
	return freq.bytes[0] | freq.bytes[1] << 8 | freq.bytes[2] << 16;
}


usb_audio_sampling_freq
AudioStreamAlternate::GetSamFreq(uint32 samplingRate)
{
	usb_audio_sampling_freq freq;
	for (size_t i = 0; i < 3; i++)
		freq.bytes[i] = 0xFF & samplingRate >> 8 * i;
	return freq;
}


//	#pragma mark - AudioStreamingInterface


AudioStreamingInterface::AudioStreamingInterface(
		AudioControlInterface*	controlInterface,
		size_t interface, usb_interface_list* List)
	:
	fInterface(interface),
	fInterfaceNumber(0),
	fControlInterface(controlInterface),
	fIsInput(false),
	fActiveAlternate(0)
{
	TRACE(UAC, "if[%d]:alt_count:%d\n", (int)interface, (int)List->alt_count);

	for (size_t alt = 0; alt < List->alt_count; alt++) {
		usb_interface_info* info = &List->alt[alt];
		if (info->descr != NULL)
			fInterfaceNumber = info->descr->interface_number;
		AudioStreamAlternate* alternate = new(std::nothrow)
			AudioStreamAlternate(alt, controlInterface->SpecReleaseNumber(),
				info);
		if (alternate != NULL)
			fAlternates.PushBack(alternate);
	}
}


AudioStreamingInterface::~AudioStreamingInterface()
{
	for (Vector<AudioStreamAlternate*>::Iterator I = fAlternates.Begin();
			I != fAlternates.End(); I++)
		delete *I;

	fAlternates.MakeEmpty();
}


uint8
AudioStreamingInterface::TerminalLink()
{
	AudioStreamAlternate* alternate = ActiveAlternate();
	return alternate != NULL ? alternate->TerminalLink() : 0;
}


uint8
AudioStreamingInterface::ChannelsCount()
{
	AudioStreamAlternate* alternate = ActiveAlternate();
	if (alternate == NULL)
		return 0;
	return alternate->Channels();
}


uint32
AudioStreamingInterface::ChannelsConfig()
{
	AudioStreamAlternate* alternate = ActiveAlternate();
	if (alternate == NULL)
		return 0;
	if (fControlInterface->SpecReleaseNumber() >= USB_AUDIO_CLASS_VERSION_2)
		return alternate->ChannelConfig();

	// UAC1: the cluster of the terminal the stream is linked to
	_AudioControl* control = fControlInterface->Find(TerminalLink());
	AudioChannelCluster* cluster = control != NULL
		? control->OutCluster() : NULL;
	return cluster != NULL ? cluster->ChannelsConfig() : 0;
}


void
AudioStreamingInterface::GetFormatsAndRates(multi_description* Description)
{
	Description->interface_flags
		|= fIsInput ? B_MULTI_INTERFACE_RECORD : B_MULTI_INTERFACE_PLAYBACK;

	// Every usable alternate with the active one's channel count adds its
	// format; their rates are what they all share with the active one
	// (SetGlobalFormat() switches alternates by format).
	AudioStreamAlternate* active = ActiveAlternate();
	if (active == NULL)
		return;

	uint32 rates = 0;
	uint32 formats = 0;
	for (int32 i = 0; i < fAlternates.Count(); i++) {
		AudioStreamAlternate* alternate = fAlternates[i];
		if (!alternate->IsUsable() || alternate->IsInput() != fIsInput
			|| alternate->Channels() != active->Channels()) {
			continue;
		}
		formats |= alternate->FormatId();
	}
	for (size_t i = 0; i < B_COUNT_OF(ratesMap); i++) {
		if (active->SupportsRate(ratesMap[i].rate))
			rates |= ratesMap[i].rateId;
	}

	if (fIsInput) {
		Description->input_rates = rates;
		Description->input_formats = formats;
	} else {
		Description->output_rates = rates;
		Description->output_formats = formats;
	}
}
