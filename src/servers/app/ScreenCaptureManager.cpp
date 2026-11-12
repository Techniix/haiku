/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ScreenCaptureManager.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>

#include <LinkReceiver.h>
#include <MessengerPrivate.h>
#include <PthreadMutexLocker.h>
#include <PortLink.h>
#include <ScreenCaptureDefs.h>
#include <ServerProtocol.h>
#include <TokenSpace.h>

#include "Desktop.h"
#include "DrawingEngine.h"
#include "HWInterface.h"
#include "RenderingBuffer.h"
#include "ServerApp.h"
#include "ServerBitmap.h"
#include "ServerCursor.h"
#include "ServerWindow.h"
#include "Window.h"
#include "decorator/Decorator.h"


using namespace BPrivate;


extern char** environ;


static const char* kPermissionsFile = "screen_capture_permissions";
static const int32 kMaxBitmapsPerSession = 8;


struct ScreenCaptureManager::Session {
	int32		id;
	team_id		team;
	BString		signature;
	BString		name;
	int32		source;
	int32		windowToken;
	BRect		region;
	uint32		flags;
	bigtime_t	started;
	int64		frames;
	bool		stopped;		// by the user (indicator) or the window went
	BRect		lastFrame;		// source frame of the previous frame
	BRegion		sinceLastFrame;	// screen coordinates
	int32		bitmapTokens[kMaxBitmapsPerSession];
	BRegion		missed[kMaxBitmapsPerSession];
		// what each bitmap lacks, screen coordinates
	BRect		bitmapFrames[kMaxBitmapsPerSession];
		// the source frame each bitmap was last filled for
	int32		bitmapCount;
	sem_id		notify;			// F-58 (O-8): released on damage, or -1
	bool		notified;		// released since the last frame

	Session()
		:
		frames(0),
		stopped(false),
		bitmapCount(0),
		notify(-1),
		notified(false)
	{
	}
};


struct ScreenCaptureManager::Request {
	int32		id;
	team_id		team;
	BString		signature;
	BString		name;
	BString		path;
	int32		source;
	team_id		prompt;
	int32		answer;		// SCREEN_CAPTURE_PERMISSION_*; UNKNOWN: pending
	bigtime_t	created;
	BObjectList<BMessenger, true> waiters;
		// F-58: whom to tell the answer (RequestPermission())
	uint32		sources;	// what they asked for (SCREEN_CAPTURE_*_MASK)

	Request()
		:
		waiters(2),
		sources(0)
	{
	}
};


struct ScreenCaptureManager::Answer {
	BMessenger	target;
	BMessage	message;

	Answer(const BMessenger& target, status_t status, int32 request,
			uint32 sources)
		:
		target(target),
		message(SCREEN_CAPTURE_PERMISSION_ANSWER)
	{
		message.AddInt32("status", status);
		message.AddInt32("request", request);
		message.AddUInt32("sources", sources);
	}
};


static BString
team_path(team_id team)
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(team, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE)
			return BString(info.name);
	}
	return BString();
}


static BString
leaf_name(const BString& path)
{
	int32 slash = path.FindLast('/');
	if (slash < 0)
		return path;
	BString leaf;
	path.CopyInto(leaf, slash + 1, path.Length() - slash - 1);
	return leaf;
}


ScreenCaptureManager::ScreenCaptureManager(Desktop* desktop)
	:
	fDesktop(desktop),
	fLock("screen capture"),
	fActiveSessions(0),
	fSessions(4),
	fRequests(4),
	fNextID(1),
	fAllowedOnce(4),
	fIndicatorTeam(-1)
{
	_LoadPermissions();
}


ScreenCaptureManager::~ScreenCaptureManager()
{
}


/*!	Only system software may answer permission requests, change
	permissions or stop other applications' sessions: a team whose
	executable lies under /boot/system (read-only, from packages), but not
	under /boot/system/non-packaged.
*/
/*static*/ bool
ScreenCaptureManager::IsTrustedTeam(team_id team)
{
	BString path = team_path(team);
	return path.StartsWith("/boot/system/")
		&& !path.StartsWith("/boot/system/non-packaged/");
}


void
ScreenCaptureManager::ScreenDamaged(const BRect& frame)
{
	if (atomic_get(&fActiveSessions) == 0 || !frame.IsValid())
		return;

	BAutolock locker(fLock);
	for (int32 i = 0; Session* session = fSessions.ItemAt(i); i++) {
		session->sinceLastFrame.Include(frame);
		for (int32 j = 0; j < session->bitmapCount; j++)
			session->missed[j].Include(frame);
		// F-58 (O-8): once per frame, when the source's area changed
		if (session->notify >= 0 && !session->notified
			&& (session->frames == 0 || session->lastFrame.Intersects(frame))) {
			session->notified = true;
			release_sem_etc(session->notify, 1, B_DO_NOT_RESCHEDULE);
		}
	}
}


