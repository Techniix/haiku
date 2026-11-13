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
	  1 on (Renga mixers only; LowLatencyAudio.h),
	- a hidden null parameter of kind "renga:app_team" named after the team
	  the stream comes from, in decimal (Renga mixers only, F-58 batch 2,
	  OBS Studio's O-17; absent for streams of media add-ons),
	The mixer's own hidden text parameter of kind "renga:routes" (Renga
	mixers only, F-58) lists every stream's route by device name, a line
	"<input id> <device name>" each ("" for the default output), and takes
	one such line to set a route; any name can be set, also of a device
	that is not there.
	The mixer's hidden text parameter of kind "renga:loopback_only_teams"
	(Renga mixers only, F-58 batch 2, O-18) lists the teams whose streams
	are loopback only for now, one id per line, and takes "<team> 1" or
	"<team> 0" to add or remove one. It is not remembered (see
	SetTeamLoopbackOnly()).
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


namespace BPrivate {


#define RENGA_APP_SIGNATURE_KIND		"renga:app_signature"
#define RENGA_LOOPBACK_FLAVOR_NAME		"What you hear (loopback)"
#define RENGA_ROUTES_KIND				"renga:routes"
// F-58 batch 2 (OBS Studio's O-17, O-18)
#define RENGA_APP_TEAM_KIND				"renga:app_team"
#define RENGA_LOOPBACK_ONLY_TEAMS_KIND	"renga:loopback_only_teams"
// F-58 (OBS Studio's O-2): the route of a stream heard on no output and
// recorded only by loopbacks of its application or team; the "Output"
// menu calls it "None (loopback only)".
#define RENGA_LOOPBACK_ONLY_ROUTE		"renga:loopback-only"
// F-58 (O-3): a hidden text parameter of the loopback, a team id in
// decimal: that team's streams only ("" or "0": by signature as before).
#define RENGA_LOOPBACK_TEAM_KIND		"renga:loopback_team"
#define RENGA_LOOPBACK_TEAM_ID			5
#define RENGA_LOOPBACK_SOURCE_ID		1
#define RENGA_LOOPBACK_SIGNATURE_ID		2
// F-58 (OBS Studio's O-1): a hidden text parameter of the loopback, the
// signatures of applications left out, one per line. Look it up by kind;
// the id is given for completeness.
#define RENGA_LOOPBACK_EXCLUDE_KIND		"renga:loopback_exclude"
#define RENGA_LOOPBACK_EXCLUDE_ID		4


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
	BString		route;			// device name routed to, "" default output,
								// RENGA_LOOPBACK_ONLY_ROUTE for none
	bool		hasRoute;		// the mixer offers routes by name (F-58)
	bool		loopbackOnly;	// heard nowhere, in its own loopbacks only
	// F-58 batch 2 (added at the end; this header is compiled into its
	// users, there is no library side):
	team_id		team;			// the team it comes from (O-17); -1 unknown
	bool		transientLoopbackOnly;
								// loopbackOnly because its team asked for
								// it with SetTeamLoopbackOnly() (O-18); not
								// remembered, route keeps the stored choice
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
		_ReadRoutes();
		_ReadLoopbackOnlyTeams();
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

	/*!	Routes the stream to the device named \a device, whether it exists
		or not (F-58, OBS Studio's O-4): it plays there whenever the device
		is there, on the default output meanwhile. NULL or "": the default
		output. Remembered for the application, like the "Output" choice.
	*/
	status_t SetRoute(int32 id, const char* device)
	{
		BParameter* parameter = _FindKind(RENGA_ROUTES_KIND);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		BString line;
		line << id << " " << (device != NULL ? device : "");
		return parameter->SetValue(line.String(), line.Length() + 1,
			system_time());
	}

	/*!	Plays the stream on no output (\a on), recorded only by loopbacks
		of its application or team, or on the default output again (F-58,
		O-2). Remembered for the application, like the "Output" choice.
	*/
	status_t SetLoopbackOnly(int32 id, bool on)
	{
		return SetRoute(id, on ? RENGA_LOOPBACK_ONLY_ROUTE : "");
	}

