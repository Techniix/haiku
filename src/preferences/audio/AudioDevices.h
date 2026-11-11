/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AUDIO_DEVICES_H
#define AUDIO_DEVICES_H


#include <MediaAddOn.h>
#include <MediaNode.h>
#include <ObjectList.h>
#include <String.h>
#include <StringList.h>


/*!	The sound cards the Media Kit knows (roadmap F-32): every physical audio
	output and input the media add-ons offer, which of them are the
	defaults, and the settings of their running nodes the audio settings
	show (frame rate, the low-latency "Buffer size" of F-31). Built from
	public Media Kit calls only, like AudioStreamControl, so that the Audio
	preferences, the Deskbar volume popup and, later, the Control Panel can
	share it.

	A card that both plays and records is one device with both directions.
	Device names are the flavor names the add-ons publish: multi_audio
	names HDMI/DisplayPort outputs after the monitor (hda reads its ELD),
	USB devices after their product string, identical cards get a number.
	The loopback flavor ("What you hear") is not a device; LoopbackInfo()
	finds it.

	Not thread safe; use one object per thread.
*/
struct AudioDevice {
	BString				name;
	dormant_node_info	info;
	bool				output;			// plays (a physical output)
	bool				input;			// records (a physical input)
	bool				defaultOutput;
	bool				defaultInput;
	bool				running;		// has a node now
	media_node_id		nodeID;
	BString				outputRate;		// "48000 Hz", from its parameters
	BString				inputRate;
	// The "Buffer size" parameter (outputs of multi_audio nodes):
	bool				hasBufferSize;
	int32				bufferFrames;	// 0: automatic
	BList				bufferValues;	// int32 values, cast to void*
	BStringList			bufferNames;
};


class AudioDeviceList {
public:
								AudioDeviceList();
								~AudioDeviceList();

			//!	Reads the devices again. \a details reads each running
			//	node's parameters too (slower).
			status_t			Update(bool details = true);

			int32				CountDevices() const
									{ return fDevices.CountItems(); }
			const AudioDevice*	DeviceAt(int32 index) const
									{ return fDevices.ItemAt(index); }
			const AudioDevice*	FindDevice(const char* name) const;

			//!	Names of the devices that play / record, in list order.
			BStringList			OutputNames() const;
			BStringList			InputNames() const;
			BString				DefaultOutputName() const;
			BString				DefaultInputName() const;

			//!	A key that changes whenever a device comes, goes or changes
			//	its role; for views that rebuild only then.
			BString				Key() const;

			status_t			SetDefaultOutput(const char* name);
			status_t			SetDefaultInput(const char* name);
			status_t			SetBufferFrames(const char* name,
									int32 frames);

	static	status_t			LoopbackInfo(dormant_node_info* _info);

private:
			void				_Add(const dormant_node_info& info,
									bool output);
			void				_ReadDetails(AudioDevice* device);

			BObjectList<AudioDevice, true> fDevices;
};


#endif	// AUDIO_DEVICES_H