void
ScreenCaptureManager::ApplicationQuit(team_id team)
{
	BObjectList<Answer, true> answers(4);
	BAutolock locker(fLock);
	for (int32 i = 0; Request* request = fRequests.ItemAt(i); i++) {
		// F-58: a prompt that went away without an answer (closed,
		// crashed); its request stays until the next Start...() takes it
		// as a no for now, as before.
		if (request->prompt == team
			&& request->answer == SCREEN_CAPTURE_PERMISSION_UNKNOWN) {
			_CollectAnswers(request, B_CANCELED, answers);
		}
	}
	for (int32 i = fSessions.CountItems() - 1; i >= 0; i--) {
		if (fSessions.ItemAt(i)->team == team)
			delete fSessions.RemoveItemAt(i);
	}
	for (int32 i = fRequests.CountItems() - 1; i >= 0; i--) {
		if (fRequests.ItemAt(i)->team == team)
			delete fRequests.RemoveItemAt(i);
	}
	BString key;
	key << team;
	for (int32 i = fAllowedOnce.CountItems() - 1; i >= 0; i--) {
		if (*fAllowedOnce.ItemAt(i) == key)
			delete fAllowedOnce.RemoveItemAt(i);
	}
	_UpdateActive();
	locker.Unlock();
	_SendAnswers(answers);
}


void
ScreenCaptureManager::HandleMessage(ServerApp* app, int32 code,
	LinkReceiver& link, PortLink& reply)
{
	switch (code) {
		case AS_SCREEN_CAPTURE_START:
			_Start(app, link, reply);
			break;
		case AS_SCREEN_CAPTURE_FRAME:
			_Frame(app, link, reply);
			break;
		case AS_SCREEN_CAPTURE_STOP:
		case AS_SCREEN_CAPTURE_STOP_ANY:
		{
			int32 id;
			link.Read<int32>(&id);
			_Stop(app, id, code == AS_SCREEN_CAPTURE_STOP_ANY, reply);
			break;
		}
		case AS_SCREEN_CAPTURE_LIST:
			_List(reply);
			break;
		case AS_SCREEN_CAPTURE_GET_PERMISSIONS:
			_GetPermissions(reply);
			break;
		case AS_SCREEN_CAPTURE_SET_PERMISSION:
			_SetPermission(app, link, reply);
			break;
		case AS_SCREEN_CAPTURE_GET_REQUEST:
			_GetRequest(app, link, reply);
			break;
		case AS_SCREEN_CAPTURE_ANSWER_REQUEST:
			_AnswerRequest(app, link, reply);
			break;
		case AS_SCREEN_CAPTURE_REQUEST_PERMISSION:
			_RequestPermission(app, link, reply);
			break;
		case AS_SCREEN_CAPTURE_SET_NOTIFICATION:
			_SetNotification(app, link, reply);
			break;
	}
	reply.Flush();
}


/*!	The user's decision for \a team's application, with fLock held:
	remembered by signature, for this run of the application (allowed
	once), or still to be asked. Returns SCREEN_CAPTURE_PERMISSION_ALLOWED
	or _DENIED, or _UNKNOWN with *_request set to the pending request
	(NULL if there is none yet). An answered request is used up here: an
	answer without "remember" holds for this run of the application if it
	was yes, and for this one call if it was no.
*/
int32
ScreenCaptureManager::_Decide(team_id team, const BString& signature,
	Request** _request)
{
	*_request = NULL;
	int32 permission = _Permission(signature);
	BString teamKey;
	teamKey << team;
	for (int32 i = 0; i < fAllowedOnce.CountItems(); i++) {
		if (*fAllowedOnce.ItemAt(i) == teamKey)
			permission = SCREEN_CAPTURE_PERMISSION_ALLOWED;
	}
	Request* request = NULL;
	for (int32 i = 0; Request* item = fRequests.ItemAt(i); i++) {
		if (item->team == team)
			request = item;
	}
	if (request != NULL
		&& request->answer != SCREEN_CAPTURE_PERMISSION_UNKNOWN) {
		// Answered, possibly without "remember": this run only.
		permission = request->answer;
		if (permission == SCREEN_CAPTURE_PERMISSION_ALLOWED)
			fAllowedOnce.AddItem(new BString(teamKey));
		fRequests.RemoveItem(request, true);
		request = NULL;
	}
	if (permission == SCREEN_CAPTURE_PERMISSION_ALLOWED
		|| permission == SCREEN_CAPTURE_PERMISSION_DENIED) {
		return permission;
	}

	if (request != NULL) {
		// The prompt went away without an answer (closed, crashed): that
		// is a no for now.
		team_info info;
		if (request->prompt >= 0
			&& get_team_info(request->prompt, &info) != B_OK) {
			fRequests.RemoveItem(request, true);
			return SCREEN_CAPTURE_PERMISSION_DENIED;
		}
	}
	*_request = request;
	return SCREEN_CAPTURE_PERMISSION_UNKNOWN;
}


