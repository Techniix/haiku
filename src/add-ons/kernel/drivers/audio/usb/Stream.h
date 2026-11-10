/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Copyright 2026, Renga contributors.
 *	Distributed under the terms of the MIT license.
 */
#ifndef _USB_AUDIO_STREAM_H_
#define _USB_AUDIO_STREAM_H_


#include <KernelExport.h>

#include "AudioStreamingInterface.h"


class Device;
class Stream;


/*!	One isochronous transfer in flight: a bounce buffer of up to
	fPacketsPerTransfer packets and their descriptors.
*/
struct StreamTransfer {
	Stream*						stream;
	uint8*						data;
	usb_iso_packet_descriptor*	packets;
	uint32						packetCount;
	size_t						length;
	uint32						frames;		// playback: frames it carries
};


/*!	A USB audio stream (one audio streaming interface) as one direction of
	an hmulti device.

	The multi_audio protocol works on a ring of fBufferCount buffers of
	fBufferFrames frames that the client fills (playback) or reads
	(capture). USB moves audio in packets, one per service interval, whose
	sizes need not divide the buffers evenly: 44.1 kHz is 44.1 frames per
	millisecond, an asynchronous device asks for a frame more or less now
	and then through its feedback endpoint, and a capture device sends what
	it has. So, like FreeBSD's uaudio and Linux's snd-usb-audio, the stream
	keeps the ring separate from the USB transfers: kTransferCount
	transfers are in flight at any time, each with a bounce buffer; when
	one completes, a playback stream fills it again with the next frames
	from the ring (as many per packet as the rate, or the feedback, says),
	a capture stream copies what each packet brought into the ring, and the
	transfer is queued again. Every time the ring position passes the end
	of a buffer, that buffer is done: the client may fill it again
	(playback) or read it (capture), which ExchangeBuffer() reports and the
	device's exchange semaphore signals.

	The copy also converts 3-byte subslots (24-bit audio, the most common
	UAC2 format) to and from the 4-byte samples the multi_audio add-on
	writes and reads, left-justified as USB audio samples are.
*/
class Stream : public AudioStreamingInterface {
	friend	class			Device;
public:
							Stream(Device* device, size_t interface,
								usb_interface_list* List);
							~Stream();

			status_t		Init();
			status_t		InitCheck() { return fStatus; }

			status_t		Start();
			status_t		Stop();
			bool			IsRunning() { return fIsRunning; }
			// whether the client asked for this stream's buffers
			bool			HasBuffers() { return fArea >= 0; }
			// stops the stream and frees its buffers (the client did not
			// ask for them; a new format needs new ones)
			void			ReleaseBuffers();
			void			OnRemove();

			status_t		GetBuffers(multi_buffer_list* List);

			status_t		OnSetConfiguration(usb_device device,
							const usb_configuration_info* config);

			bool			ExchangeBuffer(multi_buffer_info* Info);
			status_t		GetEnabledChannels(uint32& offset,
								multi_channel_enable* Enable);
			status_t		SetEnabledChannels(uint32& offset,
								multi_channel_enable* Enable);
			status_t		GetGlobalFormat(multi_format_info* Format);
			status_t		SetGlobalFormat(multi_format_info* Format);

			uint32			Rate() const { return fRate; }
			// UAC2: the clock source the stream runs on, 0 for UAC1
			uint8			ClockSourceID();
			uint32			BufferFrames() const { return fBufferFrames; }
			uint32			BufferCount() const { return fBufferCount; }

	// Frames per packet as 16.16 fixed point, and the next packet's frame
	// count: the rate accumulator that spreads fractional rates over
	// packets (public for the driver's own tests).
	static	uint32			NominalFramesPerPacket(uint32 rate,
								uint32 packetsPerSecond);
	static	uint32			NextPacketFrames(uint32& accumulator,
								uint32 framesPerPacket);
	// A feedback endpoint's value as 16.16 frames per packet (UAC1/USB 2.0
	// 5.12.4.2: 10.14 in 3 bytes at full speed, 16.16 in 4 bytes at high
	// speed; some devices use the other one, as Linux notes).
	static	uint32			ParseFeedback(const uint8* data, size_t length,
								bool highSpeed, uint32 nominal,
								uint32 packetsPerInterval);

protected:
			Device*			fDevice;
			status_t		fStatus;

			usb_pipe		fStreamEndpoint;
			usb_pipe		fFeedbackEndpoint;
			const usb_configuration_info* fConfiguration;

			volatile bool	fIsRunning;
			volatile bool	fStopping;

			// the multi_audio ring, shared with the client
			area_id			fArea, fKernelArea;
			size_t			fAreaSize;
			uint8*			fBuffers;
			uint8*			fKernelBuffers;
			uint32			fBufferFrames;
			uint32			fBufferCount;
			uint32			fRingSampleSize;	// bytes per sample in the ring
			uint32			fRingFrameSize;

			// the current format
			uint32			fRate;
			uint32			fFrameSize;		// bytes per frame on the bus
			uint32			fSubslotSize;
			uint32			fChannels;

			// USB scheduling
			bool			fHighSpeed;
			uint32			fPacketsPerSecond;
			uint32			fPacketsPerTransfer;
			uint32			fMaxPacketBytes;
			uint32			fMaxPacketFrames;
			uint32			fNominalFrames;		// 16.16 frames per packet
			volatile uint32	fFeedbackFrames;	// 16.16, 0 if none yet
			uint32			fAccumulator;

			area_id			fTransferArea;
			StreamTransfer	fTransfers[4];
			uint32			fTransferCount;
			StreamTransfer	fFeedbackTransfer;
			uint8*			fFeedbackData;

			// ring position and what the client is told (fLock)
			spinlock		fLock;
			uint32			fRingPosition;	// frames
			int32			fCycle;
			int64			fCycleFrames;
			bigtime_t		fCycleTime;
			int32			fCycles;		// boundaries passed
			int32			fReportedCycles;
			int32			fUnderruns;

			int32			fInsideNotify;
			// Transfers queued whose callback has not finished yet; see
			// _WaitForCompletions()
			int32			fOutstanding;

private:
			status_t		_ChooseAlternate();
			status_t		_ReadRates();
			status_t		_SetupBuffers();
			status_t		_SetupTransfers();
			void			_FreeTransfers();
			void			_ForgetTransfers();
			status_t		_SetRate();
			status_t		_FindPipes();
			status_t		_QueueTransfer(StreamTransfer* transfer);
			void			_FillTransfer(StreamTransfer* transfer);
			void			_DrainTransfer(StreamTransfer* transfer);
			void			_Advance(uint32 frames, bigtime_t now);
			status_t		_QueueFeedback();
			bool			_WaitForCompletions(bigtime_t timeout);
	static	void			_TransferCallback(void* cookie, status_t status,
								void* data, size_t actualLength);
	static	void			_FeedbackCallback(void* cookie, status_t status,
								void* data, size_t actualLength);
};


#endif // _USB_AUDIO_STREAM_H_
