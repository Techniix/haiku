/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MICROPHONE_PROCESSING_H
#define MICROPHONE_PROCESSING_H


#include <String.h>


/*!	Batch 11 (F-31 routing): the microphone processing of a sound card's
	input (multi_audio's hidden "renga:input_processing" parameter: mute,
	noise gate, automatic gain, high-pass; AudioStreamControl.h), read and
	set by device name. With the device's node running the node is asked
	(it keeps the setting itself); otherwise the setting file the node
	reads when it starts is used, so a choice for an unplugged device is
	kept as well.

	Push-to-talk and the mute key are kept in ~/config/settings/Media/
	audio_keys ("ptt <key code>", "mute <key code>"; 0 or missing: off),
	which the audio_keys input_server filter reads; it sends
	kMsgMicrophoneKey to the audio device agent, which mutes and unmutes
	every input with SetAllInputsMuted().
*/

enum {
	kMsgMicrophoneKey	= 'AkMk'
		// "mute" bool: set; or "toggle" bool true: the other way
};


status_t	get_input_processing(const char* device, BString& text);
status_t	set_input_processing(const char* device, const char* text);
//!	One value of the "key value" text, \a fallback if missing.
BString		input_processing_value(const BString& text, const char* key,
				const char* fallback);
//!	Sets "mute" on every sound card input. Returns the number changed.
int32		set_all_inputs_muted(bool muted);
//!	Whether any sound card input is muted now.
bool		any_input_muted();

struct audio_keys {
	int32	pushToTalk;		// raw key code, 0 off
	int32	muteToggle;		// raw key code, 0 off
};

status_t	read_audio_keys(audio_keys& keys);
status_t	write_audio_keys(const audio_keys& keys);


#endif	// MICROPHONE_PROCESSING_H