//!	A new request, with the prompt started; fLock held. NULL on failure.
ScreenCaptureManager::Request*
ScreenCaptureManager::_NewRequest(team_id team, const BString& signature,
	const BString& path, int32 source)
{
	Request* request = new(std::nothrow) Request;
	if (request == NULL)
		return NULL;
	request->id = fNextID++;
	request->team = team;
	request->signature = signature;
	request->path = path;
	request->name = leaf_name(path);
	request->source = source;
	request->answer = SCREEN_CAPTURE_PERMISSION_UNKNOWN;
	request->created = system_time();
	BString argument;
	argument << request->id;
	request->prompt = _LaunchAgent("--prompt", argument.String());
	if (request->prompt < 0) {
		delete request;
		return NULL;
	}
	if (!fRequests.AddItem(request)) {
		delete request;
		return NULL;
	}
	return request;
}


//!	Moves \a request's waiters into \a answers, with \a status; fLock held.
void
ScreenCaptureManager::_CollectAnswers(Request* request, status_t status,
	BObjectList<Answer, true>& answers)
{
	while (BMessenger* target = request->waiters.RemoveItemAt(0)) {
		Answer* answer = new(std::nothrow) Answer(*target, status,
			request->id, request->sources);
		delete target;
		if (answer != NULL && !answers.AddItem(answer))
			delete answer;
	}
}


//!	Sends the answers collected; without fLock (a target may be slow).
/*static*/ void
ScreenCaptureManager::_SendAnswers(BObjectList<Answer, true>& answers)
{
	for (int32 i = 0; Answer* answer = answers.ItemAt(i); i++)
		answer->target.SendMessage(&answer->message, (BHandler*)NULL, 500000);
}


/*!	AS_SCREEN_CAPTURE_REQUEST_PERMISSION (F-58, OBS Studio's O-7): uint32
	sources (SCREEN_CAPTURE_*_MASK), team_id, port_id and int32 token of the
	messenger to answer. Replies at once: B_OK with int32 request (-1 if the
	answer is already known; it is sent right away) and bool pending. The
	answer comes as a SCREEN_CAPTURE_PERMISSION_ANSWER message (see
	ScreenCaptureDefs.h) when the user has answered, or when the prompt was
	closed without an answer (B_CANCELED). The prompt is the same one a
	waiting Start...() of the application shares.
*/
void
ScreenCaptureManager::_RequestPermission(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	uint32 sources;
	team_id targetTeam;
	port_id targetPort;
	int32 targetToken;
	link.Read<uint32>(&sources);
	link.Read<team_id>(&targetTeam);
	link.Read<port_id>(&targetPort);
	if (link.Read<int32>(&targetToken) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}
	sources &= SCREEN_CAPTURE_ANY_MASK;
	if (sources == 0 || targetPort < 0) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}
	int32 source = (sources & SCREEN_CAPTURE_SCREEN_MASK) != 0
		? SCREEN_CAPTURE_SCREEN : (sources & SCREEN_CAPTURE_WINDOW_MASK) != 0
			? SCREEN_CAPTURE_WINDOW : SCREEN_CAPTURE_REGION;

	BMessenger target;
	BMessenger::Private(target).SetTo(targetTeam, targetPort, targetToken);

	team_id team = app->ClientTeam();
	BString signature(app->Signature());
	BString path = team_path(team);

	BObjectList<Answer, true> answers(1);
	BAutolock locker(fLock);
	Request* request = NULL;
	int32 permission = _Decide(team, signature, &request);
	if (permission == SCREEN_CAPTURE_PERMISSION_UNKNOWN && request == NULL)
		request = _NewRequest(team, signature, path, source);

	if (permission != SCREEN_CAPTURE_PERMISSION_UNKNOWN || request == NULL) {
		// Known already (or nobody can ask: no).
		status_t status = permission == SCREEN_CAPTURE_PERMISSION_ALLOWED
			? B_OK : B_NOT_ALLOWED;
		Answer* answer = new(std::nothrow) Answer(target, status, -1,
			sources);
		if (answer != NULL)
			answers.AddItem(answer);
		reply.StartMessage(B_OK);
		reply.Attach<int32>(-1);
		reply.Attach<bool>(false);
		locker.Unlock();
		reply.Flush();
		_SendAnswers(answers);
		return;
	}

	BMessenger* waiter = new(std::nothrow) BMessenger(target);
	if (waiter == NULL || !request->waiters.AddItem(waiter)) {
		delete waiter;
		reply.StartMessage(B_NO_MEMORY);
		return;
	}
	request->sources |= sources;
	reply.StartMessage(B_OK);
	reply.Attach<int32>(request->id);
	reply.Attach<bool>(true);
}


