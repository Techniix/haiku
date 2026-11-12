/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "VirtualCameraNode.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaAddOn.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include <VirtualCamera.h>


using namespace BPrivate;


static const uint32 kDefaultWidth = 640;
static const uint32 kDefaultHeight = 480;
static const float kDefaultRate = 30;
static const uint32 kNoSignal = 0xff303030;


/*!	The feed: one per media_addon_server, shared by every node of the
	flavor (the media server makes one as the default video input, and
	applications may make more), so that all of them show what the one
	feeder sends. Started with the first node, stopped with the last.
*/
struct VirtualCameraFeed {
	BLocker			lock;
	int32			users;
	int				listener;
	thread_id		thread;
	volatile bool	quitting;
	team_id			feeder;
	const uint8*	memory;
	size_t			memorySize;
	uint32			width;
	uint32			height;
	uint32			bytesPerRow;
	uint32			slots;
	int32			latestSlot;
	int64			framesReceived;

	VirtualCameraFeed()
		:
		lock("virtual camera feed"),
		users(0),
		listener(-1),
		thread(-1),
		quitting(false),
		feeder(-1),
		memory(NULL),
		memorySize(0),
		width(0),
		height(0),
		bytesPerRow(0),
		slots(0),
		latestSlot(-1),
		framesReceived(0)
	{
	}
};

static VirtualCameraFeed sFeed;


VirtualCameraNode::VirtualCameraNode(BMediaAddOn* addOn, const char* name,
	int32 internalID)
	:
	BMediaNode(name),
	BBufferProducer(B_MEDIA_RAW_VIDEO),
	BMediaEventLooper(),
	BControllable(),
	fAddOn(addOn),
	fInternalID(internalID),
	fLock("virtual camera"),
	fConnected(false),
	fEnabled(true),
	fRunning(false),
	fBufferGroup(NULL),
	fFrameThread(-1),
	fFrameSync(-1),
	fQuitting(false),
	fStartTime(0),
	fFrame(0),
	fFramesSent(0)
{
	fFormat = media_raw_video_format::wildcard;
	fOutput.node = media_node::null;
	fOutput.source = media_source::null;
	fOutput.destination = media_destination::null;
	fOutput.format.type = B_MEDIA_RAW_VIDEO;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;
	fOutput.format.u.raw_video.interlace = 1;
	fOutput.format.u.raw_video.display.format = B_RGB32;
	strlcpy(fOutput.name, "Virtual camera", sizeof(fOutput.name));
}


VirtualCameraNode::~VirtualCameraNode()
{
	fQuitting = true;
	if (fFrameSync >= 0)
		delete_sem(fFrameSync);
	status_t result;
	if (fFrameThread >= 0)
		wait_for_thread(fFrameThread, &result);
	_ReleaseFeed();
	Quit();
	delete fBufferGroup;
}


BMediaAddOn*
VirtualCameraNode::AddOn(int32* _internalID) const
{
	if (_internalID != NULL)
		*_internalID = fInternalID;
	return fAddOn;
}


status_t
VirtualCameraNode::HandleMessage(int32 message, const void* data, size_t size)
{
	if (BControllable::HandleMessage(message, data, size) == B_OK
		|| BBufferProducer::HandleMessage(message, data, size) == B_OK
		|| BMediaNode::HandleMessage(message, data, size) == B_OK)
		return B_OK;
	HandleBadMessage(message, data, size);
	return B_ERROR;
}


void
VirtualCameraNode::NodeRegistered()
{
	BParameterWeb* web = new BParameterWeb();
	BParameterGroup* group = web->MakeGroup(Name());
	group->MakeTextParameter(kStatusParameter, B_MEDIA_RAW_VIDEO,
		"Feeder", VIRTUAL_CAMERA_STATUS_KIND, 128)
			->SetFlags(B_HIDDEN_PARAMETER);
	group->MakeTextParameter(kModesParameter, B_MEDIA_RAW_VIDEO,
		"Video modes", "renga:video_modes", 256)
			->SetFlags(B_HIDDEN_PARAMETER);
	SetParameterWeb(web);

	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;

	fFrameSync = create_sem(0, "virtual camera frames");
	fFrameThread = spawn_thread(_FrameThreadEntry, "virtual camera frames",
		B_DISPLAY_PRIORITY, this);
	resume_thread(fFrameThread);

	_AcquireFeed();

	Run();
}


