/*
 * Copyright 2024, Diego Roux, diegoroux04 at proton dot me
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Playback driver for the virtio sound device (virtio 1.2, section 5.14),
	exposed to the Media Kit through the hmulti ("multi_audio") interface.

	How it fits together
	--------------------
	The device has four virtqueues: control, event, TX (playback) and RX
	(capture). Configuration happens with synchronous request/response pairs
	on the control queue (PCM_INFO, SET_PARAMS, PREPARE, START, STOP, RELEASE).
	Audio is played by queueing "I/O messages" on the TX queue: a 4-byte
	virtio_snd_pcm_xfer header, one period of PCM data, and an 8-byte status
	that the device fills in. The device hands a message back once it has
	consumed that period, which is what paces playback - there is no
	position register and no timer in this driver.

	The multi_audio side works with a ring of N equally sized buffers that
	userland (the multi_audio media add-on) writes into directly.
	B_MULTI_BUFFER_EXCHANGE blocks until the device has finished with a
	buffer and reports a "buffer cycle"; the add-on then fills buffer
	(cycle - 1) mod N. So when TX message k completes we report cycle k + 1,
	the add-on refills buffer k, and on the next exchange we queue k again.
	In steady state N - 1 periods are queued at the device while the client
	fills the remaining one; N is therefore at least 3, so that the device
	never runs dry while the client works.

	Lifetime
	--------
	devfs calls init_device() on the first open and uninit_device() after
	the last close, and the media add-on opens and closes the device more
	than once while probing. The virtio transport (feature negotiation,
	virtqueues, interrupts) is therefore set up exactly once, on the first
	init_device(), and only torn down in uninit_driver(): the virtio bus
	manager does not support allocating queues a second time on a live
	device. uninit_device() only stops the stream and frees the buffers.

	Robustness
	----------
	The device is used by one client at a time (a second open() gets
	B_BUSY), since a second client could free the buffers the first one is
	writing to. TX request cookies carry a stream generation, so buffers the
	device returns after a stop are never confused with the current
	stream's. If the device stops answering - a control request or a drain
	after STOP/RELEASE times out - it may still own buffers we can no
	longer account for, so it is marked broken and not touched again until
	the driver is reloaded. Error paths of buffer_exchange() wait about one
	period before returning, because the media add-on calls it in a loop at
	real-time priority and ignores the result.

	Scope
	-----
	Playback only, stereo only. The first output stream that supports two
	channels is used. Capture (RX queue) is not implemented yet. The event
	queue is not populated, so jack and period events are not received
	(the spec only says a driver SHOULD populate it). Since
	the virtio control elements (VIRTIO_SND_F_CTLS) are not negotiated there
	is no hardware mixer; the Media Kit's software mixer handles volume.

	Rates and formats are deliberately limited to 44.1/48 kHz and 16-bit
	(falling back to 32-bit or float only if the device lacks 16-bit). The
	media add-on always picks the highest rate it is offered, and on a
	virtual device anything above 48 kHz only costs host and guest CPU.
*/


#include <ByteOrder.h>
#include <KernelExport.h>
#include <condition_variable.h>
#include <debug.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <AutoDeleter.h>
#include <virtio.h>

#include <hmulti_audio.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio_sound.h"


//#define TRACE_VIRTIO_SOUND
#ifdef TRACE_VIRTIO_SOUND
#	define TRACE(x...) dprintf("virtio_sound: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("virtio_sound: " x)
#define ERROR(x...)	dprintf("\33[33mvirtio_sound:\33[0m " x)


#define VIRTIO_SOUND_DRIVER_MODULE_NAME \
	"drivers/audio/hmulti/virtio_sound/driver_v1"
#define VIRTIO_SOUND_DEVICE_MODULE_NAME \
	"drivers/audio/hmulti/virtio_sound/device_v1"
#define VIRTIO_SOUND_DEVICE_ID_GENERATOR	"virtio_sound/device_id"


// Playback buffer ring limits (see the file comment for why N >= 3).
static const uint32 kMinBuffers = 3;
static const uint32 kMaxBuffers = 8;
static const uint32 kDefaultBuffers = 3;
static const uint32 kMinBufferFrames = 64;
static const uint32 kMaxBufferFrames = 16384;

static const uint32 kChannels = 2;

// How long we wait for the device before giving up on a request.
static const bigtime_t kControlTimeout = 1000000;
static const bigtime_t kExchangeTimeout = 2000000;
static const bigtime_t kDrainTimeout = 2000000;

// Layout of the one-page control buffer: request first, response after it.
static const size_t kControlRequestOffset = 0;
static const size_t kControlResponseOffset = 1024;
static const size_t kControlMaxResponse = B_PAGE_SIZE - kControlResponseOffset;

// Layout of the one-page TX header buffer: one slot per playback buffer,
// holding the xfer header at +0 and the device-written status at +8.
static const size_t kTxSlotSize = 16;
static const size_t kTxStatusOffset = 8;
static_assert(kMaxBuffers * kTxSlotSize <= B_PAGE_SIZE, "TX slots too big");

// multi_audio channel ids: two output channels plus the matching output bus.
static const int32 kChannelInfoCount = 4;

// TX request cookies carry the buffer index and the stream generation they
// were queued in, so that completions of an earlier stream (e.g. buffers the
// device returns late, after a stop) are never mistaken for current ones.
static inline void*
make_tx_cookie(int32 generation, uint32 index)
{
	return (void*)(addr_t)(((uint32)generation & 0xffffff) << 8 | (index + 1));
}


static inline void
parse_tx_cookie(void* cookie, int32& generation, uint32& index)
{
	uint32 value = (uint32)(addr_t)cookie;
	generation = value >> 8;
	index = (value & 0xff) - 1;
}


struct VirtIOSoundDriverInfo {
	device_node*				node;

	// Virtio transport, set up once on first open (see file comment).
	::virtio_device				virtioDevice;
	virtio_device_interface*	virtio;
	uint64						features;
	bool						transportReady;
	bool						transportFailed;
	::virtio_queue				queues[VIRTIO_SND_VQ_MAX];
	virtio_snd_config			config;

	// Control queue: one synchronous request at a time.
	mutex						controlLock;
	ConditionVariable			controlCondition;
	int32						controlRequestId;
	int32						controlCompletedId;
	area_id						controlArea;
	uint8*						controlBuffer;
	phys_addr_t					controlPhysical;