/*!	AS_SCREEN_CAPTURE_START: int32 source, int32 window token, BRect
	region, uint32 flags. Replies B_OK with int32 session and BRect frame;
	B_WOULD_BLOCK with int32 request while the user has not answered (the
	client asks again); B_NOT_ALLOWED if the user said no.
*/
void
ScreenCaptureManager::_Start(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	int32 source;
	int32 windowToken;
	BRect region;
	uint32 flags;
	link.Read<int32>(&source);
	link.Read<int32>(&windowToken);
	link.Read<BRect>(&region);
	if (link.Read<uint32>(&flags) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}
	if (source < SCREEN_CAPTURE_SCREEN || source > SCREEN_CAPTURE_REGION
		|| (source == SCREEN_CAPTURE_REGION && !region.IsValid())) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}

	team_id team = app->ClientTeam();
	BString signature(app->Signature());
	BString path = team_path(team);

	BAutolock locker(fLock);

	Request* request = NULL;
	int32 permission = _Decide(team, signature, &request);
	if (permission == SCREEN_CAPTURE_PERMISSION_DENIED) {
		reply.StartMessage(B_NOT_ALLOWED);
		return;
	}
	if (permission != SCREEN_CAPTURE_PERMISSION_ALLOWED) {
		if (request == NULL) {
			request = _NewRequest(team, signature, path, source);
			if (request == NULL) {
				// Nobody can ask the user (or no memory): no.
				reply.StartMessage(B_NOT_ALLOWED);
				return;
			}
		}
		reply.StartMessage(B_WOULD_BLOCK);
		reply.Attach<int32>(request->id);
		return;
	}

	Session* session = new(std::nothrow) Session;
	if (session == NULL) {
		reply.StartMessage(B_NO_MEMORY);
		return;
	}
	session->id = fNextID++;
	session->team = team;
	session->signature = signature;
	session->name = leaf_name(path);
	session->source = source;
	session->windowToken = windowToken;
	session->region = region;
	session->flags = flags;
	session->started = system_time();

	locker.Unlock();
	BRect frame;
	if (!_SourceFrame(session, frame)) {
		delete session;
		reply.StartMessage(B_ENTRY_NOT_FOUND);
		return;
	}
	locker.Lock();
	session->lastFrame = frame;
	fSessions.AddItem(session);
	_UpdateActive();
	_EnsureIndicator();

	reply.StartMessage(B_OK);
	reply.Attach<int32>(session->id);
	reply.Attach<BRect>(frame);
}


/*!	The source's frame in screen coordinates now: the screen, the region
	(clipped to the screen), or the window's frame (with its decorator if
	asked). Must be called without fLock (it takes the window lock).
*/
bool
ScreenCaptureManager::_SourceFrame(Session* session, BRect& frame,
	uint32* _flags)
{
	if (_flags != NULL)
		*_flags = 0;
	BRect screen = fDesktop->VirtualScreen().Frame();
	switch (session->source) {
		case SCREEN_CAPTURE_SCREEN:
			frame = screen;
			return true;
		case SCREEN_CAPTURE_REGION:
			frame = session->region & screen;
			return frame.IsValid();
		case SCREEN_CAPTURE_WINDOW:
		{
			if (!fDesktop->LockSingleWindow())
				return false;
			bool found = false;
			{
				PthreadMutexLocker tokenLocker(BPrivate::gDefaultTokens.GetLock());
				::ServerWindow* serverWindow;
				if (BPrivate::gDefaultTokens.GetToken(session->windowToken,
						B_SERVER_TOKEN, (void**)&serverWindow) == B_OK
					&& serverWindow->Window() != NULL) {
					::Window* window = serverWindow->Window();
					frame = window->Frame();
					if ((session->flags & SCREEN_CAPTURE_WINDOW_BORDER) != 0
						&& window->Decorator() != NULL) {
						frame = frame
							| window->Decorator()->GetFootprint().Frame();
					}
					BRect whole = frame;
					frame = frame & screen;
					found = frame.IsValid();
					// F-58 (OBS Studio's O-9): whether the window's area
					// shows the window
					if (_flags != NULL) {
						uint32 flags = 0;
						if (window->IsHidden() || window->IsMinimized())
							flags |= SCREEN_CAPTURE_FRAME_HIDDEN;
						if (!window->InWorkspace(
								fDesktop->CurrentWorkspace()))
							flags |= SCREEN_CAPTURE_FRAME_OTHER_WORKSPACE;
						if (flags == 0) {
							BRegion visible(window->VisibleRegion());
							BRegion wanted(found ? frame : whole);
							BRegion shown(wanted);
							shown.IntersectWith(&visible);
							wanted.Exclude(&visible);
							if (shown.CountRects() == 0)
								flags |= SCREEN_CAPTURE_FRAME_FULLY_COVERED
									| SCREEN_CAPTURE_FRAME_COVERED;
							else if (wanted.CountRects() > 0)
								flags |= SCREEN_CAPTURE_FRAME_COVERED;
						}
						*_flags = flags;
					}
				}
			}
			fDesktop->UnlockSingleWindow();
			return found;
		}
	}
	return false;
}


