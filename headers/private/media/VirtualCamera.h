/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VIRTUAL_CAMERA_H
#define _VIRTUAL_CAMERA_H


/*!	The virtual camera (Renga, roadmap F-58; OBS Studio's request O-12): a
	Media Kit video source, "Virtual Camera", that one program feeds and
	every other program sees as an ordinary camera (CodyCam's Camera menu,
	the Media preferences' video input, Chromium, ...). It is a global
	flavor of the virtual_camera media add-on: B_BUFFER_PRODUCER |
	B_CONTROLLABLE | B_PHYSICAL_INPUT, raw video in B_RGB32 at the size and
	field_rate its consumer connects with (the fed frames are scaled to it;
	640x480 at 30 fps if the consumer leaves them open). Without a feeder it
	sends a dark grey "no signal" frame.

	Feeding: the node listens on the local SOCK_SEQPACKET socket
	VIRTUAL_CAMERA_SOCKET. A feeder sends a virtual_camera_configure message
	with a memfd (SCM_RIGHTS) holding a virtual_camera_memory header and
	slot_count frames of B_RGB32 pixels, then one virtual_camera_frame
	message per frame it has written into a slot. One feeder at a time: a
	second one gets B_BUSY. Closing the socket ends the feed.
	VirtualCameraFeeder below does all of it (header-only; link with -lgnu
	for memfd_create(), and libnetwork).

	F-58 batch 2 (OBS Studio's O-20): WriteFrame() with a timeout never
	waits longer than asked if the node stops reading (media_addon_server
	busy, stopped or hung): B_WOULD_BLOCK at once with a timeout of 0,
	B_TIMED_OUT after the timeout otherwise; the frame is dropped and the
	feed stays connected. Disconnect() never waits for the node.

	A slot's sequence is odd while the feeder writes it; the node copies a
	slot whose sequence is even and the same before and after the copy.
*/


#include <OS.h>
#include <SupportDefs.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


#define VIRTUAL_CAMERA_FLAVOR_NAME	"Virtual Camera"
#define VIRTUAL_CAMERA_SOCKET_DIRECTORY	"/boot/system/var/run/media"
#define VIRTUAL_CAMERA_SOCKET		VIRTUAL_CAMERA_SOCKET_DIRECTORY \
	"/virtual_camera"
// The node's hidden text parameter: "<feeder team> <width>x<height> <frames
// received> <frames sent>" (five values: the feeder's team, the fed size,
// the frames received from the feeder and the buffers the node sent to its
// consumer), or "none" without a feeder.
#define VIRTUAL_CAMERA_STATUS_KIND	"renga:virtual_camera"

#define VIRTUAL_CAMERA_MAGIC		'RVcm'
#define VIRTUAL_CAMERA_CONFIGURE	'RVcC'
#define VIRTUAL_CAMERA_FRAME		'RVcF'
#define VIRTUAL_CAMERA_VERSION		1
#define VIRTUAL_CAMERA_MAX_SLOTS	8
#define VIRTUAL_CAMERA_MAX_SIZE		8192
#define VIRTUAL_CAMERA_PIXELS_OFFSET	4096
	// the first slot's pixels in the memfd; slot n at
	// VIRTUAL_CAMERA_PIXELS_OFFSET + n * height * bytes_per_row


namespace BPrivate {


struct virtual_camera_slot {
	int32		sequence;		// odd while being written
	uint32		reserved;
	bigtime_t	time;			// system_time() the frame belongs to
};


// At the start of the memfd.
struct virtual_camera_memory {
	uint32		magic;			// VIRTUAL_CAMERA_MAGIC
	uint32		version;
	uint32		width;
	uint32		height;
	uint32		bytes_per_row;	// at least width * 4, B_RGB32
	uint32		slot_count;
	virtual_camera_slot slots[VIRTUAL_CAMERA_MAX_SLOTS];
};


// Feeder to node, with the memfd.
struct virtual_camera_configure {
	uint32		what;			// VIRTUAL_CAMERA_CONFIGURE
	uint32		version;
	uint32		width;
	uint32		height;
	uint32		bytes_per_row;
	uint32		slot_count;
	float		field_rate;		// the feeder's rate (informational)
	uint32		reserved;
};

// Node to feeder, the answer to virtual_camera_configure.
struct virtual_camera_reply {
	int32		status;			// B_OK, B_BUSY, B_BAD_VALUE, ...
	uint32		reserved;
};

// Feeder to node: a slot holds a new frame.
struct virtual_camera_frame {
	uint32		what;			// VIRTUAL_CAMERA_FRAME
	uint32		slot;
	bigtime_t	time;
};


/*!	Feeds the virtual camera. Not thread safe: one thread at a time. */
class VirtualCameraFeeder {
public:
	VirtualCameraFeeder()
		:
		fSocket(-1),
		fMemory(-1),
		fMapped(NULL),
		fSize(0),
		fNext(0)
	{
	}