	// The output stream we drive, as reported by VIRTIO_SND_R_PCM_INFO.
	int32						outputStream;		// -1: none usable
	uint64						deviceFormats;		// VIRTIO_SND_PCM_FMT_* bits
	uint64						deviceRates;		// VIRTIO_SND_PCM_RATE_* bits
	uint32						outputFormats;		// B_FMT_* we advertise
	uint32						outputRates;		// B_SR_* we advertise

	// Playback state, protected by "lock".
	mutex						lock;
	mutex						exchangeLock;		// one exchanger at a time
	uint32						format;				// current B_FMT_*
	uint32						rate;				// current B_SR_*
	uint32						bufferFrames;
	uint32						bufferCount;
	uint32						periodBytes;
	area_id						dataArea;
	uint8*						data;				// user-accessible mapping
	phys_addr_t					dataPhysical;
	area_id						txHeaderArea;
	uint8*						txHeaders;
	phys_addr_t					txHeadersPhysical;
	bool						prepared;			// SET_PARAMS + PREPARE done
	bool						running;			// START done
	int32						fillIndex;			// buffer owned by client
	int32						generation;			// bumped on every stop

	// Set once the device stopped answering (a control request or a drain
	// timed out). From then on it is not used any more, because it may still
	// own buffers we can no longer track; reloading the driver resets it.
	bool						broken;
	bool						controlFailed;

	// Exclusive access: one client at a time, like the HDA driver.
	int32						openCount;

	// Completion bookkeeping, written from the TX interrupt callback and
	// protected by txLock.
	spinlock					txLock;
	struct {
		uint8					index;
		bool					error;
		int64					frames;		// played frames after this buffer
		bigtime_t				time;		// when it completed
	}							completed[kMaxBuffers];
	uint32						completedHead;
	uint32						completedCount;
	int32						inFlight;
	int64						framesPlayed;
	int32						txErrors;
	sem_id						txSem;
};


struct VirtIOSoundHandle {
	VirtIOSoundDriverInfo*		info;
};


static device_manager_info* sDeviceManager;


// #pragma mark - format and rate tables


struct rate_mapping {
	uint32	haikuRate;		// B_SR_*
	uint8	virtioRate;		// VIRTIO_SND_PCM_RATE_*
	uint32	hertz;
};

// Only the rates we are willing to advertise (see file comment).
static const rate_mapping kRates[] = {
	{ B_SR_44100, VIRTIO_SND_PCM_RATE_44100, 44100 },
	{ B_SR_48000, VIRTIO_SND_PCM_RATE_48000, 48000 },
};


struct format_mapping {
	uint32	haikuFormat;	// B_FMT_*
	uint8	virtioFormat;	// VIRTIO_SND_PCM_FMT_*
	uint32	sampleSize;		// bytes per sample
};

// In order of preference.
static const format_mapping kFormats[] = {
	{ B_FMT_16BIT, VIRTIO_SND_PCM_FMT_S16, 2 },
	{ B_FMT_32BIT, VIRTIO_SND_PCM_FMT_S32, 4 },
	{ B_FMT_FLOAT, VIRTIO_SND_PCM_FMT_FLOAT, 4 },
};


static const rate_mapping*
find_rate(uint32 haikuRate)
{
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].haikuRate == haikuRate)
			return &kRates[i];
	}
	return NULL;
}


static const format_mapping*
find_format(uint32 haikuFormat)
{
	for (size_t i = 0; i < B_COUNT_OF(kFormats); i++) {
		if (kFormats[i].haikuFormat == haikuFormat)
			return &kFormats[i];
	}
	return NULL;
}


// #pragma mark - control queue


static status_t
virtio_status_to_error(uint32 code)
{
	switch (code) {
		case VIRTIO_SND_S_OK:
			return B_OK;
		case VIRTIO_SND_S_BAD_MSG:
			return B_BAD_VALUE;
		case VIRTIO_SND_S_NOT_SUPP:
			return B_NOT_SUPPORTED;
		case VIRTIO_SND_S_IO_ERR:
			return B_IO_ERROR;
	}
	return B_ERROR;
}


//!	Control queue interrupt callback. Runs in interrupt context.
static void
control_callback(void* driverCookie, void* cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)cookie;

	void* requestCookie;
	while (info->virtio->queue_dequeue(info->queues[VIRTIO_SND_VQ_CONTROL],
			&requestCookie, NULL)) {
		atomic_set(&info->controlCompletedId, (int32)(addr_t)requestCookie);
		info->controlCondition.NotifyAll();
	}
}


/*!	Sends one request on the control queue and waits for the answer.
	\a response must start with a virtio_snd_hdr; its code is translated into
	the return value. Must be called from thread context.
*/
static status_t
control_request(VirtIOSoundDriverInfo* info, const void* request,
	size_t requestSize, void* response, size_t responseSize)
{
	if (requestSize > kControlResponseOffset - kControlRequestOffset
		|| responseSize > kControlMaxResponse
		|| responseSize < sizeof(virtio_snd_hdr)) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(info->controlLock);

	if (info->controlFailed)
		return B_IO_ERROR;

	uint8* requestBuffer = info->controlBuffer + kControlRequestOffset;
	uint8* responseBuffer = info->controlBuffer + kControlResponseOffset;
	memcpy(requestBuffer, request, requestSize);
	memset(responseBuffer, 0, responseSize);

	physical_entry entries[2];
	entries[0].address = info->controlPhysical + kControlRequestOffset;
	entries[0].size = requestSize;
	entries[1].address = info->controlPhysical + kControlResponseOffset;
	entries[1].size = responseSize;

	// Request ids start at 1, so a NULL cookie never matches a real request.
	int32 id = info->controlRequestId + 1;
	if (id <= 0)
		id = 1;
	info->controlRequestId = id;

	// Register for the wakeup before queueing, so that a completion that
	// arrives before we start waiting is not lost.
	ConditionVariableEntry entry;
	info->controlCondition.Add(&entry);

	status_t status = info->virtio->queue_request_v(
		info->queues[VIRTIO_SND_VQ_CONTROL], entries, 1, 1,
		(void*)(addr_t)id);
	if (status != B_OK) {
		ERROR("queueing control request failed: %s\n", strerror(status));
		return status;
	}

	// The condition is shared by all requests; keep waiting until it is
	// really ours that completed.
	bigtime_t deadline = system_time() + kControlTimeout;
	while (atomic_get(&info->controlCompletedId) != id) {
		status = entry.Wait(B_ABSOLUTE_TIMEOUT, deadline);
		if (atomic_get(&info->controlCompletedId) == id)
			break;
		if (status != B_OK) {
			// The device still owns this request's buffers and may yet
			// execute it. Reusing the buffer for the next request could make
			// it run that one twice, so the control queue is not used again.
			ERROR("control request 0x%" B_PRIx32 " timed out, giving up on "
				"the device\n",
				B_LENDIAN_TO_HOST_INT32(((virtio_snd_hdr*)request)->code));
			info->controlFailed = true;
			info->broken = true;
			return B_TIMED_OUT;
		}
		info->controlCondition.Add(&entry);
	}

	memcpy(response, responseBuffer, responseSize);
	return virtio_status_to_error(
		B_LENDIAN_TO_HOST_INT32(((virtio_snd_hdr*)response)->code));
}