/*!	AS_SCREEN_CAPTURE_FRAME: int32 session, int32 bitmap token. Fills the
	client's bitmap (B_RGB32 or B_RGBA32 recommended; other spaces are
	converted) with what changed since that bitmap was filled. Replies B_OK
	with bool changed, BRect source frame (screen coordinates), bigtime_t
	time, int32 count and count clipping_rects (what changed since the
	previous frame, in frame coordinates); B_CANCELED if the user stopped
	the session, B_ENTRY_NOT_FOUND if its window is gone.
*/
void
ScreenCaptureManager::_Frame(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	int32 id;
	int32 bitmapToken;
	link.Read<int32>(&id);
	if (link.Read<int32>(&bitmapToken) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}

	BAutolock locker(fLock);
	Session* session = _FindSession(id);
	if (session == NULL || session->team != app->ClientTeam()) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}
	if (session->stopped) {
		reply.StartMessage(B_CANCELED);
		return;
	}
	locker.Unlock();

	BReference<ServerBitmap> bitmap(app->GetBitmap(bitmapToken), true);
	if (bitmap == NULL) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}

	BRect frame;
	uint32 visibility = 0;
	if (!_SourceFrame(session, frame, &visibility)) {
		locker.Lock();
		if ((session = _FindSession(id)) != NULL) {
			session->stopped = true;
			if (session->notify >= 0)
				release_sem_etc(session->notify, 1, B_DO_NOT_RESCHEDULE);
		}
		reply.StartMessage(B_ENTRY_NOT_FOUND);
		return;
	}

	DrawingEngine* engine = fDesktop->GetDrawingEngine();
	::HWInterface* interface = fDesktop->HWInterface();
	if (!engine->LockExclusiveAccess()) {
		reply.StartMessage(B_ERROR);
		return;
	}

	locker.Lock();
	session = _FindSession(id);
	if (session == NULL) {
		locker.Unlock();
		engine->UnlockExclusiveAccess();
		reply.StartMessage(B_BAD_VALUE);
		return;
	}

	// This bitmap's slot: what it lacks since it was last filled.
	int32 slot = -1;
	for (int32 i = 0; i < session->bitmapCount; i++) {
		if (session->bitmapTokens[i] == bitmapToken)
			slot = i;
	}
	if (slot < 0) {
		if (session->bitmapCount < kMaxBitmapsPerSession)
			slot = session->bitmapCount++;
		else
			slot = 0;
		session->bitmapTokens[slot] = bitmapToken;
		session->bitmapFrames[slot] = BRect();
	}
	BRegion copy;
	if (session->bitmapFrames[slot] != frame) {
		// New bitmap, or the window moved or changed size: all of it.
		copy.Set(frame);
	} else {
		copy = session->missed[slot];
		BRegion frameRegion(frame);
		copy.IntersectWith(&frameRegion);
	}
	session->missed[slot].MakeEmpty();
	session->bitmapFrames[slot] = frame;

	BRegion damage;
	if (session->lastFrame != frame || session->frames == 0)
		damage.Set(frame);
	else {
		damage = session->sinceLastFrame;
		BRegion frameRegion(frame);
		damage.IntersectWith(&frameRegion);
	}
	session->sinceLastFrame.MakeEmpty();
	session->lastFrame = frame;
	session->notified = false;
		// F-58 (O-8): the next change releases the semaphore again
	bool includeCursor = (session->flags & SCREEN_CAPTURE_INCLUDE_CURSOR) != 0;
	session->frames++;
	locker.Unlock();

	// Copy from the back buffer when there is one (it never holds the
	// cursor); otherwise from the front buffer with the cursor hidden.
	RenderingBuffer* buffer = interface->IsDoubleBuffered()
		? interface->BackBuffer() : interface->FrontBuffer();
	bool overlaysHidden = false;
	if (buffer != NULL && copy.CountRects() > 0 && !interface->IsDoubleBuffered())
		overlaysHidden = interface->HideFloatingOverlays(copy.Frame());

	if (buffer != NULL) {
		for (int32 i = 0; i < copy.CountRects(); i++) {
			BRect rect = copy.RectAt(i);
			bitmap->ImportBits(buffer->Bits(), buffer->BitsLength(),
				buffer->BytesPerRow(), buffer->ColorSpace(), rect.LeftTop(),
				rect.LeftTop() - frame.LeftTop(), rect.IntegerWidth() + 1,
				rect.IntegerHeight() + 1);
		}
	}

	if (overlaysHidden)
		interface->ShowFloatingOverlays();

	// The cursor, drawn into the bitmap like DrawingEngine::ReadBitmap()
	// does. Its old and new frames are part of the damage when it moves or
	// changes (HWInterface invalidates them), so a still cursor does not
	// make every frame a changed one.
	if (includeCursor && buffer != NULL
		&& (bitmap->ColorSpace() == B_RGB32
			|| bitmap->ColorSpace() == B_RGBA32)) {
		ServerCursorReference cursorReference = interface->Cursor();
		ServerCursor* cursor = cursorReference.Get();
		if (cursor != NULL && interface->IsCursorVisible()) {
			BPoint position = interface->CursorPosition()
				- cursor->GetHotSpot();
			BRect cursorFrame(position.x, position.y,
				position.x + cursor->Width() - 1,
				position.y + cursor->Height() - 1);
			// Restore what was under the cursor last time it was drawn in:
			// the region of the cursor is always copied again.
			BRect inFrame = cursorFrame & frame;
			if (inFrame.IsValid()) {
				uint8* bits = bitmap->Bits();
				int32 bpr = bitmap->BytesPerRow();
				const uint8* cursorBits = cursor->Bits();
				int32 cursorBPR = cursor->BytesPerRow();
				for (int32 y = (int32)inFrame.top; y <= (int32)inFrame.bottom;
						y++) {
					int32 by = y - (int32)frame.top;
					int32 cy = y - (int32)cursorFrame.top;
					if (by >= bitmap->Height())
						break;
					for (int32 x = (int32)inFrame.left;
							x <= (int32)inFrame.right; x++) {
						int32 bx = x - (int32)frame.left;
						int32 cx = x - (int32)cursorFrame.left;
						if (bx >= bitmap->Width())
							break;
						uint8* d = bits + by * bpr + bx * 4;
						const uint8* c = cursorBits + cy * cursorBPR + cx * 4;
						uint8 alpha = 255 - c[3];
						d[0] = ((d[0] * alpha) >> 8) + c[0];
						d[1] = ((d[1] * alpha) >> 8) + c[1];
						d[2] = ((d[2] * alpha) >> 8) + c[2];
					}
				}
				// The pixels under the cursor have to be copied again for
				// the next frame into this bitmap.
				locker.Lock();
				if ((session = _FindSession(id)) != NULL)
					session->missed[slot].Include(inFrame);
				locker.Unlock();
			}
		}
	}

	engine->UnlockExclusiveAccess();
	bigtime_t time = system_time();

	reply.StartMessage(B_OK);
	reply.Attach<bool>(damage.CountRects() > 0);
	reply.Attach<BRect>(frame);
	reply.Attach<bigtime_t>(time);
	int32 count = damage.CountRects();
	// Keep the reply small: many rectangles are sent as their bounds.
	if (count > 64) {
		BRect bounds = damage.Frame();
		damage.Set(bounds);
		count = 1;
	}
	reply.Attach<int32>(count);
	for (int32 i = 0; i < count; i++) {
		BRect rect = damage.RectAt(i);
		rect.OffsetBy(-frame.LeftTop());
		reply.Attach<BRect>(rect);
	}
	// F-58 (O-9), after the rectangles: older clients stop reading before
	reply.Attach<uint32>(visibility);
}