	/*!	F-58 batch 2 (OBS Studio's O-17): the first stream of \a team, or
		NULL.
	*/
	const AudioStream* FindStream(team_id team) const
	{
		for (int32 i = 0; i < CountStreams(); i++) {
			if (StreamAt(i)->team == team)
				return StreamAt(i);
		}
		return NULL;
	}

	/*!	F-58 batch 2 (OBS Studio's O-18): plays the streams of \a team on
		no output, recorded only by loopbacks of its application or team
		(\a on), or as their own settings say again. Unlike
		SetLoopbackOnly() it is not remembered: nothing is saved, it ends
		when the team quits, and the streams' remembered "Output" choice is
		left alone. Call it before the team's stream starts and not even
		its first buffer reaches an output; streams already playing switch
		at once. A choice the user makes in a stream's "Output" menu wins
		for that stream. B_BAD_VALUE for a team that does not exist.
		Needs Update() first (it uses the web read there).
	*/
	status_t SetTeamLoopbackOnly(team_id team, bool on)
	{
		BParameter* parameter = _FindKind(RENGA_LOOPBACK_ONLY_TEAMS_KIND);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		if (team <= 0)
			return B_BAD_VALUE;
		char line[32];
		snprintf(line, sizeof(line), "%" B_PRId32 " %d", team, on ? 1 : 0);
		status_t status = parameter->SetValue(line, strlen(line) + 1,
			system_time());
		if (status != B_OK)
			return status;
		// The mixer ignores an unknown team; read back to tell.
		if (on) {
			BString teams;
			if (GetLoopbackOnlyTeams(teams) == B_OK) {
				char number[16];
				snprintf(number, sizeof(number), "%" B_PRId32, team);
				BStringList list;
				teams.Split("\n", true, list);
				if (!list.HasString(number))
					return B_BAD_VALUE;
			}
		}
		return B_OK;
	}

	/*!	F-58 batch 2 (O-18): the teams set with SetTeamLoopbackOnly() that
		still run, one id per line.
	*/
	status_t GetLoopbackOnlyTeams(BString& teams)
	{
		BParameter* parameter = _FindKind(RENGA_LOOPBACK_ONLY_TEAMS_KIND);
		if (parameter == NULL)
			return B_ENTRY_NOT_FOUND;
		char text[1025];
		size_t size = sizeof(text) - 1;
		bigtime_t when;
		status_t status = parameter->GetValue(text, &size, &when);
		if (status != B_OK)
			return status;
		text[min_c(size, sizeof(text) - 1)] = '\0';
		teams = text;
		return B_OK;
	}

	//!	Records only the streams of \a team (0: by signature again; O-3).
	static status_t SetLoopbackTeam(const media_node& node, team_id team)
	{
		char text[16];
		snprintf(text, sizeof(text), "%" B_PRId32, team > 0 ? team : 0);
		return SetLoopbackText(node, RENGA_LOOPBACK_TEAM_KIND, text);
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

	/*!	Leaves the applications in \a list (signatures separated by
		newlines; NULL or "" for none) out of a loopback (F-58, O-1).
	*/
	static status_t SetLoopbackExclusions(const media_node& node,
		const char* list)
	{
		return SetLoopbackText(node, RENGA_LOOPBACK_EXCLUDE_KIND,
			list != NULL ? list : "");
	}

	//!	Sets a loopback's text parameter of the given kind.
	static status_t SetLoopbackText(const media_node& node, const char* kind,
		const char* text)
	{
		BParameterWeb* web;
		status_t status = BMediaRoster::Roster()->GetParameterWebFor(node,
			&web);
		if (status != B_OK)
			return status;
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (parameter->Type() != BParameter::B_TEXT_PARAMETER
				|| strcmp(parameter->Kind(), kind) != 0)
				continue;
			status = parameter->SetValue(text, strlen(text) + 1,
				system_time());
			break;
		}
		delete web;
		return status;
	}

