/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include <ScreenCapture.h>

#include <Bitmap.h>
#include <Message.h>

#include <AppServerLink.h>
#include <BitmapPrivate.h>
#include <ServerProtocol.h>


namespace BPrivate {


BScreenCapture::BScreenCapture()
	:
	fSession(-1)
{
}


BScreenCapture::~BScreenCapture()
{
	Stop();
}


status_t
BScreenCapture::StartScreen(uint32 flags, bigtime_t permissionTimeout)
{
	return _Start(SCREEN_CAPTURE_SCREEN, -1, BRect(), flags,
		permissionTimeout);
}


status_t
BScreenCapture::StartWindow(int32 windowServerToken, uint32 flags,
	bigtime_t permissionTimeout)
{
	return _Start(SCREEN_CAPTURE_WINDOW, windowServerToken, BRect(), flags,
		permissionTimeout);
}


status_t
BScreenCapture::StartRegion(BRect region, uint32 flags,
	bigtime_t permissionTimeout)
{
	return _Start(SCREEN_CAPTURE_REGION, -1, region, flags,
		permissionTimeout);
}


status_t
BScreenCapture::_Start(int32 source, int32 windowToken, BRect region,
	uint32 flags, bigtime_t permissionTimeout)
{
	Stop();

	// While the user has not answered, app_server replies B_WOULD_BLOCK;
	// ask again until there is an answer or the time is up.
	bigtime_t deadline = system_time() + permissionTimeout;
	while (true) {
		AppServerLink link;
		link.StartMessage(AS_SCREEN_CAPTURE_START);
		link.Attach<int32>(source);
		link.Attach<int32>(windowToken);
		link.Attach<BRect>(region);
		link.Attach<uint32>(flags);
		status_t status;
		if (link.FlushWithReply(status) != B_OK)
			return B_ERROR;
		if (status == B_OK) {
			link.Read<int32>(&fSession);
			link.Read<BRect>(&fFrame);
			return B_OK;
		}
		if (status != B_WOULD_BLOCK)
			return status;
		if (system_time() >= deadline)
			return B_NOT_ALLOWED;
		snooze(100000);
	}
}


status_t
BScreenCapture::Stop()
{
	if (fSession < 0)
		return B_OK;
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_STOP);
	link.Attach<int32>(fSession);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		status = B_ERROR;
	fSession = -1;
	return status;
}


status_t
BScreenCapture::NextFrame(BBitmap* bitmap, bool* _changed, BRegion* _damage,
	bigtime_t* _time)
{
	if (fSession < 0)
		return B_NO_INIT;
	if (bitmap == NULL || !bitmap->IsValid())
		return B_BAD_VALUE;
	int32 token = BBitmap::Private(bitmap).ServerToken();
	if (token < 0)
		return B_BAD_VALUE;

	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_FRAME);
	link.Attach<int32>(fSession);
	link.Attach<int32>(token);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;

	bool changed;
	bigtime_t time;
	int32 count;
	link.Read<bool>(&changed);
	link.Read<BRect>(&fFrame);
	link.Read<bigtime_t>(&time);
	link.Read<int32>(&count);
	if (_damage != NULL)
		_damage->MakeEmpty();
	for (int32 i = 0; i < count; i++) {
		BRect rect;
		if (link.Read<BRect>(&rect) != B_OK)
			break;
		if (_damage != NULL)
			_damage->Include(rect);
	}
	if (_changed != NULL)
		*_changed = changed;
	if (_time != NULL)
		*_time = time;
	return B_OK;
}


/*static*/ status_t
BScreenCapture::GetSessions(BMessage* sessions)
{
	if (sessions == NULL)
		return B_BAD_VALUE;
	sessions->MakeEmpty();
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_LIST);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;
	int32 count = 0;
	link.Read<int32>(&count);
	for (int32 i = 0; i < count; i++) {
		int32 id;
		team_id team;
		BString signature;
		BString name;
		int32 source;
		BRect frame;
		bigtime_t started;
		int64 frames;
		link.Read<int32>(&id);
		link.Read<team_id>(&team);
		link.ReadString(signature);
		link.ReadString(name);
		link.Read<int32>(&source);
		link.Read<BRect>(&frame);
		link.Read<bigtime_t>(&started);
		if (link.Read<int64>(&frames) != B_OK)
			return B_ERROR;
		sessions->AddInt32("id", id);
		sessions->AddInt32("team", team);
		sessions->AddString("signature", signature);
		sessions->AddString("name", name);
		sessions->AddInt32("source", source);
		sessions->AddRect("frame", frame);
		sessions->AddInt64("started", started);
		sessions->AddInt64("frames", frames);
	}
	return B_OK;
}


/*static*/ status_t
BScreenCapture::StopSession(int32 id)
{
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_STOP_ANY);
	link.Attach<int32>(id);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	return status;
}


/*static*/ status_t
BScreenCapture::GetPermissions(BMessage* permissions)
{
	if (permissions == NULL)
		return B_BAD_VALUE;
	permissions->MakeEmpty();
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_GET_PERMISSIONS);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;
	int32 count = 0;
	link.Read<int32>(&count);
	for (int32 i = 0; i < count; i++) {
		BString signature;
		BString name;
		int32 state;
		link.ReadString(signature);
		link.ReadString(name);
		if (link.Read<int32>(&state) != B_OK)
			return B_ERROR;
		permissions->AddString("signature", signature);
		permissions->AddString("name", name);
		permissions->AddInt32("state", state);
	}
	return B_OK;
}


/*static*/ status_t
BScreenCapture::SetPermission(const char* signature, int32 state)
{
	if (signature == NULL)
		return B_BAD_VALUE;
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_SET_PERMISSION);
	link.AttachString(signature);
	link.Attach<int32>(state);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	return status;
}


/*static*/ status_t
BScreenCapture::GetRequest(int32 request, BString& signature, BString& name,
	BString& path, int32& source)
{
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_GET_REQUEST);
	link.Attach<int32>(request);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;
	link.ReadString(signature);
	link.ReadString(name);
	link.ReadString(path);
	return link.Read<int32>(&source);
}


/*static*/ status_t
BScreenCapture::AnswerRequest(int32 request, bool allow, bool remember)
{
	AppServerLink link;
	link.StartMessage(AS_SCREEN_CAPTURE_ANSWER_REQUEST);
	link.Attach<int32>(request);
	link.Attach<bool>(allow);
	link.Attach<bool>(remember);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	return status;
}


}	// namespace BPrivate