/*!	AS_SCREEN_CAPTURE_SET_NOTIFICATION (F-58, OBS Studio's O-8): int32
	session, sem_id. The semaphore (the client's) is released once when
	the source's area changes after a frame was taken, and when the
	session is stopped; -1 ends the notification. Replies B_OK.
*/
void
ScreenCaptureManager::_SetNotification(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	int32 id;
	sem_id semaphore;
	link.Read<int32>(&id);
	if (link.Read<sem_id>(&semaphore) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}
	sem_info info;
	if (semaphore >= 0 && (get_sem_info(semaphore, &info) != B_OK
			|| info.team != app->ClientTeam())) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}
	BAutolock locker(fLock);
	Session* session = _FindSession(id);
	if (session == NULL || session->team != app->ClientTeam()) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}
	session->notify = semaphore;
	// a change since the last frame is reported at once
	session->notified = false;
	if (semaphore >= 0 && (session->sinceLastFrame.CountRects() > 0
			|| session->stopped)) {
		session->notified = true;
		release_sem_etc(semaphore, 1, B_DO_NOT_RESCHEDULE);
	}
	reply.StartMessage(B_OK);
}


void
ScreenCaptureManager::_Stop(ServerApp* app, int32 id, bool any,
	PortLink& reply)
{
	if (any && !IsTrustedTeam(app->ClientTeam())) {
		reply.StartMessage(B_NOT_ALLOWED);
		return;
	}
	BAutolock locker(fLock);
	Session* session = _FindSession(id);
	if (session == NULL || (!any && session->team != app->ClientTeam())) {
		reply.StartMessage(B_BAD_VALUE);
		return;
	}
	if (any && session->team != app->ClientTeam()) {
		// Stopped by the user: the owner learns it at its next frame.
		session->stopped = true;
		if (session->notify >= 0)
			release_sem_etc(session->notify, 1, B_DO_NOT_RESCHEDULE);
	} else
		_RemoveSession(session);
	_UpdateActive();
	reply.StartMessage(B_OK);
}


