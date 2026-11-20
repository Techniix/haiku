/*
 * Copyright 2010-2017, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NOTIFICATION_SERVER_H
#define _NOTIFICATION_SERVER_H


#include <MessageRunner.h>
#include <ObjectList.h>
#include <Server.h>


class NotificationWindow;


class NotificationServer : public BServer {
public:
								NotificationServer(status_t& error);
	virtual						~NotificationServer();

	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);

	virtual	status_t			GetSupportedSuites(BMessage* msg);
	virtual	BHandler*			ResolveSpecifier(BMessage* msg, int32 index,
									BMessage* spec, int32 form,
									const char* prop);

private:
			bool				_StreamerMode(BMessage* message);
			void				_ReleaseHeld();

			NotificationWindow*	fWindow;
			BObjectList<BMessage, true> fHeld;
				// Renga: held while streamer mode pauses notifications
			BMessageRunner*		fHeldCheck;
};


#endif	// _NOTIFICATION_SERVER_H