/*!	The first node starts the feed's socket; later ones share it. */
/*static*/ void
VirtualCameraNode::_AcquireFeed()
{
	BAutolock locker(sFeed.lock);
	if (sFeed.users++ > 0)
		return;
	sFeed.quitting = false;
	// /boot/system/var/run may not exist yet
	mkdir("/boot/system/var/run", 0755);
	mkdir(VIRTUAL_CAMERA_SOCKET_DIRECTORY, 0755);
	unlink(VIRTUAL_CAMERA_SOCKET);
	sFeed.listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (sFeed.listener < 0)
		return;
	fcntl(sFeed.listener, F_SETFD, FD_CLOEXEC);
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	strlcpy(address.sun_path, VIRTUAL_CAMERA_SOCKET,
		sizeof(address.sun_path));
	if (bind(sFeed.listener, (sockaddr*)&address, sizeof(address)) == 0
		&& listen(sFeed.listener, 2) == 0) {
		chmod(VIRTUAL_CAMERA_SOCKET, 0666);
		sFeed.thread = spawn_thread(_ServerThreadEntry,
			"virtual camera feed", B_NORMAL_PRIORITY, NULL);
		resume_thread(sFeed.thread);
	} else {
		fprintf(stderr, "virtual camera: cannot listen on %s: %s\n",
			VIRTUAL_CAMERA_SOCKET, strerror(errno));
		close(sFeed.listener);
		sFeed.listener = -1;
	}
}


//!	The last node stops the feed.
/*static*/ void
VirtualCameraNode::_ReleaseFeed()
{
	thread_id thread = -1;
	{
		BAutolock locker(sFeed.lock);
		if (--sFeed.users > 0)
			return;
		sFeed.quitting = true;
		thread = sFeed.thread;
		sFeed.thread = -1;
	}
	// The server thread polls with a timeout and sees "quitting".
	status_t result;
	if (thread >= 0)
		wait_for_thread(thread, &result);
	BAutolock locker(sFeed.lock);
	if (sFeed.listener >= 0) {
		close(sFeed.listener);
		sFeed.listener = -1;
		unlink(VIRTUAL_CAMERA_SOCKET);
	}
	_DropFeed();
}


// #pragma mark - BMediaEventLooper


void
VirtualCameraNode::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	switch (event->type) {
		case BTimedEventQueue::B_START:
		{
			BAutolock locker(fLock);
			if (!fRunning) {
				fRunning = true;
				fStartTime = event->event_time;
				fFrame = 0;
			}
			release_sem(fFrameSync);
			break;
		}
		case BTimedEventQueue::B_STOP:
		{
			BAutolock locker(fLock);
			fRunning = false;
			release_sem(fFrameSync);
			break;
		}
		case BTimedEventQueue::B_SEEK:
		case BTimedEventQueue::B_WARP:
		{
			BAutolock locker(fLock);
			fStartTime = event->bigdata;
			fFrame = 0;
			release_sem(fFrameSync);
			break;
		}
		default:
			break;
	}
}


// #pragma mark - BBufferProducer


void
VirtualCameraNode::_Specialize(media_format* format) const
{
	media_raw_video_format& raw = format->u.raw_video;
	format->type = B_MEDIA_RAW_VIDEO;
	raw.display.format = B_RGB32;
	if (raw.display.line_width == 0)
		raw.display.line_width = kDefaultWidth;
	if (raw.display.line_count == 0)
		raw.display.line_count = kDefaultHeight;
	raw.display.bytes_per_row = raw.display.line_width * 4;
	if (raw.field_rate <= 0)
		raw.field_rate = kDefaultRate;
	raw.interlace = 1;
	raw.first_active = 0;
	raw.last_active = raw.display.line_count - 1;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.pixel_width_aspect = 1;
	raw.pixel_height_aspect = 1;
}


