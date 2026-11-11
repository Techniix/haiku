/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _AUDIO_STREAM_CONTROL_H
#define _AUDIO_STREAM_CONTROL_H


/*!	Per-application audio controls of the system mixer, for the desktop's
	sound controls (the Deskbar volume control, Media preferences,
	SoundRecorder) and the tests. Header-only on purpose: it only uses
	public Media Kit API, so it adds nothing to libmedia's ABI.

	It reads the system mixer's parameter web, where each stream (a mixer
	input) is a group with
	- a B_WEB_BUFFER_INPUT null parameter: the stream's display name,
	- a hidden null parameter of kind "renga:app_signature" named after the
	  application's signature (Renga mixers only),
	- a B_MUTE discrete parameter, a B_GAIN continuous parameter (one value
	  per channel, in dB, or one value plus a B_BALANCE parameter),
	- a B_OUTPUT_MUX discrete parameter "Output": 0 the default output, n
	  the n-th output device (Renga mixers only),
	- a discrete parameter of kind "renga:low_latency" "Low latency": 0 off,
	  1 on (Renga mixers only; LowLatencyAudio.h).
	The stream's mixer input id is the parameters' id >> 16, as the mixer
	encodes them. The loopback ("What you hear") is a flavor of the mixer
	add-on; StartLoopback() instantiates one and selects whose streams it
	records.

	Not thread safe; use one object per thread.
*/


#include <MediaAddOn.h>
#include <MediaRoster.h>
#include <ParameterWeb.h>
#include <String.h>
#include <StringList.h>

#include <stdlib.h>
#include <string.h>


namespace BPrivate {


#define RENGA_APP_SIGNATURE_KIND		"renga:app_signature"
#define RENGA_LOOPBACK_FLAVOR_NAME		"What you hear (loopback)"
#define RENGA_LOOPBACK_SOURCE_ID		1
#define RENGA_LOOPBACK_SIGNATURE_ID		2


struct AudioStream {
	int32		id;				// mixer input id
	BString		name;
	BString		signature;		// empty if unknown
	float		gain;			// dB, the average of the channels
	bool		muted;
	int32		output;			// index into AudioStreamControl::Outputs()
	bool		hasOutput;		// the mixer offers routing
	bool		lowLatency;		// low-latency mode switched on
	bool		hasLowLatency;	// the mixer offers the low-latency mode
};


class AudioStreamControl {
public:
	AudioStreamControl()
		:
		fRoster(NULL),
		fWeb(NULL),
		fHaveMixer(false)
	{
	}

	~AudioStreamControl()
	{
		_Clear();
		if (fHaveMixer)
			fRoster->ReleaseNode(fMixer);
	}

	//!	(Re)reads the streams from the system mixer.
	status_t Update()
	{
		_Clear();
		status_t status;
		if (fRoster == NULL) {
			fRoster = BMediaRoster::Roster(&status);
			if (fRoster == NULL)
				return status;
		}
		if (!fHaveMixer) {
			status = fRoster->GetAudioMixer(&fMixer);
			if (status != B_OK)
				return status;
			fHaveMixer = true;
		}
		status = fRoster->GetParameterWebFor(fMixer, &fWeb);
		if (status != B_OK) {
			// The mixer may have been restarted with the media services.
			fRoster->ReleaseNode(fMixer);
			fHaveMixer = false;
			fWeb = NULL;
			return status;
		}

		for (int32 i = 0; i < fWeb->CountGroups(); i++)
			_ScanGroup(fWeb->GroupAt(i));
		return B_OK;
	}

	int32 CountStreams() const
	{
		return fStreams.CountItems();
	}

	const AudioStream* StreamAt(int32 index) const
	{
		return (const AudioStream*)fStreams.ItemAt(index);
	}

	const AudioStream* FindStream(const char* signature) const
	{
		for (int32 i = 0; i < CountStreams(); i++) {
			if (StreamAt(i)->signature == signature)
				return StreamAt(i);
		}
		return NULL;
	}