/*!	AS_SCREEN_CAPTURE_LIST: int32 count, then per session int32 id, team_id,
	signature, name, int32 source, BRect frame, bigtime_t started, int64
	frames. Sessions the user stopped are not listed.
*/
void
ScreenCaptureManager::_List(PortLink& reply)
{
	BAutolock locker(fLock);
	int32 count = 0;
	for (int32 i = 0; Session* session = fSessions.ItemAt(i); i++) {
		if (!session->stopped)
			count++;
	}
	reply.StartMessage(B_OK);
	reply.Attach<int32>(count);
	for (int32 i = 0; Session* session = fSessions.ItemAt(i); i++) {
		if (session->stopped)
			continue;
		reply.Attach<int32>(session->id);
		reply.Attach<team_id>(session->team);
		reply.AttachString(session->signature.String());
		reply.AttachString(session->name.String());
		reply.Attach<int32>(session->source);
		reply.Attach<BRect>(session->lastFrame);
		reply.Attach<bigtime_t>(session->started);
		reply.Attach<int64>(session->frames);
	}
}


void
ScreenCaptureManager::_GetPermissions(PortLink& reply)
{
	BAutolock locker(fLock);
	int32 count = 0;
	type_code type;
	char* name;
	for (int32 i = 0; fPermissions.GetInfo(B_INT32_TYPE, i, &name, &type)
			== B_OK; i++) {
		count++;
	}
	reply.StartMessage(B_OK);
	reply.Attach<int32>(count);
	for (int32 i = 0; fPermissions.GetInfo(B_INT32_TYPE, i, &name, &type)
			== B_OK; i++) {
		BString key("name:");
		key << name;
		reply.AttachString(name);
		reply.AttachString(fPermissions.GetString(key.String(), name));
		reply.Attach<int32>(fPermissions.GetInt32(name, 0));
	}
}


void
ScreenCaptureManager::_SetPermission(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	BString signature;
	int32 state;
	link.ReadString(signature);
	if (link.Read<int32>(&state) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}
	if (!IsTrustedTeam(app->ClientTeam())) {
		reply.StartMessage(B_NOT_ALLOWED);
		return;
	}
	BAutolock locker(fLock);
	BString key("name:");
	key << signature;
	BString name = fPermissions.GetString(key.String(), signature.String());
	_SetPermissionLocked(signature, name, state);
	reply.StartMessage(B_OK);
}


void
ScreenCaptureManager::_GetRequest(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	int32 id;
	if (link.Read<int32>(&id) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}
	if (!IsTrustedTeam(app->ClientTeam())) {
		reply.StartMessage(B_NOT_ALLOWED);
		return;
	}
	BAutolock locker(fLock);
	for (int32 i = 0; Request* request = fRequests.ItemAt(i); i++) {
		if (request->id != id)
			continue;
		reply.StartMessage(B_OK);
		reply.AttachString(request->signature.String());
		reply.AttachString(request->name.String());
		reply.AttachString(request->path.String());
		reply.Attach<int32>(request->source);
		return;
	}
	reply.StartMessage(B_ENTRY_NOT_FOUND);
}