status_t
VirtualCameraNode::FormatSuggestionRequested(media_type type, int32 quality,
	media_format* format)
{
	if (type != B_MEDIA_RAW_VIDEO && type != B_MEDIA_UNKNOWN_TYPE)
		return B_MEDIA_BAD_FORMAT;
	*format = fOutput.format;
	_Specialize(format);
	return B_OK;
}


status_t
VirtualCameraNode::FormatProposal(const media_source& output,
	media_format* format)
{
	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	if (format->type != B_MEDIA_RAW_VIDEO
		&& format->type != B_MEDIA_UNKNOWN_TYPE)
		return B_MEDIA_BAD_FORMAT;
	color_space space = format->u.raw_video.display.format;
	if (space != B_RGB32 && space != media_raw_video_format::wildcard
			.display.format)
		return B_MEDIA_BAD_FORMAT;
	uint32 width = format->u.raw_video.display.line_width;
	uint32 height = format->u.raw_video.display.line_count;
	if (width > VIRTUAL_CAMERA_MAX_SIZE || height > VIRTUAL_CAMERA_MAX_SIZE)
		return B_MEDIA_BAD_FORMAT;
	_Specialize(format);
	return B_OK;
}


status_t
VirtualCameraNode::FormatChangeRequested(const media_source& source,
	const media_destination& destination, media_format* format,
	int32* _deprecated)
{
	return B_ERROR;
}


status_t
VirtualCameraNode::GetNextOutput(int32* cookie, media_output* _output)
{
	if (*cookie != 0)
		return B_BAD_INDEX;
	BAutolock locker(fLock);
	*_output = fOutput;
	(*cookie)++;
	return B_OK;
}


status_t
VirtualCameraNode::DisposeOutputCookie(int32 cookie)
{
	return B_OK;
}


status_t
VirtualCameraNode::SetBufferGroup(const media_source& source,
	BBufferGroup* group)
{
	return B_ERROR;
}


status_t
VirtualCameraNode::GetLatency(bigtime_t* _latency)
{
	*_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}


status_t
VirtualCameraNode::PrepareToConnect(const media_source& what,
	const media_destination& where, media_format* format,
	media_source* _source, char* _name)
{
	BAutolock locker(fLock);
	if (fConnected)
		return EALREADY;
	if (what != fOutput.source)
		return B_MEDIA_BAD_SOURCE;
	status_t status = FormatProposal(what, format);
	if (status != B_OK)
		return status;
	fOutput.destination = where;
	*_source = fOutput.source;
	strlcpy(_name, fOutput.name, B_MEDIA_NAME_LENGTH);
	return B_OK;
}


void
VirtualCameraNode::Connect(status_t error, const media_source& source,
	const media_destination& destination, const media_format& format,
	char* ioName)
{
	BAutolock locker(fLock);
	if (error != B_OK || source != fOutput.source) {
		fOutput.destination = media_destination::null;
		return;
	}
	fOutput.destination = destination;
	fOutput.format = format;
	fFormat = format.u.raw_video;
	strlcpy(ioName, fOutput.name, B_MEDIA_NAME_LENGTH);

	bigtime_t latency = 0;
	media_node_id timeSource;
	FindLatencyFor(fOutput.destination, &latency, &timeSource);
	SetEventLatency(latency + 2000);

	delete fBufferGroup;
	fBufferGroup = new(std::nothrow) BBufferGroup(
		fFormat.display.bytes_per_row * fFormat.display.line_count, 4);
	if (fBufferGroup != NULL && fBufferGroup->InitCheck() != B_OK) {
		delete fBufferGroup;
		fBufferGroup = NULL;
	}
	fConnected = fBufferGroup != NULL;
	fEnabled = true;
	fFrame = 0;
	fStartTime = TimeSource()->Now();
	release_sem(fFrameSync);
}