	//!	Output choices: 0 is the default output, then the devices.
	const BStringList& Outputs() const
	{
		return fOutputs;
	}

	status_t SetGain(int32 id, float gain)
	{
		BContinuousParameter* parameter
			= (BContinuousParameter*)_Find(id, B_GAIN, false);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		int32 count = parameter->CountChannels();
		if (count < 1 || count > 16)
			return B_BAD_VALUE;
		float values[16];
		for (int32 i = 0; i < count; i++)
			values[i] = gain;
		return parameter->SetValue(values, count * sizeof(float),
			system_time());
	}

	status_t SetMuted(int32 id, bool muted)
	{
		BParameter* parameter = _Find(id, B_MUTE, true);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		int32 value = muted ? 1 : 0;
		return parameter->SetValue(&value, sizeof(value), system_time());
	}

	//!	Switches the low-latency mode of a stream's application.
	status_t SetLowLatency(int32 id, bool on)
	{
		BParameter* parameter = _Find(id, "renga:low_latency", true);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		int32 value = on ? 1 : 0;
		return parameter->SetValue(&value, sizeof(value), system_time());
	}

	status_t SetOutput(int32 id, int32 index)
	{
		BParameter* parameter = _Find(id, B_OUTPUT_MUX, true);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		return parameter->SetValue(&index, sizeof(index), system_time());
	}

	//!	Routes the stream to the device named \a device (NULL: default).
	status_t SetOutputByName(int32 id, const char* device)
	{
		if (device == NULL || device[0] == '\0')
			return SetOutput(id, 0);
		int32 index = fOutputs.IndexOf(device);
		if (index <= 0)
			return B_ENTRY_NOT_FOUND;
		return SetOutput(id, index);
	}

	/*!	Instantiates a loopback source, recording every stream (\a signature
		NULL or empty) or the streams of one application. The caller owns
		the node reference (BMediaRoster::ReleaseNode()).
	*/
	static status_t StartLoopback(const char* signature, media_node* _node)
	{
		BMediaRoster* roster = BMediaRoster::Roster();
		dormant_node_info info;
		status_t status = FindLoopbackFlavor(&info);
		if (status != B_OK)
			return status;
		status = roster->InstantiateDormantNode(info, _node,
			B_FLAVOR_IS_GLOBAL);
		if (status != B_OK)
			return status;
		status = SetLoopbackSource(*_node, signature);
		if (status != B_OK) {
			roster->ReleaseNode(*_node);
			return status;
		}
		return B_OK;
	}

	static status_t SetLoopbackSource(const media_node& node,
		const char* signature)
	{
		BParameterWeb* web;
		status_t status = BMediaRoster::Roster()->GetParameterWebFor(node,
			&web);
		if (status != B_OK)
			return status;
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (parameter->ID() != RENGA_LOOPBACK_SIGNATURE_ID)
				continue;
			const char* text = signature != NULL ? signature : "";
			status = parameter->SetValue(text, strlen(text) + 1,
				system_time());
			break;
		}
		delete web;
		return status;
	}

	static status_t FindLoopbackFlavor(dormant_node_info* _info)
	{
		dormant_node_info infos[64];
		int32 count = 64;
		status_t status = BMediaRoster::Roster()->GetDormantNodes(infos,
			&count, NULL, NULL, NULL, B_BUFFER_PRODUCER | B_PHYSICAL_INPUT, 0);
		if (status != B_OK)
			return status;
		for (int32 i = 0; i < count; i++) {
			if (strcmp(infos[i].name, RENGA_LOOPBACK_FLAVOR_NAME) == 0) {
				*_info = infos[i];
				return B_OK;
			}
		}
		return B_ENTRY_NOT_FOUND;
	}

private:
	void _Clear()
	{
		for (int32 i = 0; i < fStreams.CountItems(); i++)
			delete (AudioStream*)fStreams.ItemAt(i);
		fStreams.MakeEmpty();
		fOutputs.MakeEmpty();
		delete fWeb;
		fWeb = NULL;
	}

