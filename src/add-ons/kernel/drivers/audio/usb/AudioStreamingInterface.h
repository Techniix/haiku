/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Copyright 2026, Renga contributors.
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _AUDIO_STREAMING_INTERFACE_H_
#define _AUDIO_STREAMING_INTERFACE_H_


#include <USB3.h>
#include <util/VectorMap.h>

#include "AudioControlInterface.h"


/*!	One alternate setting of an audio streaming interface, as far as it can
	carry PCM audio: the format (Type I: channels, subslot size, bit
	resolution, the UAC1 format tag), the sample rates, the isochronous data
	endpoint and, for asynchronous OUT endpoints, the feedback endpoint.

	UAC1 (Audio 1.0) alternates list their sample rates in the format
	descriptor (discrete, or a continuous range). UAC2 (Audio 2.0)
	alternates have none: the rates belong to the clock entity the
	terminal is connected to and are read from the device with a RANGE
	request (AudioControlInterface::GetClockRates()). The channel cluster
	is the terminal's in UAC1 and the alternate's own in UAC2.
*/
class AudioStreamAlternate {
public:
								AudioStreamAlternate(size_t alternate,
									uint16 specRelease,
									usb_interface_info* interface);
								~AudioStreamAlternate();

			bool				IsUsable() const { return fUsable; }
			bool				IsInput() const;

			size_t				Alternate() const { return fAlternate; }
			uint8				TerminalLink() const { return fTerminalLink; }
			uint16				FormatTag() const { return fFormatTag; }
			uint8				Channels() const { return fChannels; }
			uint32				ChannelConfig() const
									{ return fChannelConfig; }
			uint8				SubslotSize() const { return fSubslotSize; }
			uint8				BitResolution() const
									{ return fBitResolution; }

			uint8				EndpointAddress() const
									{ return fEndpointAddress; }
			uint8				EndpointAttributes() const
									{ return fEndpointAttributes; }
			uint16				MaxPacketSize() const
									{ return fMaxPacketSize; }
			uint8				Interval() const { return fInterval; }
			uint8				FeedbackAddress() const
									{ return fFeedbackAddress; }
			uint16				FeedbackMaxPacketSize() const
									{ return fFeedbackMaxPacketSize; }
			uint8				FeedbackInterval() const
									{ return fFeedbackInterval; }
			uint8				CSAttributes() const
									{ return fCSAttributes; }

			// The rates this alternate can play: discrete rates, or a
			// continuous range [RateMin(), RateMax()].
			bool				IsContinuous() const { return fContinuous; }
			uint32				RateMin() const { return fRateMin; }
			uint32				RateMax() const { return fRateMax; }
			const Vector<uint32>& Rates() const { return fRates; }
			void				SetRates(const Vector<uint32>& rates);
			bool				SupportsRate(uint32 rate) const;

			// hmulti format of the samples (B_FMT_*), 0 if none fits
			uint32				FormatId() const;

	static	uint32				SamplingRateId(uint32 rate);
	static	uint32				SamplingRateFromId(uint32 id);
	static	uint32				GetSamFreq(const usb_audio_sampling_freq& freq);
	static	usb_audio_sampling_freq	GetSamFreq(uint32 rate);

private:
			void				_ParseEndpoints(usb_interface_info* interface);

			size_t				fAlternate;
			bool				fUsable;
			uint16				fSpecRelease;

			uint8				fTerminalLink;
			uint16				fFormatTag;
			uint8				fFormatType;
			uint8				fChannels;
			uint32				fChannelConfig;
			uint8				fSubslotSize;
			uint8				fBitResolution;

			bool				fContinuous;
			uint32				fRateMin;
			uint32				fRateMax;
			Vector<uint32>		fRates;

			uint8				fEndpointAddress;
			uint8				fEndpointAttributes;
			uint16				fMaxPacketSize;
			uint8				fInterval;
			uint8				fCSAttributes;
			uint8				fFeedbackAddress;
			uint16				fFeedbackMaxPacketSize;
			uint8				fFeedbackInterval;
};


class AudioStreamingInterface {
public:
								AudioStreamingInterface(
									AudioControlInterface*	controlInterface,
									size_t interface, usb_interface_list* List);
								~AudioStreamingInterface();

			uint8				TerminalLink();
			bool				IsInput() { return fIsInput; }

			// The channel cluster of the active alternate: from the
			// terminal (UAC1) or the alternate itself (UAC2).
			uint8				ChannelsCount();
			uint32				ChannelsConfig();

			void				GetFormatsAndRates(
									multi_description* Description);

			AudioStreamAlternate* ActiveAlternate()
									{ return fAlternates.Count() > 0
										? fAlternates[fActiveAlternate]
										: NULL; }
protected:
			size_t				fInterface;
			uint8				fInterfaceNumber;
			AudioControlInterface*	fControlInterface;

			bool				fIsInput;
		// alternates of the streams
			Vector<AudioStreamAlternate*>	fAlternates;
			size_t				fActiveAlternate;
};


#endif // _AUDIO_STREAMING_INTERFACE_H_