void
VirtualCameraNode::Disconnect(const media_source& what,
	const media_destination& where)
{
	BAutolock locker(fLock);
	if (what != fOutput.source || where != fOutput.destination)
		return;
	fConnected = false;
	fOutput.destination = media_destination::null;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;
	fOutput.format.u.raw_video.display.format = B_RGB32;
	delete fBufferGroup;
	fBufferGroup = NULL;
}


void
VirtualCameraNode::LateNoticeReceived(const media_source& what,
	bigtime_t howMuch, bigtime_t performanceTime)
{
}


void
VirtualCameraNode::EnableOutput(const media_source& what, bool enabled,
	int32* _deprecated)
{
	BAutolock locker(fLock);
	if (what == fOutput.source)
		fEnabled = enabled;
}


// #pragma mark - BControllable


status_t
VirtualCameraNode::GetParameterValue(int32 id, bigtime_t* _lastChange,
	void* value, size_t* _size)
{
	char text[256];
	if (id == kStatusParameter) {
		BAutolock locker(sFeed.lock);
		if (sFeed.feeder < 0)
			strlcpy(text, "none", sizeof(text));
		else {
			snprintf(text, sizeof(text), "%" B_PRId32 " %" B_PRIu32 "x%"
				B_PRIu32 " %" B_PRId64 " %" B_PRId64, sFeed.feeder, sFeed.width,
				sFeed.height, sFeed.framesReceived, fFramesSent);
		}
	} else if (id == kModesParameter) {
		// Any size and rate; these are the usual ones.
		strlcpy(text, "640x480 RGB32 30,15\n1280x720 RGB32 30,15\n"
			"1920x1080 RGB32 30,15\n", sizeof(text));
	} else
		return B_BAD_VALUE;
	size_t length = strlen(text) + 1;
	if (*_size < length)
		return B_BAD_VALUE;
	memcpy(value, text, length);
	*_size = length;
	*_lastChange = 0;
	return B_OK;
}


void
VirtualCameraNode::SetParameterValue(int32 id, bigtime_t when,
	const void* value, size_t size)
{
	// read only
}


// #pragma mark - frames


/*static*/ status_t
VirtualCameraNode::_FrameThreadEntry(void* data)
{
	((VirtualCameraNode*)data)->_FrameThread();
	return B_OK;
}


/*!	One buffer per frame period of the connection: the latest frame of
	the feed, scaled (nearest neighbour), or "no signal".
*/
void
VirtualCameraNode::_FrameThread()
{
	bigtime_t wakeUp = B_INFINITE_TIMEOUT;
	while (!fQuitting) {
		status_t status = acquire_sem_etc(fFrameSync, 1, B_ABSOLUTE_TIMEOUT,
			wakeUp);
		if (status != B_OK && status != B_TIMED_OUT)
			break;

		BAutolock locker(fLock);
		if (!fRunning || !fConnected || fBufferGroup == NULL
			|| fFormat.field_rate <= 0) {
			wakeUp = B_INFINITE_TIMEOUT;
			continue;
		}
		bigtime_t period = (bigtime_t)(1000000 / fFormat.field_rate);
		bigtime_t performance = fStartTime + fFrame * period;
		bigtime_t real = TimeSource()->RealTimeFor(performance,
			EventLatency());
		if (status == B_OK || real > system_time() + 1000) {
			// woken for a change, or early: sleep until it is time
			wakeUp = real;
			continue;
		}
		fFrame++;
		wakeUp = TimeSource()->RealTimeFor(fStartTime + fFrame * period,
			EventLatency());
		if (real < system_time() - period) {
			// a frame late: skip it
			continue;
		}
		if (!fEnabled)
			continue;

		size_t size = fFormat.display.bytes_per_row
			* fFormat.display.line_count;
		BBuffer* buffer = fBufferGroup->RequestBuffer(size, 0);
		if (buffer == NULL)
			continue;
		bigtime_t frameTime = 0;
		_Fill((uint32*)buffer->Data(), fFormat.display.line_width,
			fFormat.display.line_count, &frameTime);
		media_header* header = buffer->Header();
		header->type = B_MEDIA_RAW_VIDEO;
		header->size_used = size;
		header->time_source = TimeSource()->ID();
		header->start_time = performance;
		header->u.raw_video.field_sequence = fFrame - 1;
		header->u.raw_video.field_number = 0;
		header->u.raw_video.first_active_line = 0;
		header->u.raw_video.line_count = fFormat.display.line_count;
		media_source source = fOutput.source;
		media_destination destination = fOutput.destination;
		// Not with fLock held: the consumer may be busy (quitting) and the
		// send wait, and Disconnect() needs the lock meanwhile.
		locker.Unlock();
		if (SendBuffer(buffer, source, destination) != B_OK)
			buffer->Recycle();
		else
			atomic_add64(&fFramesSent, 1);
	}
}