	//!	Reads a loopback's text parameter of the given kind.
	static status_t GetLoopbackText(const media_node& node, const char* kind,
		BString& text)
	{
		BParameterWeb* web;
		status_t status = BMediaRoster::Roster()->GetParameterWebFor(node,
			&web);
		if (status != B_OK)
			return status;
		status = B_ENTRY_NOT_FOUND;
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (parameter->Type() != BParameter::B_TEXT_PARAMETER
				|| strcmp(parameter->Kind(), kind) != 0)
				continue;
			size_t size = ((BTextParameter*)parameter)->MaxBytes() + 1;
			char* buffer = (char*)malloc(size);
			if (buffer == NULL) {
				status = B_NO_MEMORY;
				break;
			}
			bigtime_t when;
			status = parameter->GetValue(buffer, &size, &when);
			if (status == B_OK)
				text.SetTo(buffer, strnlen(buffer, size));
			free(buffer);
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
		stream.hasRoute = false;
		stream.loopbackOnly = false;
		stream.team = -1;
		stream.transientLoopbackOnly = false;
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
			else if (strcmp(kind, RENGA_APP_TEAM_KIND) == 0)
				stream.team = atoi(parameter->Name());
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

	BParameter* _FindKind(const char* kind)
	{
		if (fWeb == NULL)
			return NULL;
		for (int32 i = 0; i < fWeb->CountParameters(); i++) {
			BParameter* parameter = fWeb->ParameterAt(i);
			if (strcmp(parameter->Kind(), kind) == 0)
				return parameter;
		}
		return NULL;
	}

	//!	Fills in the streams' routes from the mixer's "renga:routes".
	void _ReadRoutes()
	{
		BParameter* parameter = _FindKind(RENGA_ROUTES_KIND);
		if (parameter == NULL)
			return;
		char* text = (char*)malloc(8193);
		if (text == NULL)
			return;
		size_t size = 8192;
		bigtime_t when;
		if (parameter->GetValue(text, &size, &when) == B_OK) {
			text[min_c(size, (size_t)8192)] = '\0';
			for (char* line = text; line != NULL && *line != '\0';) {
				char* next = strchr(line, '\n');
				if (next != NULL)
					*next++ = '\0';
				int32 id = atoi(line);
				const char* space = strchr(line, ' ');
				for (int32 i = 0; i < fStreams.CountItems(); i++) {
					AudioStream* stream = (AudioStream*)fStreams.ItemAt(i);
					if (stream->id != id)
						continue;
					stream->hasRoute = true;
					stream->route = space != NULL ? space + 1 : "";
					stream->loopbackOnly
						= stream->route == RENGA_LOOPBACK_ONLY_ROUTE;
				}
				line = next;
			}
		}
		free(text);
	}

	/*!	F-58 batch 2 (O-18): marks the streams of the mixer's loopback-only
		teams. The mixer has the input's own flag, but the teams' list is
		one read for all streams (see _ReadRoutes()).
	*/
	void _ReadLoopbackOnlyTeams()
	{
		BString text;
		if (GetLoopbackOnlyTeams(text) != B_OK || text.Length() == 0)
			return;
		BStringList teams;
		text.Split("\n", true, teams);
		for (int32 i = 0; i < fStreams.CountItems(); i++) {
			AudioStream* stream = (AudioStream*)fStreams.ItemAt(i);
			if (stream->team < 0)
				continue;
			char number[16];
			snprintf(number, sizeof(number), "%" B_PRId32, stream->team);
			if (!teams.HasString(number))
				continue;
			// A choice made in the stream's "Output" menu wins over the
			// team's request (the mixer clears it for that stream; the menu
			// then shows the choice). The menu's "None (loopback only)" is
			// its last shared item, after the default output and the
			// devices. A stream routed there itself is not transient.
			if (stream->route == RENGA_LOOPBACK_ONLY_ROUTE)
				continue;
			if (!stream->hasOutput
				|| stream->output == fOutputs.CountStrings() - 1) {
				stream->transientLoopbackOnly = true;
				stream->loopbackOnly = true;
			}
		}
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