//!	Sends one of the simple PCM requests that only carry a stream id.
static status_t
pcm_request(VirtIOSoundDriverInfo* info, uint32 code)
{
	virtio_snd_pcm_hdr request;
	request.hdr.code = B_HOST_TO_LENDIAN_INT32(code);
	request.stream_id = B_HOST_TO_LENDIAN_INT32(info->outputStream);

	virtio_snd_hdr response;
	status_t status = control_request(info, &request, sizeof(request),
		&response, sizeof(response));
	if (status != B_OK)
		ERROR("PCM request 0x%" B_PRIx32 " failed: %s\n", code, strerror(status));
	return status;
}


// #pragma mark - TX queue


//!	TX queue interrupt callback. Runs in interrupt context.
static void
tx_callback(void* driverCookie, void* cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)cookie;

	void* bufferCookie;
	while (info->virtio->queue_dequeue(info->queues[VIRTIO_SND_VQ_TX],
			&bufferCookie, NULL)) {
		int32 generation;
		uint32 index;
		parse_tx_cookie(bufferCookie, generation, index);

		SpinLocker locker(info->txLock);
		// Every queued buffer was counted in inFlight, current or not.
		info->inFlight--;

		// Only buffers of the current stream are handed to the client.
		if (generation != (atomic_get(&info->generation) & 0xffffff)
			|| index >= info->bufferCount || index >= kMaxBuffers
			|| info->completedCount >= kMaxBuffers) {
			continue;
		}

		virtio_snd_pcm_status* pcmStatus = (virtio_snd_pcm_status*)
			(info->txHeaders + index * kTxSlotSize + kTxStatusOffset);
		bool error = B_LENDIAN_TO_HOST_INT32(pcmStatus->status)
			!= VIRTIO_SND_S_OK;
		if (error)
			info->txErrors++;

		info->framesPlayed += info->bufferFrames;
		uint32 slot = (info->completedHead + info->completedCount)
			% kMaxBuffers;
		info->completed[slot].index = (uint8)index;
		info->completed[slot].error = error;
		info->completed[slot].frames = info->framesPlayed;
		info->completed[slot].time = system_time();
		info->completedCount++;
		locker.Unlock();

		TRACE("buffer %" B_PRIu32 " done%s\n", index, error ? " (error)" : "");

		release_sem_etc(info->txSem, 1, B_DO_NOT_RESCHEDULE);
	}
}


//!	Hands playback buffer \a index to the device. Called with info->lock held.
static status_t
submit_buffer(VirtIOSoundDriverInfo* info, uint32 index)
{
	if (info->broken || index >= info->bufferCount)
		return B_IO_ERROR;

	uint8* slot = info->txHeaders + index * kTxSlotSize;
	virtio_snd_pcm_xfer* xfer = (virtio_snd_pcm_xfer*)slot;
	virtio_snd_pcm_status* pcmStatus
		= (virtio_snd_pcm_status*)(slot + kTxStatusOffset);
	xfer->stream_id = B_HOST_TO_LENDIAN_INT32(info->outputStream);
	pcmStatus->status = B_HOST_TO_LENDIAN_INT32(0xffffffff);
	pcmStatus->latency_bytes = 0;

	physical_entry entries[3];
	entries[0].address = info->txHeadersPhysical + index * kTxSlotSize;
	entries[0].size = sizeof(virtio_snd_pcm_xfer);
	entries[1].address = info->dataPhysical + index * info->periodBytes;
	entries[1].size = info->periodBytes;
	entries[2].address = info->txHeadersPhysical + index * kTxSlotSize
		+ kTxStatusOffset;
	entries[2].size = sizeof(virtio_snd_pcm_status);

	// Count it before queueing: the completion may arrive before
	// queue_request_v() returns.
	InterruptsSpinLocker locker(info->txLock);
	info->inFlight++;
	locker.Unlock();

	TRACE("queue buffer %" B_PRIu32 "\n", index);
	status_t status = info->virtio->queue_request_v(
		info->queues[VIRTIO_SND_VQ_TX], entries, 2, 1,
		make_tx_cookie(atomic_get(&info->generation), index));
	if (status != B_OK) {
		locker.Lock();
		info->inFlight--;
		locker.Unlock();
		ERROR("queueing playback buffer %" B_PRIu32 " failed: %s\n", index,
			strerror(status));
	}
	return status;
}


// #pragma mark - stream state


/*!	Sends SET_PARAMS and PREPARE for the current format, rate and buffer
	layout. Called with info->lock held.
*/
static status_t
prepare_stream(VirtIOSoundDriverInfo* info)
{
	const format_mapping* format = find_format(info->format);
	const rate_mapping* rate = find_rate(info->rate);
	if (format == NULL || rate == NULL || info->dataArea < 0)
		return B_NO_INIT;

	virtio_snd_pcm_set_params params;
	memset(&params, 0, sizeof(params));
	params.hdr.hdr.code = B_HOST_TO_LENDIAN_INT32(VIRTIO_SND_R_PCM_SET_PARAMS);
	params.hdr.stream_id = B_HOST_TO_LENDIAN_INT32(info->outputStream);
	params.buffer_bytes = B_HOST_TO_LENDIAN_INT32(
		info->periodBytes * info->bufferCount);
	params.period_bytes = B_HOST_TO_LENDIAN_INT32(info->periodBytes);
	params.features = 0;
	params.channels = kChannels;
	params.format = format->virtioFormat;
	params.rate = rate->virtioRate;

	virtio_snd_hdr response;
	status_t status = control_request(info, &params, sizeof(params),
		&response, sizeof(response));
	if (status != B_OK) {
		ERROR("SET_PARAMS (%" B_PRIu32 " Hz, %" B_PRIu32 "-byte samples, "
			"%" B_PRIu32 " x %" B_PRIu32 " bytes) failed: %s\n", rate->hertz,
			format->sampleSize, info->bufferCount, info->periodBytes,
			strerror(status));
		return status;
	}

	status = pcm_request(info, VIRTIO_SND_R_PCM_PREPARE);
	if (status != B_OK)
		return status;

	info->prepared = true;
	return B_OK;
}