void
ScreenCaptureManager::_AnswerRequest(ServerApp* app, LinkReceiver& link,
	PortLink& reply)
{
	int32 id;
	bool allow;
	bool remember;
	link.Read<int32>(&id);
	link.Read<bool>(&allow);
	if (link.Read<bool>(&remember) != B_OK) {
		reply.StartMessage(B_BAD_DATA);
		return;
	}
	if (!IsTrustedTeam(app->ClientTeam())) {
		reply.StartMessage(B_NOT_ALLOWED);
		return;
	}
	BObjectList<Answer, true> answers(2);
	BAutolock locker(fLock);
	for (int32 i = 0; Request* request = fRequests.ItemAt(i); i++) {
		if (request->id != id)
			continue;
		request->answer = allow ? SCREEN_CAPTURE_PERMISSION_ALLOWED
			: SCREEN_CAPTURE_PERMISSION_DENIED;
		if (remember) {
			_SetPermissionLocked(request->signature, request->name,
				request->answer);
		}
		// F-58: tell those who asked asynchronously. The answer stays in
		// the request for the application's next Start...().
		_CollectAnswers(request, allow ? B_OK : B_NOT_ALLOWED, answers);
		reply.StartMessage(B_OK);
		locker.Unlock();
		_SendAnswers(answers);
		return;
	}
	reply.StartMessage(B_ENTRY_NOT_FOUND);
}


int32
ScreenCaptureManager::_Permission(const char* signature) const
{
	return fPermissions.GetInt32(signature, SCREEN_CAPTURE_PERMISSION_UNKNOWN);
}


void
ScreenCaptureManager::_SetPermissionLocked(const char* signature,
	const char* name, int32 state)
{
	BString key("name:");
	key << signature;
	fPermissions.RemoveName(signature);
	fPermissions.RemoveName(key.String());
	if (state == SCREEN_CAPTURE_PERMISSION_ALLOWED
		|| state == SCREEN_CAPTURE_PERMISSION_DENIED) {
		fPermissions.AddInt32(signature, state);
		fPermissions.AddString(key.String(), name);
	}
	if (state != SCREEN_CAPTURE_PERMISSION_ALLOWED) {
		// Revoked: running sessions of that application end.
		for (int32 i = 0; Session* session = fSessions.ItemAt(i); i++) {
			if (session->signature == signature) {
				session->stopped = true;
				if (session->notify >= 0)
					release_sem_etc(session->notify, 1, B_DO_NOT_RESCHEDULE);
			}
		}
		fAllowedOnce.MakeEmpty();
		_UpdateActive();
	}
	_SavePermissions();
}


static status_t
permissions_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("system/app_server");
	if (status == B_OK)
		status = create_directory(path.Path(), 0755);
	if (status == B_OK)
		status = path.Append(kPermissionsFile);
	return status;
}


void
ScreenCaptureManager::_LoadPermissions()
{
	BPath path;
	if (permissions_path(path) != B_OK)
		return;
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() == B_OK)
		fPermissions.Unflatten(&file);
}


void
ScreenCaptureManager::_SavePermissions()
{
	BPath path;
	if (permissions_path(path) != B_OK)
		return;
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		fPermissions.Flatten(&file);
}


ScreenCaptureManager::Session*
ScreenCaptureManager::_FindSession(int32 id)
{
	for (int32 i = 0; Session* session = fSessions.ItemAt(i); i++) {
		if (session->id == id)
			return session;
	}
	return NULL;
}


void
ScreenCaptureManager::_RemoveSession(Session* session)
{
	fSessions.RemoveItem(session, true);
}


//!	Called with fLock held.
void
ScreenCaptureManager::_UpdateActive()
{
	int32 active = 0;
	for (int32 i = 0; Session* session = fSessions.ItemAt(i); i++) {
		if (!session->stopped)
			active++;
	}
	atomic_set(&fActiveSessions, active);
}


//!	Starts the indicator if it is not running. Called with fLock held.
void
ScreenCaptureManager::_EnsureIndicator()
{
	team_info info;
	if (fIndicatorTeam >= 0 && get_team_info(fIndicatorTeam, &info) == B_OK)
		return;
	fIndicatorTeam = _LaunchAgent("--indicator", NULL);
}


team_id
ScreenCaptureManager::_LaunchAgent(const char* mode, const char* argument)
{
	const char* args[4] = { SCREEN_CAPTURE_AGENT_PATH, mode, argument, NULL };
	int32 argc = argument != NULL ? 3 : 2;
	thread_id thread = load_image(argc, args, (const char**)environ);
	if (thread < 0)
		return thread;
	thread_info info;
	team_id team = get_thread_info(thread, &info) == B_OK ? info.team : -1;
	resume_thread(thread);
	return team;
}
