/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Copyright 2026, Renga contributors.
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _USB_AUDIO_DEVICE_H_
#define _USB_AUDIO_DEVICE_H_


#include "AudioControlInterface.h"
#include "Stream.h"


class Device {
	friend	class			Stream;

public:
							Device(usb_device device);
	virtual					~Device();

			status_t		InitCheck() { return fStatus; };

			status_t		Open(uint32 flags);
			bool			IsOpen() { return fOpen; };

			status_t		Close();
			status_t		Free();

			status_t		Read(uint8* buffer, size_t* numBytes);
			status_t		Write(const uint8* buffer, size_t* numBytes);
			status_t		Control(uint32 op, void* buffer, size_t length);

			void			Removed();
			bool			IsRemoved() { return fRemoved; };

			status_t		CompareAndReattach(usb_device device);
	virtual	status_t		SetupDevice(bool deviceReplugged);

			usb_device		USBDevice() { return fDevice; }
			bool			IsHighSpeed() { return fHighSpeed; }
			const char*		Name() { return fName; }

			AudioControlInterface&
							AudioControl() { return fAudioControl; }

			// The stream whose progress the buffer exchange waits for:
			// playback if there is one (capture rides along), else capture.
			bool			IsPacer(Stream* stream);

private:
			status_t		_SetupEndpoints();
			void			_ReadName(const usb_device_descriptor* descriptor);
			Stream*			_PlaybackStream();
			Stream*			_RecordStream();
			Stream*			_PacingStream();

// protected:
	virtual	status_t		StartDevice() { return B_OK; }
	virtual	status_t		StopDevice();

			void			TraceMultiDescription(multi_description* Description,
								Vector<multi_channel_info>& Channels);
			void			TraceListMixControls(multi_mix_control_info* Info);

		// state tracking
			status_t		fStatus;
			bool			fOpen;
			volatile bool	fRemoved;
			usb_device		fDevice;
			uint16			fUSBVersion;
			uint16			fVendorID;
			uint16			fProductID;
	const	char*			fDescription;
			bool			fNonBlocking;
			bool			fHighSpeed;
			char			fName[32];

			AudioControlInterface	fAudioControl;
			Vector<Stream*>	fStreams;

// protected:
			status_t		_MultiGetDescription(multi_description* Description);
			status_t		_MultiGetEnabledChannels(multi_channel_enable* Enable);
			status_t		_MultiSetEnabledChannels(multi_channel_enable* Enable);
			status_t		_MultiGetBuffers(multi_buffer_list* List);
			status_t		_MultiGetGlobalFormat(multi_format_info* Format);
			status_t		_MultiSetGlobalFormat(multi_format_info* Format);
			status_t		_MultiGetMix(multi_mix_value_info* Info);
			status_t		_MultiSetMix(multi_mix_value_info* Info);
			status_t		_MultiListMixControls(multi_mix_control_info* Info);
			status_t		_MultiBufferExchange(multi_buffer_info* Info);
			status_t		_MultiBufferForceStop();

			sem_id			fBuffersReadySem;
			// counts force stops, so that an exchange waiting across one
			// returns B_CANCELED instead of a stale buffer
			int32			fStopGeneration;
};


#endif // _USB_AUDIO_DEVICE_H_