/*!	Stops the stream and waits until the device has returned every buffer.
	Afterwards the buffers may be freed or rewritten. Called with info->lock
	held. Returns an error only if the device kept buffers; in that case the
	caller must not free them, and the device is marked broken.
*/
static status_t
stop_stream(VirtIOSoundDriverInfo* info)
{
	if (info->running)
		pcm_request(info, VIRTIO_SND_R_PCM_STOP);
	if (info->prepared) {
		// RELEASE makes the device complete all pending I/O messages.
		pcm_request(info, VIRTIO_SND_R_PCM_RELEASE);
	}

	TRACE("stopping stream (running %d, prepared %d)\n", info->running,
		info->prepared);
	info->running = false;
	info->prepared = false;
	info->fillIndex = -1;
	atomic_add(&info->generation, 1);

	// Wake up a client blocked in buffer_exchange(): it notices the new
	// generation and returns.
	release_sem_etc(info->txSem, 1, B_DO_NOT_RESCHEDULE);

	bigtime_t deadline = system_time() + kDrainTimeout;
	while (true) {
		InterruptsSpinLocker locker(info->txLock);
		int32 inFlight = info->inFlight;
		locker.Unlock();
		if (inFlight <= 0)
			break;
		if (info->controlFailed || system_time() > deadline) {
			ERROR("device still holds %" B_PRId32 " playback buffers, giving "
				"up on the device\n", inFlight);
			info->broken = true;
			return B_TIMED_OUT;
		}
		snooze(1000);
	}

	// Forget completions of the old stream, including their semaphore counts.
	InterruptsSpinLocker locker(info->txLock);
	int32 txErrors = info->txErrors;
	info->completedHead = 0;
	info->completedCount = 0;
	info->framesPlayed = 0;
	info->txErrors = 0;
	locker.Unlock();

	if (txErrors > 0) {
		ERROR("device reported errors for %" B_PRId32 " playback buffers\n",
			txErrors);
	}

	int32 count;
	if (get_sem_count(info->txSem, &count) == B_OK && count > 0)
		acquire_sem_etc(info->txSem, count, B_RELATIVE_TIMEOUT, 0);

	return B_OK;
}


/*!	Stops the stream and frees the playback buffers. Called with info->lock
	held.
*/
static status_t
free_buffers(VirtIOSoundDriverInfo* info)
{
	if (info->dataArea < 0)
		return B_OK;

	status_t status = stop_stream(info);
	if (status != B_OK) {
		// The device may still DMA from these pages: leak them rather than
		// hand them back to the system.
		ERROR("leaking playback buffers of a stuck device\n");
		info->dataArea = -1;
		info->data = NULL;
		return status;
	}

	delete_area(info->dataArea);
	info->dataArea = -1;
	info->data = NULL;
	return B_OK;
}


