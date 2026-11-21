/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Push-to-talk and the microphone mute key (batch 11, roadmap F-31). The
 * keys are chosen on the Audio preferences' Microphone page, which writes
 * ~/config/settings/Media/audio_keys ("ptt <raw key code>", "mute <raw
 * key code>"). This filter watches those keys and hands the change to the
 * audio device agent (application/x-vnd.Renga-AudioDeviceAgent, message
 * 'AkMk': "mute" bool, or "toggle" true), which mutes or unmutes every
 * sound card input. The filter itself never waits: key events go through
 * a port to a thread of its own that does the messaging. The push-to-talk
 * key still reaches applications; the mute key is taken.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <new>

#include <FindDirectory.h>
#include <InputServerFilter.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>


static const char* kAgentSignature = "application/x-vnd.Renga-AudioDeviceAgent";
static const uint32 kMsgMicrophoneKey = 'AkMk';
static const bigtime_t kReloadInterval = 1000000;

enum {
	kTalk = 1,
	kQuiet,
	kToggle
};


class AudioKeysFilter : public BInputServerFilter {
public:
								AudioKeysFilter();
	virtual						~AudioKeysFilter();

	virtual	status_t			InitCheck();
	virtual	filter_result		Filter(BMessage* message, BList* _list);

private:
			void				_Reload();
	static	status_t			_Worker(void* data);

			port_id				fPort;
			thread_id			fThread;
			int32				fPushToTalk;
			int32				fMute;
			bool				fTalking;
			time_t				fModified;
			bigtime_t			fChecked;
			BPath				fPath;
};


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) AudioKeysFilter();
}


AudioKeysFilter::AudioKeysFilter()
	:
	fPort(-1),
	fThread(-1),
	fPushToTalk(0),
	fMute(0),
	fTalking(false),
	fModified(0),
	fChecked(0)
{
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &fPath) == B_OK)
		fPath.Append("Media/audio_keys");
	fPort = create_port(16, "audio keys");
	if (fPort >= 0) {
		fThread = spawn_thread(_Worker, "audio keys", B_NORMAL_PRIORITY,
			this);
		if (fThread >= 0)
			resume_thread(fThread);
	}
	_Reload();
}


AudioKeysFilter::~AudioKeysFilter()
{
	if (fPort >= 0)
		delete_port(fPort);
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}
}


status_t
AudioKeysFilter::InitCheck()
{
	return fPort >= 0 && fThread >= 0 ? B_OK : B_ERROR;
}


void
AudioKeysFilter::_Reload()
{
	fChecked = system_time();
	struct stat st;
	if (fPath.InitCheck() != B_OK || stat(fPath.Path(), &st) != 0) {
		fPushToTalk = fMute = 0;
		fModified = 0;
		return;
	}
	if (st.st_mtime == fModified)
		return;
	fModified = st.st_mtime;
	fPushToTalk = fMute = 0;
	FILE* file = fopen(fPath.Path(), "r");
	if (file == NULL)
		return;
	char line[64];
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "ptt ", 4) == 0)
			fPushToTalk = atoi(line + 4);
		else if (strncmp(line, "mute ", 5) == 0)
			fMute = atoi(line + 5);
	}
	fclose(file);
}


filter_result
AudioKeysFilter::Filter(BMessage* message, BList* _list)
{
	bool down;
	switch (message->what) {
		case B_KEY_DOWN:
		case B_UNMAPPED_KEY_DOWN:
			down = true;
			break;
		case B_KEY_UP:
		case B_UNMAPPED_KEY_UP:
			down = false;
			break;
		default:
			return B_DISPATCH_MESSAGE;
	}
	if (system_time() - fChecked > kReloadInterval)
		_Reload();
	if (fPushToTalk == 0 && fMute == 0)
		return B_DISPATCH_MESSAGE;

	int32 key;
	if (message->FindInt32("key", &key) != B_OK)
		return B_DISPATCH_MESSAGE;

	if (key == fPushToTalk && fPushToTalk != 0) {
		// key repeat sends more downs: only the change counts
		if (down != fTalking) {
			fTalking = down;
			int32 code = down ? kTalk : kQuiet;
			write_port_etc(fPort, code, NULL, 0, B_RELATIVE_TIMEOUT, 0);
		}
		return B_DISPATCH_MESSAGE;
	}
	if (key == fMute && fMute != 0) {
		if (down && message->GetInt32("be:key_repeat", 0) == 0) {
			write_port_etc(fPort, kToggle, NULL, 0, B_RELATIVE_TIMEOUT,
				0);
		}
		return B_SKIP_MESSAGE;
	}
	return B_DISPATCH_MESSAGE;
}


status_t
AudioKeysFilter::_Worker(void* data)
{
	AudioKeysFilter* self = (AudioKeysFilter*)data;
	while (true) {
		int32 code;
		if (read_port(self->fPort, &code, NULL, 0) < 0)
			break;
		BMessage message(kMsgMicrophoneKey);
		if (code == kToggle)
			message.AddBool("toggle", true);
		else
			message.AddBool("mute", code == kQuiet);
		BMessenger agent(kAgentSignature);
		if (!agent.IsValid() || agent.SendMessage(&message,
				(BHandler*)NULL, 1000000) != B_OK) {
			// not running (it stays once keys are set, but it may have
			// been quit): start it with the message
			be_roster->Launch(kAgentSignature, &message);
		}
	}
	return B_OK;
}
