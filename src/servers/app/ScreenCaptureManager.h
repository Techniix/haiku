/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SCREEN_CAPTURE_MANAGER_H
#define SCREEN_CAPTURE_MANAGER_H


#include <Locker.h>
#include <Message.h>
#include <ObjectList.h>
#include <OS.h>
#include <Region.h>
#include <String.h>

#include "HWInterface.h"


namespace BPrivate {
	class LinkReceiver;
	class PortLink;
}

class Desktop;
class ServerApp;
class ServerBitmap;


/*!	Screen capture for recording and streaming applications (Renga, F-31
	phase 3).

	A session records the screen, one window or a region. Its client asks
	for frames (AS_SCREEN_CAPTURE_FRAME) into BBitmaps it owns; app_server
	copies only what changed since that bitmap was last filled, from the
	front buffer, so a static screen costs nothing but the request. Damage
	comes from HWInterface::Invalidate(), through which every change of the
	front buffer passes (drawing, copies, cursor moves). Each frame carries
	the region that changed since the previous frame (for encoders) and the
	system_time() it was taken at.

	Permission: a session needs the user's consent for the requesting
	application (by signature). Unknown applications get a prompt from the
	Screen recording preferences ("--prompt"), which app_server starts; the
	answer can be remembered. While any session runs, app_server keeps the
	indicator ("--indicator", a floating window with a Stop button) running.
	Answers and permission changes are only accepted from system software
	(see IsTrustedTeam()). Haiku has no sandbox: an application can still
	read the screen with BScreen::ReadBitmap() or BDirectWindow, so this is
	the user's consent and a visible indicator for the well-behaved capture
	path, not a security boundary.
*/
class ScreenCaptureManager : public HWInterfaceDamageListener {
public:
								ScreenCaptureManager(Desktop* desktop);
								~ScreenCaptureManager();

	// Called by HWInterface::Invalidate(), with the HWInterface locked
	// (possibly in parallel by several threads).
	virtual	void				ScreenDamaged(const BRect& frame);

	// Message handlers (called from ServerApp's thread).
			void				HandleMessage(ServerApp* app, int32 code,
									BPrivate::LinkReceiver& link,
									BPrivate::PortLink& reply);

			void				ApplicationQuit(team_id team);

	static	bool				IsTrustedTeam(team_id team);

private:
	struct Session;
	struct Request;

			void				_Start(ServerApp* app,
									BPrivate::LinkReceiver& link,
									BPrivate::PortLink& reply);
			void				_Frame(ServerApp* app,
									BPrivate::LinkReceiver& link,
									BPrivate::PortLink& reply);
			void				_Stop(ServerApp* app, int32 id,
									bool any, BPrivate::PortLink& reply);
			void				_List(BPrivate::PortLink& reply);
			void				_GetPermissions(BPrivate::PortLink& reply);
			void				_SetPermission(ServerApp* app,
									BPrivate::LinkReceiver& link,
									BPrivate::PortLink& reply);
			void				_GetRequest(ServerApp* app,
									BPrivate::LinkReceiver& link,
									BPrivate::PortLink& reply);
			void				_AnswerRequest(ServerApp* app,
									BPrivate::LinkReceiver& link,
									BPrivate::PortLink& reply);

			int32				_Permission(const char* signature) const;
			void				_SetPermissionLocked(const char* signature,
									const char* name, int32 state);
			void				_LoadPermissions();
			void				_SavePermissions();

			bool				_SourceFrame(Session* session, BRect& frame);
			Session*			_FindSession(int32 id);
			void				_RemoveSession(Session* session);
			void				_UpdateActive();
			void				_EnsureIndicator();
			team_id				_LaunchAgent(const char* mode,
									const char* argument);

private:
			Desktop*			fDesktop;
			BLocker				fLock;
			int32				fActiveSessions;
				// atomic; read without fLock in ScreenDamaged()
			BObjectList<Session, true> fSessions;
			BObjectList<Request, true> fRequests;
			int32				fNextID;
			BMessage			fPermissions;
				// "signature" -> state, "name:<signature>" -> name
			BObjectList<BString, true> fAllowedOnce;
				// "<team>" of applications allowed until they quit
			team_id				fIndicatorTeam;
};


#endif	// SCREEN_CAPTURE_MANAGER_H