//!	With fLock held.
void
VirtualCameraNode::_Fill(uint32* destination, uint32 width, uint32 height,
	bigtime_t* _time)
{
	BAutolock feedLocker(sFeed.lock);
	const virtual_camera_memory* header
		= (const virtual_camera_memory*)sFeed.memory;
	int32 slot = sFeed.latestSlot;
	if (sFeed.memory == NULL || slot < 0 || (uint32)slot >= sFeed.slots) {
		for (size_t i = 0; i < (size_t)width * height; i++)
			destination[i] = kNoSignal;
		return;
	}
	const uint8* pixels = sFeed.memory + VIRTUAL_CAMERA_PIXELS_OFFSET
		+ (size_t)slot * sFeed.height * sFeed.bytesPerRow;
	for (int attempt = 0; attempt < 2; attempt++) {
		int32 before = header->slots[slot].sequence;
		for (uint32 y = 0; y < height; y++) {
			const uint32* row = (const uint32*)(pixels
				+ (size_t)(y * sFeed.height / height) * sFeed.bytesPerRow);
			uint32* out = destination + (size_t)y * width;
			if (width == sFeed.width)
				memcpy(out, row, width * 4);
			else {
				for (uint32 x = 0; x < width; x++)
					out[x] = row[x * sFeed.width / width];
			}
		}
		*_time = header->slots[slot].time;
		if ((before & 1) == 0 && before == header->slots[slot].sequence)
			break;
		// the feeder wrote it meanwhile: once more
	}
}


// #pragma mark - the feed


/*static*/ status_t
VirtualCameraNode::_ServerThreadEntry(void* data)
{
	_ServerThread();
	return B_OK;
}


/*static*/ void
VirtualCameraNode::_ServerThread()
{
	while (!sFeed.quitting && sFeed.listener >= 0) {
		pollfd entry = { sFeed.listener, POLLIN, 0 };
		if (poll(&entry, 1, 500) <= 0)
			continue;
		int client = accept(sFeed.listener, NULL, NULL);
		if (client < 0)
			continue;
		_ServeClient(client);
		close(client);
	}
}