/*!	Allocates the playback ring for the current format and the given layout.
	Called with info->lock held and no buffers allocated.
*/
static status_t
allocate_buffers(VirtIOSoundDriverInfo* info, uint32 count, uint32 frames)
{
	const format_mapping* format = find_format(info->format);
	if (format == NULL)
		return B_NO_INIT;

	uint32 periodBytes = frames * kChannels * format->sampleSize;
	size_t areaSize = ROUNDUP((size_t)periodBytes * count, B_PAGE_SIZE);

	// The media add-on writes into these buffers directly from userland,
	// so the area is user-accessible, like the HDA driver's. Because of that
	// (and SMAP) the kernel itself only touches it through user_memset().
	void* address;
	area_id area = create_area("virtio_sound playback", &address,
		B_ANY_KERNEL_ADDRESS, areaSize, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, areaSize, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	info->dataArea = area;
	info->data = (uint8*)address;
	info->dataPhysical = entry.address;
	info->bufferCount = count;
	info->bufferFrames = frames;
	info->periodBytes = periodBytes;
	return B_OK;
}


/*!	Queues all buffers (silent) and starts the stream. Called with
	info->lock held.
*/
static status_t
start_stream(VirtIOSoundDriverInfo* info)
{
	if (info->broken)
		return B_IO_ERROR;

	if (!info->prepared) {
		status_t status = prepare_stream(info);
		if (status != B_OK)
			return status;
	}

	status_t status = user_memset(info->data, 0,
		(size_t)info->periodBytes * info->bufferCount);
	if (status != B_OK)
		return status;

	for (uint32 i = 0; i < info->bufferCount; i++) {
		status = submit_buffer(info, i);
		if (status != B_OK) {
			stop_stream(info);
			return status;
		}
	}

	status = pcm_request(info, VIRTIO_SND_R_PCM_START);
	if (status != B_OK) {
		stop_stream(info);
		return status;
	}

	info->running = true;
	info->fillIndex = -1;
	TRACE("stream started, generation %" B_PRId32 "\n",
		atomic_get(&info->generation));
	return B_OK;
}


/*!	Waits for about one period. Used on error paths of buffer_exchange():
	the multi_audio add-on calls it in a loop at real-time priority and
	ignores errors, so returning at once would starve a CPU.
*/
static void
snooze_period(VirtIOSoundDriverInfo* info)
{
	const rate_mapping* rate = find_rate(info->rate);
	bigtime_t period = 20000;
	if (rate != NULL && info->bufferFrames > 0)
		period = (bigtime_t)info->bufferFrames * 1000000 / rate->hertz;
	snooze(period);
}


// #pragma mark - multi_audio


static status_t
get_description(VirtIOSoundDriverInfo* info, multi_description* userData)
{
	multi_description description;
	if (user_memcpy(&description, userData, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;

	int32 requested = description.request_channel_count;
	multi_channel_info* userChannels = description.channels;

	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name, "Virtio Sound",
		sizeof(description.friendly_name));
	strlcpy(description.vendor_info, "Virtio",
		sizeof(description.vendor_info));

	description.output_channel_count = kChannels;
	description.input_channel_count = 0;
	description.output_bus_channel_count = kChannels;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;

	description.output_rates = info->outputRates;
	description.input_rates = 0;
	description.min_cvsr_rate = 0;
	description.max_cvsr_rate = 0;
	description.output_formats = info->outputFormats;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 3000;
	description.control_panel[0] = '\0';

	static const multi_channel_info kChannelInfo[kChannelInfoCount] = {
		{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
			0, {} },
		{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
			0, {} },
		{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
			B_CHANNEL_MINI_JACK_STEREO, {} },
		{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
			B_CHANNEL_MINI_JACK_STEREO, {} },
	};

	int32 count = min_c(requested, kChannelInfoCount);
	if (count > 0) {
		if (userChannels == NULL || !IS_USER_ADDRESS(userChannels)
			|| user_memcpy(userChannels, kChannelInfo,
				count * sizeof(multi_channel_info)) != B_OK) {
			return B_BAD_ADDRESS;
		}
	}

	if (user_memcpy(userData, &description, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
get_enabled_channels(VirtIOSoundDriverInfo* info,
	multi_channel_enable* userData)
{
	multi_channel_enable enable;
	if (user_memcpy(&enable, userData, sizeof(enable)) != B_OK)
		return B_BAD_ADDRESS;

	// Both output channels are always on and cannot be switched off.
	uchar bits = (1 << kChannels) - 1;
	if (enable.enable_bits == NULL || !IS_USER_ADDRESS(enable.enable_bits)
		|| user_memcpy(enable.enable_bits, &bits, sizeof(bits)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	enable.lock_source = B_MULTI_LOCK_INTERNAL;
	if (user_memcpy(userData, &enable, sizeof(enable)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
get_global_format(VirtIOSoundDriverInfo* info, multi_format_info* userData)
{
	multi_format_info formatInfo;
	if (user_memcpy(&formatInfo, userData, sizeof(formatInfo)) != B_OK)
		return B_BAD_ADDRESS;

	MutexLocker locker(info->lock);

	const rate_mapping* rate = find_rate(info->rate);
	uint32 frames = info->bufferFrames;
	uint32 count = info->dataArea >= 0 ? info->bufferCount : kDefaultBuffers;

	// Data queued at the device: all buffers but the one being filled.
	formatInfo.output_latency = rate != NULL
		? (bigtime_t)frames * (count - 1) * 1000000 / rate->hertz : 0;
	formatInfo.input_latency = 0;
	formatInfo.timecode_kind = 0;
	formatInfo.output.rate = info->rate;
	formatInfo.output.cvsr = 0;
	formatInfo.output.format = info->format;
	formatInfo.input.rate = 0;
	formatInfo.input.cvsr = 0;
	formatInfo.input.format = 0;
	locker.Unlock();

	if (user_memcpy(userData, &formatInfo, sizeof(formatInfo)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
set_global_format(VirtIOSoundDriverInfo* info, multi_format_info* userData)
{
	multi_format_info formatInfo;
	if (user_memcpy(&formatInfo, userData, sizeof(formatInfo)) != B_OK)
		return B_BAD_ADDRESS;

	// Input fields are ignored: there is no capture stream. The media
	// add-on fills them in anyway (with 0, as we advertise no input).
	uint32 newRate = formatInfo.output.rate;
	uint32 newFormat = formatInfo.output.format;
	if ((newRate & info->outputRates) == 0 || find_rate(newRate) == NULL
		|| (newFormat & info->outputFormats) == 0
		|| find_format(newFormat) == NULL) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(info->lock);
	if (newRate == info->rate && newFormat == info->format)
		return B_OK;

	// The buffer layout depends on the format; the media add-on always asks
	// for new buffers (B_MULTI_GET_BUFFERS) after changing the format.
	status_t status = free_buffers(info);
	if (status != B_OK)
		return status;

	info->rate = newRate;
	info->format = newFormat;
	return B_OK;
}


static status_t
get_buffers(VirtIOSoundDriverInfo* info, multi_buffer_list* userData)
{
	multi_buffer_list list;
	if (user_memcpy(&list, userData, sizeof(list)) != B_OK)
		return B_BAD_ADDRESS;

	uint32 count = list.request_playback_buffers;
	if (count < kMinBuffers || count > kMaxBuffers)
		count = kDefaultBuffers;

	MutexLocker locker(info->lock);

	if (info->broken)
		return B_IO_ERROR;

	uint32 frames = list.request_playback_buffer_size;
	if (frames < kMinBufferFrames || frames > kMaxBufferFrames) {
		// About 21 ms per period at 44.1/48 kHz.
		frames = 1024;
	}

	status_t status = free_buffers(info);
	if (status != B_OK)
		return status;

	status = allocate_buffers(info, count, frames);
	if (status != B_OK)
		return status;

	status = prepare_stream(info);
	if (status != B_OK) {
		free_buffers(info);
		return status;
	}

	const format_mapping* format = find_format(info->format);
	size_t sampleSize = format->sampleSize;

	// playback_buffers is a userland array of userland pointers, one per
	// buffer, each pointing at an array of one buffer_desc per channel.
	if (list.playback_buffers == NULL
		|| !IS_USER_ADDRESS(list.playback_buffers)) {
		free_buffers(info);
		return B_BAD_ADDRESS;
	}

	for (uint32 i = 0; i < count; i++) {
		buffer_desc* userDescs;
		if (user_memcpy(&userDescs, &list.playback_buffers[i],
				sizeof(userDescs)) != B_OK
			|| userDescs == NULL || !IS_USER_ADDRESS(userDescs)) {
			free_buffers(info);
			return B_BAD_ADDRESS;
		}

		buffer_desc descs[kChannels];
		memset(descs, 0, sizeof(descs));
		for (uint32 channel = 0; channel < kChannels; channel++) {
			descs[channel].base = (char*)info->data
				+ (size_t)i * info->periodBytes + channel * sampleSize;
			descs[channel].stride = sampleSize * kChannels;
		}
		if (user_memcpy(userDescs, descs, sizeof(descs)) != B_OK) {
			free_buffers(info);
			return B_BAD_ADDRESS;
		}
	}

	list.flags = B_MULTI_BUFFER_PLAYBACK;
	list.return_playback_buffers = count;
	list.return_playback_channels = kChannels;
	list.return_playback_buffer_size = frames;
	list.return_record_buffers = 0;
	list.return_record_channels = 0;
	list.return_record_buffer_size = 0;
	locker.Unlock();

	if (user_memcpy(userData, &list, sizeof(list)) != B_OK)
		return B_BAD_ADDRESS;

	INFO("playback: %" B_PRIu32 " buffers of %" B_PRIu32 " frames, "
		"%" B_PRIu32 " bytes each\n", count, frames, info->periodBytes);
	return B_OK;
}


static status_t
buffer_exchange(VirtIOSoundDriverInfo* info, multi_buffer_info* userData)
{
	multi_buffer_info bufferInfo;
	if (user_memcpy(&bufferInfo, userData, sizeof(bufferInfo)) != B_OK)
		return B_BAD_ADDRESS;

	// Only one thread may wait for completions; see the generation logic.
	MutexLocker exchangeLocker(info->exchangeLock);
	MutexLocker locker(info->lock);

	status_t status = B_OK;
	if (info->broken)
		status = B_IO_ERROR;
	else if (info->dataArea < 0)
		status = B_NO_INIT;
	else if (!info->running)
		status = start_stream(info);
	else if (info->fillIndex >= 0) {
		// The client has finished writing the buffer we gave it last time.
		// If queueing fails we keep it and try again on the next call.
		status = submit_buffer(info, info->fillIndex);
		if (status == B_OK)
			info->fillIndex = -1;
	}

	if (status != B_OK) {
		locker.Unlock();
		snooze_period(info);
		return status;
	}

	int32 generation = atomic_get(&info->generation);
	uint32 count = info->bufferCount;
	locker.Unlock();

	int32 index = -1;
	bool error = false;
	int64 frames = 0;
	bigtime_t realTime = 0;
	while (index < 0) {
		status = acquire_sem_etc(info->txSem, 1,
			B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, kExchangeTimeout);
		if (status != B_OK) {
			if (status == B_TIMED_OUT)
				ERROR("device stopped returning playback buffers\n");
			return status;
		}

		locker.Lock();
		if (atomic_get(&info->generation) != generation || !info->running) {
			// The stream was stopped while we waited. Not B_INTERRUPTED:
			// the kernel restarts syscalls that return it, which would
			// silently start the stream again right after the stop.
			return B_CANCELED;
		}

		InterruptsSpinLocker txLocker(info->txLock);
		if (info->completedCount > 0) {
			uint32 head = info->completedHead;
			index = info->completed[head].index;
			error = info->completed[head].error;
			frames = info->completed[head].frames;
			realTime = info->completed[head].time;
			info->completedHead = (head + 1) % kMaxBuffers;
			info->completedCount--;
		}
		txLocker.Unlock();

		if (index < 0 || (uint32)index >= count) {
			// A stale semaphore count from before a stop; wait again.
			index = -1;
			locker.Unlock();
			continue;
		}

		info->fillIndex = index;
		locker.Unlock();
	}

	// A device that fails buffers may return them at once; keep real-time
	// pacing so that the client does not spin.
	if (error)
		snooze_period(info);

	bufferInfo.flags = B_MULTI_BUFFER_PLAYBACK;
	bufferInfo.playback_buffer_cycle = (index + 1) % count;
	bufferInfo.played_real_time = realTime;
	bufferInfo.played_frames_count = frames;
	bufferInfo.record_buffer_cycle = -1;
	bufferInfo.recorded_real_time = 0;
	bufferInfo.recorded_frames_count = 0;

	if (user_memcpy(userData, &bufferInfo, sizeof(bufferInfo)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
buffer_force_stop(VirtIOSoundDriverInfo* info)
{
	MutexLocker locker(info->lock);
	// Keep the buffers: the media add-on may start again without asking for
	// new ones, in which case the next exchange prepares the stream again.
	return stop_stream(info);
}


static status_t
list_mix_controls(VirtIOSoundDriverInfo* info,
	multi_mix_control_info* userData)
{
	// No hardware mixer controls (VIRTIO_SND_F_CTLS is not negotiated).
	multi_mix_control_info controlInfo;
	if (user_memcpy(&controlInfo, userData, sizeof(controlInfo)) != B_OK)
		return B_BAD_ADDRESS;
	controlInfo.control_count = 0;
	if (user_memcpy(userData, &controlInfo, sizeof(controlInfo)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
multi_audio_control(VirtIOSoundDriverInfo* info, uint32 op, void* arg,
	size_t length)
{
	if (op != B_MULTI_BUFFER_EXCHANGE)
		TRACE("ioctl 0x%" B_PRIx32 "\n", op);

	switch (op) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description(info, (multi_description*)arg);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels(info, (multi_channel_enable*)arg);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format(info, (multi_format_info*)arg);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format(info, (multi_format_info*)arg);
		case B_MULTI_LIST_MIX_CONTROLS:
			return list_mix_controls(info, (multi_mix_control_info*)arg);
		case B_MULTI_GET_MIX:
		case B_MULTI_SET_MIX:
			return B_OK;
		case B_MULTI_GET_BUFFERS:
			return get_buffers(info, (multi_buffer_list*)arg);
		case B_MULTI_BUFFER_EXCHANGE:
			return buffer_exchange(info, (multi_buffer_info*)arg);
		case B_MULTI_BUFFER_FORCE_STOP:
			return buffer_force_stop(info);

		case B_MULTI_GET_EVENT_INFO:
		case B_MULTI_SET_EVENT_INFO:
		case B_MULTI_GET_EVENT:
		case B_MULTI_GET_CHANNEL_FORMATS:
		case B_MULTI_SET_CHANNEL_FORMATS:
		case B_MULTI_LIST_MIX_CHANNELS:
		case B_MULTI_LIST_MIX_CONNECTIONS:
		case B_MULTI_SET_BUFFERS:
		case B_MULTI_SET_START_TIME:
			return B_ERROR;
	}

	return B_DEV_INVALID_IOCTL;
}


// #pragma mark - transport setup


//!	Asks the device about its PCM streams and picks the output we drive.
static status_t
probe_streams(VirtIOSoundDriverInfo* info)
{
	uint32 streams = info->config.streams;
	uint32 maxStreams = (kControlMaxResponse - sizeof(virtio_snd_hdr))
		/ sizeof(virtio_snd_pcm_info);
	if (streams > maxStreams)
		streams = maxStreams;
	if (streams == 0)
		return B_ENTRY_NOT_FOUND;

	virtio_snd_query_info query;
	query.hdr.code = B_HOST_TO_LENDIAN_INT32(VIRTIO_SND_R_PCM_INFO);
	query.start_id = 0;
	query.count = B_HOST_TO_LENDIAN_INT32(streams);
	query.size = B_HOST_TO_LENDIAN_INT32(sizeof(virtio_snd_pcm_info));

	size_t responseSize = sizeof(virtio_snd_hdr)
		+ streams * sizeof(virtio_snd_pcm_info);
	uint8* response = (uint8*)malloc(responseSize);
	if (response == NULL)
		return B_NO_MEMORY;
	MemoryDeleter responseDeleter(response);

	status_t status = control_request(info, &query, sizeof(query), response,
		responseSize);
	if (status != B_OK) {
		ERROR("PCM_INFO failed: %s\n", strerror(status));
		return status;
	}

	const virtio_snd_pcm_info* pcmInfo
		= (const virtio_snd_pcm_info*)(response + sizeof(virtio_snd_hdr));
	for (uint32 i = 0; i < streams; i++) {
		uint64 formats = B_LENDIAN_TO_HOST_INT64(pcmInfo[i].formats);
		uint64 rates = B_LENDIAN_TO_HOST_INT64(pcmInfo[i].rates);
		INFO("stream %" B_PRIu32 ": %s, %u-%u channels, formats 0x%" B_PRIx64
			", rates 0x%" B_PRIx64 "\n", i,
			pcmInfo[i].direction == VIRTIO_SND_D_OUTPUT ? "output" : "input",
			pcmInfo[i].channels_min, pcmInfo[i].channels_max, formats, rates);

		if (pcmInfo[i].direction != VIRTIO_SND_D_OUTPUT
			|| pcmInfo[i].channels_min > kChannels
			|| pcmInfo[i].channels_max < kChannels
			|| info->outputStream >= 0) {
			continue;
		}

		uint32 haikuRates = 0;
		for (size_t r = 0; r < B_COUNT_OF(kRates); r++) {
			if ((rates & (1ULL << kRates[r].virtioRate)) != 0)
				haikuRates |= kRates[r].haikuRate;
		}

		// Advertise only the single best format we support, so that the
		// media add-on cannot pick a wider one (see the file comment).
		uint32 haikuFormat = 0;
		for (size_t f = 0; f < B_COUNT_OF(kFormats); f++) {
			if ((formats & (1ULL << kFormats[f].virtioFormat)) != 0) {
				haikuFormat = kFormats[f].haikuFormat;
				break;
			}
		}

		if (haikuRates == 0 || haikuFormat == 0)
			continue;

		info->outputStream = i;
		info->deviceFormats = formats;
		info->deviceRates = rates;
		info->outputRates = haikuRates;
		info->outputFormats = haikuFormat;
	}

	if (info->outputStream < 0) {
		ERROR("no usable stereo output stream\n");
		return B_NOT_SUPPORTED;
	}

	// Defaults until the media add-on sets a format: the best rate we offer.
	info->format = info->outputFormats;
	info->rate = (info->outputRates & B_SR_48000) != 0 ? B_SR_48000
		: B_SR_44100;
	info->bufferFrames = 1024;
	info->bufferCount = kDefaultBuffers;

	INFO("using output stream %" B_PRId32 "\n", info->outputStream);
	return B_OK;
}


//!	One-time virtio setup; see the file comment. Called with info->lock held.
static status_t
setup_transport(VirtIOSoundDriverInfo* info)
{
	device_node* parent = sDeviceManager->get_parent_node(info->node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&info->virtio,
		(void**)&info->virtioDevice);
	sDeviceManager->put_node(parent);

	status_t status = info->virtio->negotiate_features(info->virtioDevice, 0,
		&info->features, NULL);
	if (status != B_OK) {
		ERROR("feature negotiation failed: %s\n", strerror(status));
		return status;
	}

	status = info->virtio->read_device_config(info->virtioDevice, 0,
		&info->config, sizeof(info->config));
	if (status != B_OK)
		return status;
	info->config.jacks = B_LENDIAN_TO_HOST_INT32(info->config.jacks);
	info->config.streams = B_LENDIAN_TO_HOST_INT32(info->config.streams);
	info->config.chmaps = B_LENDIAN_TO_HOST_INT32(info->config.chmaps);
	INFO("%" B_PRIu32 " jacks, %" B_PRIu32 " streams, %" B_PRIu32
		" channel maps\n", info->config.jacks, info->config.streams,
		info->config.chmaps);

	status = info->virtio->alloc_queues(info->virtioDevice, VIRTIO_SND_VQ_MAX,
		info->queues, NULL);
	if (status != B_OK) {
		ERROR("queue allocation failed: %s\n", strerror(status));
		return status;
	}

	status = info->virtio->setup_interrupt(info->virtioDevice, NULL, info);
	if (status == B_OK) {
		status = info->virtio->queue_setup_interrupt(
			info->queues[VIRTIO_SND_VQ_CONTROL], control_callback, info);
	}
	if (status == B_OK) {
		status = info->virtio->queue_setup_interrupt(
			info->queues[VIRTIO_SND_VQ_TX], tx_callback, info);
	}
	if (status != B_OK) {
		ERROR("interrupt setup failed: %s\n", strerror(status));
		info->virtio->free_interrupts(info->virtioDevice);
		info->virtio->free_queues(info->virtioDevice);
		return status;
	}

	info->transportReady = true;
	return probe_streams(info);
}


// #pragma mark - device module API


static status_t
virtio_sound_init_device(void* _info, void** _cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)_info;

	MutexLocker locker(info->lock);
	if (!info->transportReady && !info->transportFailed) {
		if (setup_transport(info) != B_OK && !info->transportReady)
			info->transportFailed = true;
	} else if (info->transportReady && info->outputStream < 0
		&& !info->controlFailed) {
		// The stream probe failed last time; try again.
		probe_streams(info);
	}

	if (!info->transportReady || info->outputStream < 0)
		return B_NOT_SUPPORTED;

	*_cookie = info;
	return B_OK;
}


static void
virtio_sound_uninit_device(void* _cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)_cookie;

	// Last close: stop playing and give the memory back. The transport
	// stays up until the driver is unloaded.
	MutexLocker locker(info->lock);
	free_buffers(info);
}


static status_t
virtio_sound_open(void* _info, const char* path, int openMode, void** _cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)_info;

	// One client at a time: a second one could free the buffers the first
	// one is writing to, or steal its completions.
	if (atomic_test_and_set(&info->openCount, 1, 0) != 0)
		return B_BUSY;

	VirtIOSoundHandle* handle
		= (VirtIOSoundHandle*)malloc(sizeof(VirtIOSoundHandle));
	if (handle == NULL) {
		atomic_set(&info->openCount, 0);
		return B_NO_MEMORY;
	}

	handle->info = info;
	*_cookie = handle;
	return B_OK;
}


static status_t
virtio_sound_close(void* cookie)
{
	return B_OK;
}


static status_t
virtio_sound_free(void* cookie)
{
	VirtIOSoundHandle* handle = (VirtIOSoundHandle*)cookie;
	atomic_set(&handle->info->openCount, 0);
	free(handle);
	return B_OK;
}


static status_t
virtio_sound_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
virtio_sound_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
virtio_sound_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	VirtIOSoundHandle* handle = (VirtIOSoundHandle*)cookie;
	return multi_audio_control(handle->info, op, buffer, length);
}


// #pragma mark - driver module API


static float
virtio_sound_supports_device(device_node* parent)
{
	const char* bus;
	uint16 deviceType;

	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| sDeviceManager->get_attr_uint16(parent, VIRTIO_DEVICE_TYPE_ITEM,
			&deviceType, true) != B_OK) {
		return 0.0f;
	}

	if (strcmp(bus, "virtio") != 0 || deviceType != VIRTIO_DEVICE_ID_SOUND)
		return 0.0f;

	return 0.6f;
}


static status_t
virtio_sound_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "Virtio Sound" } },
		{ NULL }
	};

	return sDeviceManager->register_node(node, VIRTIO_SOUND_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static area_id
create_dma_page(const char* name, uint8** _address, phys_addr_t* _physical)
{
	void* address;
	area_id area = create_area(name, &address, B_ANY_KERNEL_ADDRESS,
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	memset(address, 0, B_PAGE_SIZE);
	*_address = (uint8*)address;
	*_physical = entry.address;
	return area;
}


static void virtio_sound_uninit_driver(void* cookie);


static status_t
virtio_sound_init_driver(device_node* node, void** _cookie)
{
	VirtIOSoundDriverInfo* info = new(std::nothrow) VirtIOSoundDriverInfo;
	if (info == NULL)
		return B_NO_MEMORY;

	info->node = node;
	info->virtioDevice = NULL;
	info->virtio = NULL;
	info->features = 0;
	info->transportReady = false;
	info->transportFailed = false;
	memset(info->queues, 0, sizeof(info->queues));
	memset(&info->config, 0, sizeof(info->config));

	mutex_init(&info->controlLock, "virtio_sound control");
	info->controlCondition.Init(info, "virtio_sound control");
	info->controlRequestId = 0;
	info->controlCompletedId = 0;

	info->outputStream = -1;
	info->deviceFormats = 0;
	info->deviceRates = 0;
	info->outputFormats = 0;
	info->outputRates = 0;

	mutex_init(&info->lock, "virtio_sound");
	mutex_init(&info->exchangeLock, "virtio_sound exchange");
	info->format = 0;
	info->rate = 0;
	info->bufferFrames = 0;
	info->bufferCount = 0;
	info->periodBytes = 0;
	info->dataArea = -1;
	info->data = NULL;
	info->dataPhysical = 0;
	info->prepared = false;
	info->running = false;
	info->fillIndex = -1;
	info->generation = 0;

	info->broken = false;
	info->controlFailed = false;
	info->openCount = 0;

	B_INITIALIZE_SPINLOCK(&info->txLock);
	info->completedHead = 0;
	info->completedCount = 0;
	info->inFlight = 0;
	info->framesPlayed = 0;
	info->txErrors = 0;

	info->controlArea = create_dma_page("virtio_sound control",
		&info->controlBuffer, &info->controlPhysical);
	info->txHeaderArea = create_dma_page("virtio_sound tx headers",
		&info->txHeaders, &info->txHeadersPhysical);
	info->txSem = create_sem(0, "virtio_sound tx");

	if (info->controlArea < 0 || info->txHeaderArea < 0 || info->txSem < 0) {
		virtio_sound_uninit_driver(info);
		return B_NO_MEMORY;
	}

	*_cookie = info;
	return B_OK;
}


static void
virtio_sound_uninit_driver(void* cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)cookie;

	mutex_lock(&info->lock);
	free_buffers(info);
	if (info->transportReady) {
		info->virtio->free_interrupts(info->virtioDevice);
		info->virtio->free_queues(info->virtioDevice);
		info->transportReady = false;
	}

	if (info->controlArea >= 0)
		delete_area(info->controlArea);
	if (info->txHeaderArea >= 0)
		delete_area(info->txHeaderArea);
	if (info->txSem >= 0)
		delete_sem(info->txSem);

	mutex_unlock(&info->lock);

	mutex_destroy(&info->exchangeLock);
	mutex_destroy(&info->lock);
	mutex_destroy(&info->controlLock);
	delete info;
}


static status_t
virtio_sound_register_child_devices(void* cookie)
{
	VirtIOSoundDriverInfo* info = (VirtIOSoundDriverInfo*)cookie;

	int32 id = sDeviceManager->create_id(VIRTIO_SOUND_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "audio/hmulti/virtio_sound/%" B_PRId32, id);

	status_t status = sDeviceManager->publish_device(info->node, name,
		VIRTIO_SOUND_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(VIRTIO_SOUND_DEVICE_ID_GENERATOR, id);
	return status;
}


// #pragma mark -


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


struct device_module_info sVirtioSoundDevice = {
	{
		VIRTIO_SOUND_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	virtio_sound_init_device,
	virtio_sound_uninit_device,
	NULL,	// remove

	virtio_sound_open,
	virtio_sound_close,
	virtio_sound_free,
	virtio_sound_read,
	virtio_sound_write,
	NULL,	// io
	virtio_sound_ioctl,

	NULL,	// select
	NULL,	// deselect
};


struct driver_module_info sVirtioSoundDriver = {
	{
		VIRTIO_SOUND_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	virtio_sound_supports_device,
	virtio_sound_register_device,
	virtio_sound_init_driver,
	virtio_sound_uninit_driver,
	virtio_sound_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sVirtioSoundDriver,
	(module_info*)&sVirtioSoundDevice,
	NULL
};