	~VirtualCameraFeeder()
	{
		Disconnect();
	}

	//!	B_BUSY if another program feeds the camera, B_NO_INIT if the media
	//!	services (the node) do not run.
	status_t Connect(uint32 width, uint32 height, float fieldRate,
		uint32 slotCount = 3)
	{
		return ConnectTo(VIRTUAL_CAMERA_SOCKET, width, height, fieldRate,
			slotCount);
	}

	/*!	Connect() to the node listening on \a socketPath instead of the
		system's virtual camera (F-58 batch 2: the tests use it with a node
		of their own that stops reading).
	*/
	status_t ConnectTo(const char* socketPath, uint32 width, uint32 height,
		float fieldRate, uint32 slotCount = 3)
	{
		Disconnect();
		if (socketPath == NULL)
			return B_BAD_VALUE;
		if (width == 0 || height == 0 || width > VIRTUAL_CAMERA_MAX_SIZE
			|| height > VIRTUAL_CAMERA_MAX_SIZE || slotCount < 2
			|| slotCount > VIRTUAL_CAMERA_MAX_SLOTS)
			return B_BAD_VALUE;

		uint32 bytesPerRow = width * 4;
		fSize = VIRTUAL_CAMERA_PIXELS_OFFSET
			+ (size_t)slotCount * height * bytesPerRow;
		fMemory = memfd_create("virtual camera", 0);
		if (fMemory < 0 || ftruncate(fMemory, fSize) != 0)
			return _Fail(errno);
		fMapped = (uint8*)mmap(NULL, fSize, PROT_READ | PROT_WRITE,
			MAP_SHARED, fMemory, 0);
		if (fMapped == MAP_FAILED) {
			fMapped = NULL;
			return _Fail(errno);
		}
		virtual_camera_memory* header = (virtual_camera_memory*)fMapped;
		header->magic = VIRTUAL_CAMERA_MAGIC;
		header->version = VIRTUAL_CAMERA_VERSION;
		header->width = width;
		header->height = height;
		header->bytes_per_row = bytesPerRow;
		header->slot_count = slotCount;

		fSocket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		if (fSocket < 0)
			return _Fail(errno);
		fcntl(fSocket, F_SETFD, FD_CLOEXEC);
		sockaddr_un address = {};
		address.sun_family = AF_UNIX;
		strlcpy(address.sun_path, socketPath, sizeof(address.sun_path));
		if (connect(fSocket, (sockaddr*)&address, sizeof(address)) != 0)
			return _Fail(errno == ENOENT || errno == ECONNREFUSED
				? B_NO_INIT : errno);

		virtual_camera_configure configure = {};
		configure.what = VIRTUAL_CAMERA_CONFIGURE;
		configure.version = VIRTUAL_CAMERA_VERSION;
		configure.width = width;
		configure.height = height;
		configure.bytes_per_row = bytesPerRow;
		configure.slot_count = slotCount;
		configure.field_rate = fieldRate;
		char control[CMSG_SPACE(sizeof(int))];
		memset(control, 0, sizeof(control));
		iovec iov = { &configure, sizeof(configure) };
		msghdr message = {};
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fMemory, sizeof(int));
		if (sendmsg(fSocket, &message, 0) != (ssize_t)sizeof(configure))
			return _Fail(errno);
		virtual_camera_reply reply;
		if (recv(fSocket, &reply, sizeof(reply), 0) != (ssize_t)sizeof(reply))
			return _Fail(B_ERROR);
		if (reply.status != B_OK)
			return _Fail(reply.status);
		fWidth = width;
		fHeight = height;
		fBytesPerRow = bytesPerRow;
		fSlotCount = slotCount;
		return B_OK;
	}

	bool IsConnected() const
	{
		return fSocket >= 0;
	}

	/*!	Ends the feed. Never waits for the node (F-58 batch 2, O-20): the
		socket is shut down before it is closed, which also wakes a
		WriteFrame() blocked in another thread (the class is not thread
		safe otherwise: do not call it while another thread is inside
		WriteFrame() copying a frame).
	*/
	void Disconnect()
	{
		if (fSocket >= 0) {
			shutdown(fSocket, SHUT_RDWR);
			close(fSocket);
		}
		fSocket = -1;
		if (fMapped != NULL)
			munmap(fMapped, fSize);
		fMapped = NULL;
		if (fMemory >= 0)
			close(fMemory);
		fMemory = -1;
	}