/*!	One feeder, until it closes the socket. Others that connect meanwhile
	are answered B_BUSY by _ServeClient()'s poll on the listener.
*/
/*static*/ void
VirtualCameraNode::_ServeClient(int client)
{
	virtual_camera_configure configure;
	char control[CMSG_SPACE(sizeof(int))];
	memset(control, 0, sizeof(control));
	iovec iov = { &configure, sizeof(configure) };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	ssize_t received = recvmsg(client, &message, 0);
	int memory = -1;
	cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
	if (received > 0 && cmsg != NULL && cmsg->cmsg_level == SOL_SOCKET
		&& cmsg->cmsg_type == SCM_RIGHTS)
		memcpy(&memory, CMSG_DATA(cmsg), sizeof(int));

	virtual_camera_reply reply = { B_OK, 0 };
	size_t size = 0;
	if (received != (ssize_t)sizeof(configure)
		|| configure.what != VIRTUAL_CAMERA_CONFIGURE
		|| configure.version != VIRTUAL_CAMERA_VERSION || memory < 0
		|| configure.width == 0 || configure.height == 0
		|| configure.width > VIRTUAL_CAMERA_MAX_SIZE
		|| configure.height > VIRTUAL_CAMERA_MAX_SIZE
		|| configure.bytes_per_row < configure.width * 4
		|| configure.slot_count < 1
		|| configure.slot_count > VIRTUAL_CAMERA_MAX_SLOTS) {
		reply.status = B_BAD_VALUE;
	} else {
		size = VIRTUAL_CAMERA_PIXELS_OFFSET + (size_t)configure.slot_count
			* configure.height * configure.bytes_per_row;
		struct stat st;
		if (fstat(memory, &st) != 0 || (size_t)st.st_size < size)
			reply.status = B_BAD_VALUE;
	}
	const uint8* mapped = NULL;
	if (reply.status == B_OK) {
		mapped = (const uint8*)mmap(NULL, size, PROT_READ, MAP_SHARED,
			memory, 0);
		if (mapped == MAP_FAILED) {
			mapped = NULL;
			reply.status = B_NO_MEMORY;
		} else if (((const virtual_camera_memory*)mapped)->magic
				!= VIRTUAL_CAMERA_MAGIC) {
			munmap((void*)mapped, size);
			mapped = NULL;
			reply.status = B_BAD_DATA;
		}
	}
	if (memory >= 0)
		close(memory);

	ucred credentials;
	socklen_t length = sizeof(credentials);
	team_id team = getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials,
		&length) == 0 ? (team_id)credentials.pid : 0;
	if (reply.status == B_OK) {
		BAutolock locker(sFeed.lock);
		sFeed.memory = mapped;
		sFeed.memorySize = size;
		sFeed.width = configure.width;
		sFeed.height = configure.height;
		sFeed.bytesPerRow = configure.bytes_per_row;
		sFeed.slots = configure.slot_count;
		sFeed.latestSlot = -1;
		sFeed.feeder = team;
		sFeed.framesReceived = 0;
	}
	send(client, &reply, sizeof(reply), 0);
	if (reply.status != B_OK)
		return;

	while (!sFeed.quitting) {
		pollfd entries[2] = { { client, POLLIN, 0 }, { sFeed.listener, POLLIN, 0 } };
		int result = poll(entries, 2, 500);
		if (result < 0 && errno != EINTR)
			break;
		if ((entries[1].revents & POLLIN) != 0) {
			// a second feeder: one at a time
			int other = accept(sFeed.listener, NULL, NULL);
			if (other >= 0) {
				virtual_camera_configure ignored;
				recv(other, &ignored, sizeof(ignored), MSG_DONTWAIT);
				virtual_camera_reply busy = { B_BUSY, 0 };
				send(other, &busy, sizeof(busy), 0);
				close(other);
			}
		}
		if ((entries[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
			continue;
		virtual_camera_frame frame;
		ssize_t bytes = recv(client, &frame, sizeof(frame), 0);
		if (bytes <= 0)
			break;
		if (bytes == (ssize_t)sizeof(frame)
			&& frame.what == VIRTUAL_CAMERA_FRAME
			&& frame.slot < configure.slot_count) {
			BAutolock locker(sFeed.lock);
			sFeed.latestSlot = frame.slot;
			sFeed.framesReceived++;
		}
	}
	_DropFeed();
}


/*static*/ void
VirtualCameraNode::_DropFeed()
{
	BAutolock locker(sFeed.lock);
	if (sFeed.memory != NULL)
		munmap((void*)sFeed.memory, sFeed.memorySize);
	sFeed.memory = NULL;
	sFeed.memorySize = 0;
	sFeed.latestSlot = -1;
	sFeed.feeder = -1;
}
