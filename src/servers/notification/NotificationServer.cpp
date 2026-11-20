/*
 * Copyright 2010-2017, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Pier Luigi Fiorini, pierluigi.fiorini@gmail.com
 */


#include "NotificationServer.h"

#include <stdlib.h>

#include <Alert.h>
#include <Beep.h>
#include <Notification.h>
#include <Notifications.h>
#include <PropertyInfo.h>
#include <Roster.h>

#include "NotificationWindow.h"

#include <streamer_mode.h>


const char* kSoundNames[] = {
	"Notification information",
	"Notification important",
	"Notification error",
	"Notification progress",
	NULL
};


NotificationServer::NotificationServer(status_t& error)
	:
	BServer(kNotificationServerSignature, true, &error),
	fWindow(NULL),
	fHeld(8),
	fHeldCheck(NULL)
{
}


NotificationServer::~NotificationServer()
{
	delete fHeldCheck;
}


static const uint32 kMsgCheckHeld = 'chkh';
static const int32 kMaxHeld = 100;


/*!	Renga (batch 5, streamer mode, headers/private/shared/streamer_mode.h):
	returns true if \a message (a notification) is held for later because
	notifications are paused; otherwise it may hide its content (only the
	application's name stays) and returns false. Progress notifications are
	not held, only made content-free.
*/
bool
NotificationServer::_StreamerMode(BMessage* message)
{
	StreamerMode::Settings settings;
	StreamerMode::load(settings);
	if (!settings.enabled)
		return false;

	int32 type = 0;
	message->FindInt32("_type", &type);
	if (settings.pauseNotifications && type != B_PROGRESS_NOTIFICATION) {
		if (fHeld.CountItems() < kMaxHeld)
			fHeld.AddItem(new BMessage(*message));
		if (fHeldCheck == NULL) {
			BMessage check(kMsgCheckHeld);
			fHeldCheck = new BMessageRunner(be_app_messenger, &check,
				2000000);
		}
		BMessage reply(B_REPLY);
		reply.AddInt32("error", B_OK);
		message->SendReply(&reply);
		return true;
	}

	if (settings.hideNotificationContent) {
		const char* name = message->GetString("_appname", "");
		message->RemoveName("_title");
		message->RemoveName("_content");
		message->AddString("_title", name);
		message->AddString("_content", "New notification");
	}
	return false;
}


//!	Streamer mode ended (or no longer pauses): the held notifications.
void
NotificationServer::_ReleaseHeld()
{
	StreamerMode::Settings settings;
	StreamerMode::load(settings);
	if (settings.enabled && settings.pauseNotifications)
		return;
	delete fHeldCheck;
	fHeldCheck = NULL;
	while (BMessage* held = fHeld.RemoveItemAt(0)) {
		if (fWindow != NULL && !_StreamerMode(held))
			BMessenger(fWindow).SendMessage(held);
		delete held;
	}
}


void
NotificationServer::ReadyToRun()
{
	fWindow = new NotificationWindow();
}


void
NotificationServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kNotificationMessage:
		{
			// Skip this message if we don't have the window
			if (!fWindow)
				return;

			// Renga: streamer mode may hold it or hide its content
			if (_StreamerMode(message))
				break;

			// Emit a sound for this event
			// Progress notifications only emit a sound if the visible percentage changed
			int32 type = 0;
			if (message->FindInt32("_type", &type) == B_OK) {
				if (type < (int32)(sizeof(kSoundNames) / sizeof(const char*))
					&& type != B_PROGRESS_NOTIFICATION)
					system_beep(kSoundNames[type]);
			}

			// Let the notification window handle this message
			BMessenger(fWindow).SendMessage(message);
			break;
		}
		case kMsgCheckHeld:
			_ReleaseHeld();
			break;
		default:
			BApplication::MessageReceived(message);
	}
}


status_t
NotificationServer::GetSupportedSuites(BMessage* msg)
{
	msg->AddString("suites", "suite/x-vnd.Haiku-notification_server");

	BPropertyInfo info(main_prop_list);
	msg->AddFlat("messages", &info);

	return BApplication::GetSupportedSuites(msg);
}


BHandler*
NotificationServer::ResolveSpecifier(BMessage* msg, int32 index,
	BMessage* spec, int32 from, const char* prop)
{
	BPropertyInfo info(main_prop_list);

	if (strcmp(prop, "message") == 0) {
		BMessenger messenger(fWindow);
		messenger.SendMessage(msg, fWindow);
		return NULL;
	}

	return BApplication::ResolveSpecifier(msg, index, spec, from, prop);
}


// #pragma mark -


int
main(int argc, char* argv[])
{
	int32 i = 0;

	// Add system sounds
	while (kSoundNames[i] != NULL)
		add_system_beep_event(kSoundNames[i++], 0);

	// Start!
	status_t error;
	NotificationServer server(error);
	if (error == B_OK)
		server.Run();

	return error == B_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