	/*!	Copies a B_RGB32 frame of the configured size (\a bytesPerRow per
		row) into the next slot and announces it. Waits as long as it takes
		if the node is not reading; see the overload with a timeout.
	*/
	status_t WriteFrame(const void* bits, uint32 bytesPerRow, bigtime_t time)
	{
		return WriteFrame(bits, bytesPerRow, time, B_INFINITE_TIMEOUT);
	}

	/*!	WriteFrame() that waits at most \a timeout (F-58 batch 2, OBS
		Studio's O-20) for room to announce the frame: 0 does not wait
		(B_WOULD_BLOCK if the node has not taken the previous frames yet),
		B_INFINITE_TIMEOUT waits as long as it takes (as the other
		overload), anything else returns B_TIMED_OUT once it is up. The
		check comes first, so a frame that cannot be announced is not
		copied and costs nothing; it is dropped, the feed stays connected
		and the next WriteFrame() tries again. Other errors (the node went
		away) disconnect, as before.
	*/
	status_t WriteFrame(const void* bits, uint32 bytesPerRow, bigtime_t time,
		bigtime_t timeout)
	{
		if (fSocket < 0 || fMapped == NULL)
			return B_NO_INIT;
		bigtime_t deadline = timeout == B_INFINITE_TIMEOUT
			? B_INFINITE_TIMEOUT
			: system_time() + (timeout > 0 ? timeout : 0);
		status_t ready = _WaitWritable(timeout);
		if (ready != B_OK) {
			if (ready != B_WOULD_BLOCK && ready != B_TIMED_OUT)
				Disconnect();
			return ready;
		}
		uint32 slot = fNext;
		fNext = (fNext + 1) % fSlotCount;
		virtual_camera_memory* header = (virtual_camera_memory*)fMapped;
		virtual_camera_slot& info = header->slots[slot];
		atomic_add(&info.sequence, 1);
		uint8* destination = fMapped + VIRTUAL_CAMERA_PIXELS_OFFSET
			+ (size_t)slot * fHeight * fBytesPerRow;
		for (uint32 y = 0; y < fHeight; y++) {
			memcpy(destination + y * fBytesPerRow,
				(const uint8*)bits + y * bytesPerRow, fWidth * 4);
		}
		info.time = time;
		atomic_add(&info.sequence, 1);

		virtual_camera_frame frame = { VIRTUAL_CAMERA_FRAME, slot, time };
		int flags = timeout == B_INFINITE_TIMEOUT ? 0 : MSG_DONTWAIT;
		while (send(fSocket, &frame, sizeof(frame), flags)
				!= (ssize_t)sizeof(frame)) {
			status_t status = errno;
			if (status == EINTR)
				continue;
			if (status != EAGAIN && status != EWOULDBLOCK) {
				Disconnect();
				return status;
			}
			// poll() saw some room, but not enough for the message (or it
			// was taken meanwhile): try again until the time is up
			bigtime_t left = deadline - system_time();
			if (timeout == 0)
				return B_WOULD_BLOCK;
			if (left <= 0)
				return B_TIMED_OUT;
			snooze(min_c(left, (bigtime_t)2000));
		}
		return B_OK;
	}

private:
	//!	Waits until a frame message can be sent, at most \a timeout.
	status_t _WaitWritable(bigtime_t timeout)
	{
		if (timeout == B_INFINITE_TIMEOUT)
			return B_OK;
		bigtime_t deadline = system_time() + (timeout > 0 ? timeout : 0);
		while (true) {
			bigtime_t left = deadline - system_time();
			// poll() counts in milliseconds; round up so that a short
			// timeout still waits
			int milliseconds = left <= 0 ? 0
				: (int)min_c((left + 999) / 1000, (bigtime_t)0x7fffffff);
			pollfd entry = { fSocket, POLLOUT, 0 };
			int result = poll(&entry, 1, milliseconds);
			if (result > 0) {
				if ((entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
					return B_ERROR;
				if ((entry.revents & POLLOUT) != 0)
					return B_OK;
			} else if (result < 0 && errno != EINTR)
				return errno;
			if (system_time() >= deadline)
				return timeout <= 0 ? B_WOULD_BLOCK : B_TIMED_OUT;
		}
	}

	status_t _Fail(status_t status)
	{
		Disconnect();
		return status;
	}

	int			fSocket;
	int			fMemory;
	uint8*		fMapped;
	size_t		fSize;
	uint32		fNext;
	uint32		fWidth;
	uint32		fHeight;
	uint32		fBytesPerRow;
	uint32		fSlotCount;
};


}	// namespace BPrivate


#endif	// _VIRTUAL_CAMERA_H