	void _ScanGroup(BParameterGroup* group)
	{
		AudioStream stream;
		stream.id = -1;
		stream.gain = 0;
		stream.muted = false;
		stream.output = 0;
		stream.hasOutput = false;
		stream.lowLatency = false;
		stream.hasLowLatency = false;
		bool isStream = false;

		for (int32 i = 0; i < group->CountParameters(); i++) {
			BParameter* parameter = group->ParameterAt(i);
			int32 id = parameter->ID() >> 16;
			const char* kind = parameter->Kind();
			if (parameter->Type() == BParameter::B_NULL_PARAMETER
				&& strcmp(kind, B_WEB_BUFFER_INPUT) == 0 && id > 0) {
				stream.id = id;
				stream.name = parameter->Name();
				isStream = true;
			} else if (strcmp(kind, RENGA_APP_SIGNATURE_KIND) == 0)
				stream.signature = parameter->Name();
			else if (strcmp(kind, "renga:low_latency") == 0
				&& parameter->Type() == BParameter::B_DISCRETE_PARAMETER) {
				int32 value = 0;
				stream.hasLowLatency = true;
				if (_Value(parameter, &value, sizeof(value)))
					stream.lowLatency = value != 0;
			} else if (strcmp(kind, B_MUTE) == 0) {
				int32 value = 0;
				if (_Value(parameter, &value, sizeof(value)))
					stream.muted = value != 0;
			} else if (strcmp(kind, B_GAIN) == 0
				&& parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER) {
				float values[16];
				int32 count = parameter->CountChannels();
				if (count >= 1 && count <= 16
					&& _Value(parameter, values, count * sizeof(float))) {
					float sum = 0;
					for (int32 c = 0; c < count; c++)
						sum += values[c];
					stream.gain = sum / count;
				}
			} else if (strcmp(kind, B_OUTPUT_MUX) == 0
				&& parameter->Type() == BParameter::B_DISCRETE_PARAMETER) {
				BDiscreteParameter* discrete = (BDiscreteParameter*)parameter;
				stream.hasOutput = true;
				int32 value = 0;
				if (_Value(parameter, &value, sizeof(value)))
					stream.output = value;
				// Every stream lists the same devices; a stream routed to a
				// device that is not available has one item more. So the
				// shortest list is the shared one.
				if (fOutputs.IsEmpty()
					|| discrete->CountItems() < fOutputs.CountStrings()) {
					fOutputs.MakeEmpty();
					for (int32 j = 0; j < discrete->CountItems(); j++)
						fOutputs.Add(discrete->ItemNameAt(j));
				}
			}
		}

		if (isStream) {
			AudioStream* copy = new(std::nothrow) AudioStream(stream);
			if (copy != NULL)
				fStreams.AddItem(copy);
		}

		for (int32 i = 0; i < group->CountGroups(); i++)
			_ScanGroup(group->GroupAt(i));
	}

	bool _Value(BParameter* parameter, void* value, size_t size)
	{
		bigtime_t when;
		size_t got = size;
		return parameter->GetValue(value, &got, &when) == B_OK;
	}

	BParameter* _Find(int32 id, const char* kind, bool discrete)
	{
		if (fWeb == NULL)
			return NULL;
		for (int32 i = 0; i < fWeb->CountParameters(); i++) {
			BParameter* parameter = fWeb->ParameterAt(i);
			if ((parameter->ID() >> 16) == id
				&& strcmp(parameter->Kind(), kind) == 0
				&& (parameter->Type() == BParameter::B_DISCRETE_PARAMETER)
					== discrete) {
				return parameter;
			}
		}
		return NULL;
	}

private:
	BMediaRoster*	fRoster;
	media_node		fMixer;
	BParameterWeb*	fWeb;
	bool			fHaveMixer;
	BList			fStreams;
	BStringList		fOutputs;
};


}	// namespace BPrivate


#endif	// _AUDIO_STREAM_CONTROL_H
