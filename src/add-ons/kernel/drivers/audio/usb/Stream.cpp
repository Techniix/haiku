/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Copyright 2026, Renga contributors.
 *	Distributed under the terms of the MIT license.
 */


#include "Stream.h"

#include <kernel.h>
#include <usb/USB_audio.h>

#include "Device.h"
#include "Driver.h"
#include "Settings.h"
#include "UAC2.h"


// Transfers in flight and how much audio each carries. Four transfers of
// 8 ms keep 24 to 32 ms queued: enough for the host controller's finisher
// thread to be late now and then (and for emulated controllers), not so
// much that the latency becomes noticeable. FreeBSD's uaudio uses two to
// four transfers of a few milliseconds; Linux eight URBs of 1 ms or more.
static const uint32 kTransferCount = 4;
static const uint32 kTransferMilliseconds = 8;
// the multi_audio ring
static const uint32 kMinBufferFrames = 256;
static const uint32 kMaxBufferFrames = 16384;


Stream::Stream(Device* device, size_t interface, usb_interface_list* List)
	:
	AudioStreamingInterface(&device->AudioControl(), interface, List),
	fDevice(device),
	fStatus(B_NO_INIT),
	fStreamEndpoint(0),
	fFeedbackEndpoint(0),
	fConfiguration(NULL),
	fIsRunning(false),
	fStopping(false),
	fArea(-1),
	fKernelArea(-1),
	fAreaSize(0),
	fBuffers(NULL),
	fKernelBuffers(NULL),
	fBufferFrames(kSamplesBufferSize),
	fBufferCount(kSamplesBufferCount),
	fRingSampleSize(0),
	fRingFrameSize(0),
	fRate(0),
	fFrameSize(0),
	fSubslotSize(0),
	fChannels(0),
	fHighSpeed(false),
	fPacketsPerSecond(1000),
	fPacketsPerTransfer(0),
	fMaxPacketBytes(0),
	fMaxPacketFrames(0),
	fNominalFrames(0),
	fFeedbackFrames(0),
	fAccumulator(0),
	fTransferArea(-1),
	fTransferCount(0),
	fFeedbackData(NULL),
	fRingPosition(0),
	fCycle(0),
	fCycleFrames(0),
	fCycleTime(0),
	fCycles(0),
	fReportedCycles(0),
	fUnderruns(0),
	fInsideNotify(0),
	fOutstanding(0)
{
	B_INITIALIZE_SPINLOCK(&fLock);
	memset(fTransfers, 0, sizeof(fTransfers));
	memset(&fFeedbackTransfer, 0, sizeof(fFeedbackTransfer));
}


Stream::~Stream()
{
	_FreeTransfers();
	// The ring lives in the client's team. A device unplugged while open is
	// deleted when devfs disconnects its file descriptors, which happens at
	// once, while the client (the multi_audio node) may still write to the
	// buffers until it notices: deleting the area then crashed it. Leave it
	// to the client; it goes when the client's team does.
	if (fArea >= 0 && !fDevice->RemovedWhileOpen())
		delete_area(fArea);
	if (fKernelArea >= 0)
		delete_area(fKernelArea);
}


//	#pragma mark - setup


/*!	Picks the alternate setting to use: the most channels (up to 8), then
	the highest resolution, then PCM over the other formats. The format can
	still change to another alternate with the same channel count
	(SetGlobalFormat()).
*/
status_t
Stream::_ChooseAlternate()
{
	int32 best = -1;
	uint32 bestScore = 0;
	for (int32 i = 0; i < fAlternates.Count(); i++) {
		AudioStreamAlternate* alternate = fAlternates[i];
		if (!alternate->IsUsable())
			continue;
		uint32 score = alternate->Channels() * 1000
			+ alternate->BitResolution() * 10
			+ (alternate->FormatTag() == USB_AUDIO_FORMAT_PCM ? 1 : 0);
		if (score > bestScore) {
			bestScore = score;
			best = i;
		}
	}

	if (best < 0) {
		TRACE(ERR, "interface %d: no usable alternate setting\n",
			(int)fInterface);
		return B_NO_INIT;
	}

	fActiveAlternate = best;
	fIsInput = fAlternates[best]->IsInput();

	TRACE(INF, "Alternate %d EP:%#x selected for %s\n", (int)best,
		fAlternates[best]->EndpointAddress(),
		fIsInput ? "recording" : "playback");
	return B_OK;
}


/*!	UAC2: the rates come from the clock source the stream's terminal is
	clocked by. Every alternate of the interface uses that clock.
*/
status_t
Stream::_ReadRates()
{
	AudioControlInterface& control = fDevice->AudioControl();
	if (control.SpecReleaseNumber() < USB_AUDIO_CLASS_VERSION_2)
		return B_OK;

	_AudioControl* terminal = control.Find(TerminalLink());
	if (terminal == NULL
		|| (terminal->SubType() != USB_AUDIO_AC_INPUT_TERMINAL
			&& terminal->SubType() != USB_AUDIO_AC_OUTPUT_TERMINAL)) {
		TRACE(ERR, "no terminal %d for the stream\n", TerminalLink());
		return B_ERROR;
	}
	uint8 clock = control.ResolveClockSource(
		static_cast<_Terminal*>(terminal)->ClockSourceID());
	if (clock == 0) {
		TRACE(ERR, "terminal %d has no clock source\n", TerminalLink());
		return B_ERROR;
	}

	Vector<uint32> rates;
	bool continuous;
	uint32 minimum, maximum;
	status_t status = control.GetClockRates(clock, rates, continuous,
		minimum, maximum);
	if (status != B_OK)
		return status;

	for (int32 i = 0; i < fAlternates.Count(); i++)
		fAlternates[i]->SetRates(rates);
	return B_OK;
}


uint8
Stream::ClockSourceID()
{
	AudioControlInterface& control = fDevice->AudioControl();
	if (control.SpecReleaseNumber() < USB_AUDIO_CLASS_VERSION_2)
		return 0;
	_AudioControl* terminal = control.Find(TerminalLink());
	if (terminal == NULL
		|| (terminal->SubType() != USB_AUDIO_AC_INPUT_TERMINAL
			&& terminal->SubType() != USB_AUDIO_AC_OUTPUT_TERMINAL)) {
		return 0;
	}
	return control.ResolveClockSource(
		static_cast<_Terminal*>(terminal)->ClockSourceID());
}


status_t
Stream::Init()
{
	fStatus = _ChooseAlternate();
	if (fStatus != B_OK)
		return fStatus;

	if (_ReadRates() != B_OK) {
		fStatus = B_NO_INIT;
		return fStatus;
	}

	AudioStreamAlternate* alternate = ActiveAlternate();
	// The highest rate by default, as the multi_audio add-on also picks.
	fRate = 0;
	if (alternate->IsContinuous())
		fRate = alternate->RateMax();
	for (int32 i = 0; i < alternate->Rates().Count(); i++)
		fRate = max_c(fRate, alternate->Rates()[i]);
	if (fRate == 0) {
		TRACE(ERR, "interface %d: no sample rate\n", (int)fInterface);
		fStatus = B_NO_INIT;
		return fStatus;
	}

	fChannels = alternate->Channels();
	fSubslotSize = alternate->SubslotSize();
	fFrameSize = fChannels * fSubslotSize;
	fHighSpeed = fDevice->IsHighSpeed();
	return fStatus;
}


void
Stream::OnRemove()
{
	// The transfer callback schedules traffic, so we must make sure we are
	// not inside the callback any more before returning, as we would
	// otherwise break the promise not to use any of the pipes after
	// returning from the removed callback.
	fStopping = true;
	if (fStreamEndpoint != 0)
		gUSBModule->cancel_queued_transfers(fStreamEndpoint);
	if (fFeedbackEndpoint != 0)
		gUSBModule->cancel_queued_transfers(fFeedbackEndpoint);
	while (atomic_get(&fInsideNotify) != 0)
		snooze(100);
	_WaitForCompletions(1000000);
	fIsRunning = false;
}


status_t
Stream::OnSetConfiguration(usb_device device,
		const usb_configuration_info* config)
{
	if (config == NULL) {
		TRACE(ERR, "NULL configuration. Not set.\n");
		return B_ERROR;
	}

	// The interface stays at its zero bandwidth setting (alternate 0) until
	// the stream starts: a device needs the bus bandwidth only while it
	// plays or records, and several devices on one bus may not all fit.
	fConfiguration = config;
	return B_OK;
}


/*!	The multi_audio ring, in the client's address space and cloned into the
	kernel's. It holds fBufferCount buffers of fBufferFrames frames,
	interleaved, with fRingSampleSize bytes per sample.
*/
status_t
Stream::_SetupBuffers()
{
	fRingSampleSize = fSubslotSize == 3 ? 4 : fSubslotSize;
	fRingFrameSize = fRingSampleSize * fChannels;

	size_t size = (size_t)fRingFrameSize * fBufferFrames * fBufferCount;
	size = (size + (B_PAGE_SIZE - 1)) & ~(B_PAGE_SIZE - 1);
	if (fArea >= 0 && size == fAreaSize)
		return B_OK;

	if (fArea >= 0) {
		delete_area(fArea);
		delete_area(fKernelArea);
		fArea = fKernelArea = -1;
	}

	fAreaSize = size;
	fArea = create_area(fIsInput ? DRIVER_NAME "_record_area"
		: DRIVER_NAME "_playback_area", (void**)&fBuffers,
		B_ANY_ADDRESS, fAreaSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (fArea < 0) {
		TRACE(ERR, "could not create a %#x byte buffer area: %#x\n",
			(int)fAreaSize, fArea);
		fAreaSize = 0;
		return fArea;
	}

	// The kernel is not allowed to touch userspace areas, so we clone our
	// area into kernel space.
	fKernelArea = clone_area(DRIVER_NAME " cloned area",
		(void**)&fKernelBuffers, B_ANY_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, fArea);
	if (fKernelArea < 0) {
		status_t status = fKernelArea;
		delete_area(fArea);
		fArea = -1;
		fAreaSize = 0;
		return status;
	}
	memset(fKernelBuffers, 0, fAreaSize);
	return B_OK;
}


/*!	Works out the packet schedule for the current rate and allocates the
	bounce buffers of the transfers.

	An isochronous endpoint is served every 2^(bInterval - 1) frames (1 ms)
	at full speed and every 2^(bInterval - 1) microframes (125 us) at high
	speed and faster (USB 2.0 9.6.6). Packets of at most wMaxPacketSize
	bytes; at high speed bits 12..11 allow up to two more transactions per
	microframe.
*/
status_t
Stream::_SetupTransfers()
{
	_FreeTransfers();

	AudioStreamAlternate* alternate = ActiveAlternate();
	uint32 interval = alternate->Interval();
	if (interval < 1)
		interval = 1;
	if (interval > 16)
		interval = 16;
	uint32 unitsPerSecond = fHighSpeed ? 8000 : 1000;
	fPacketsPerSecond = unitsPerSecond >> (interval - 1);
	if (fPacketsPerSecond == 0)
		fPacketsPerSecond = 1;

	// Only the 11-bit size: Haiku's host controller drivers do not set up
	// high-bandwidth endpoints (several transactions per microframe, bits
	// 12..11) yet, and a packet larger than the pipe's maximum is refused.
	// Audio rarely needs more than 1024 bytes per microframe (8 channels
	// of 24-bit audio at 192 kHz take 576).
	uint16 maxPacket = alternate->MaxPacketSize();
	fMaxPacketBytes = maxPacket & 0x7ff;
	fMaxPacketFrames = fFrameSize > 0 ? fMaxPacketBytes / fFrameSize : 0;

	fNominalFrames = NominalFramesPerPacket(fRate, fPacketsPerSecond);
	fFeedbackFrames = 0;
	fAccumulator = 0;
	// A packet may have to carry one frame more than the average.
	if (fMaxPacketFrames < (fNominalFrames >> 16) + 1
		&& !(fMaxPacketFrames == (fNominalFrames >> 16)
			&& (fNominalFrames & 0xffff) == 0)) {
		TRACE(ERR, "%u Hz needs packets of %u frames, the endpoint takes %u\n",
			(unsigned)fRate, (unsigned)(fNominalFrames >> 16) + 1,
			(unsigned)fMaxPacketFrames);
		return B_BAD_VALUE;
	}

	fPacketsPerTransfer = fPacketsPerSecond * kTransferMilliseconds / 1000;
	if (fPacketsPerTransfer < 1)
		fPacketsPerTransfer = 1;
	fTransferCount = kTransferCount;

	size_t dataSize = (size_t)fMaxPacketBytes * fPacketsPerTransfer;
	size_t descriptorsSize = sizeof(usb_iso_packet_descriptor)
		* fPacketsPerTransfer;
	size_t perTransfer = (dataSize + descriptorsSize + 15) & ~(size_t)15;
	size_t feedbackSize = 64 + sizeof(usb_iso_packet_descriptor);
	size_t size = perTransfer * fTransferCount + feedbackSize;
	size = (size + (B_PAGE_SIZE - 1)) & ~(B_PAGE_SIZE - 1);

	uint8* base;
	fTransferArea = create_area(DRIVER_NAME " transfers", (void**)&base,
		B_ANY_KERNEL_ADDRESS, size, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fTransferArea < 0)
		return fTransferArea;
	memset(base, 0, size);

	for (uint32 i = 0; i < fTransferCount; i++) {
		StreamTransfer& transfer = fTransfers[i];
		transfer.stream = this;
		transfer.data = base + perTransfer * i;
		transfer.packets = (usb_iso_packet_descriptor*)(transfer.data
			+ dataSize);
		transfer.packetCount = fPacketsPerTransfer;
		transfer.length = 0;
		transfer.frames = 0;
	}
	fFeedbackData = base + perTransfer * fTransferCount;
	fFeedbackTransfer.stream = this;
	fFeedbackTransfer.data = fFeedbackData;
	fFeedbackTransfer.packets = (usb_iso_packet_descriptor*)(fFeedbackData
		+ 64);
	fFeedbackTransfer.packetCount = 1;

	TRACE(INF, "%s: %u Hz, %u packets/s of up to %u frames (%u bytes), "
		"%u packets per transfer, %s speed\n", fIsInput ? "record" : "play",
		(unsigned)fRate, (unsigned)fPacketsPerSecond,
		(unsigned)fMaxPacketFrames, (unsigned)fMaxPacketBytes,
		(unsigned)fPacketsPerTransfer, fHighSpeed ? "high" : "full");
	return B_OK;
}


void
Stream::_FreeTransfers()
{
	if (fTransferArea >= 0)
		delete_area(fTransferArea);
	fTransferArea = -1;
	fTransferCount = 0;
	fFeedbackData = NULL;
}


/*!	Renga: lets go of transfers a callback may still use, without freeing
	them (Stop() when a completion did not come within a second).
*/
void
Stream::_ForgetTransfers()
{
	fTransferArea = -1;
	fTransferCount = 0;
	fFeedbackData = NULL;
	atomic_set(&fOutstanding, 0);
}


/*!	Finds the pipes of the active alternate. Setting an alternate creates
	new pipes, so this has to follow every set_alt_interface().
*/
status_t
Stream::_FindPipes()
{
	fStreamEndpoint = 0;
	fFeedbackEndpoint = 0;

	const usb_configuration_info* config
		= gUSBModule->get_configuration(fDevice->USBDevice());
	if (config == NULL)
		config = fConfiguration;
	if (config == NULL || fInterface >= config->interface_count)
		return B_ERROR;

	usb_interface_info* interface = config->interface[fInterface].active;
	if (interface == NULL)
		return B_ERROR;

	AudioStreamAlternate* alternate = ActiveAlternate();
	for (size_t i = 0; i < interface->endpoint_count; i++) {
		uint8 address = interface->endpoint[i].descr->endpoint_address;
		if (address == alternate->EndpointAddress())
			fStreamEndpoint = interface->endpoint[i].handle;
		else if (address == alternate->FeedbackAddress()
			&& alternate->FeedbackAddress() != 0)
			fFeedbackEndpoint = interface->endpoint[i].handle;
	}

	if (fStreamEndpoint == 0) {
		TRACE(ERR, "endpoint %#x not found\n", alternate->EndpointAddress());
		return B_ERROR;
	}
	return B_OK;
}


/*!	Sets the stream's sample rate on the device: UAC1 through the endpoint's
	sampling frequency control (5.2.3.2.3.1), sent if the endpoint says it
	has one or the format offers more than one rate (FreeBSD and Linux send
	it in both cases and ignore a stall); UAC2 through the clock source, if
	the clock's frequency is programmable, and then read back.
*/
status_t
Stream::_SetRate()
{
	AudioControlInterface& control = fDevice->AudioControl();
	AudioStreamAlternate* alternate = ActiveAlternate();

	if (control.SpecReleaseNumber() >= USB_AUDIO_CLASS_VERSION_2) {
		_AudioControl* terminal = control.Find(TerminalLink());
		if (terminal == NULL)
			return B_ERROR;
		uint8 clock = control.ResolveClockSource(
			static_cast<_Terminal*>(terminal)->ClockSourceID());
		ClockSource* source = static_cast<ClockSource*>(control.Find(clock));
		if (source == NULL)
			return B_ERROR;
		uint32 current = 0;
		if (control.GetClockRate(clock, current) == B_OK && current == fRate)
			return B_OK;
		if (!source->CanSetFrequency()) {
			TRACE(ERR, "clock %d runs at %u Hz and cannot be set to %u Hz\n",
				clock, (unsigned)current, (unsigned)fRate);
			return B_NOT_SUPPORTED;
		}
		status_t status = control.SetClockRate(clock, fRate);
		if (status != B_OK)
			return status;
		if (control.GetClockRate(clock, current) == B_OK && current != fRate) {
			TRACE(ERR, "clock %d: asked for %u Hz, runs at %u Hz\n", clock,
				(unsigned)fRate, (unsigned)current);
			return B_ERROR;
		}
		if (!control.IsClockValid(clock))
			TRACE(INF, "clock %d is not valid (yet)\n", clock);
		return B_OK;
	}

	bool hasControl = (alternate->CSAttributes() & 0x01) != 0;
	if (!hasControl && !alternate->IsContinuous()
		&& alternate->Rates().Count() <= 1) {
		return B_OK;
	}

	usb_audio_sampling_freq freq = AudioStreamAlternate::GetSamFreq(fRate);
	size_t actualLength = 0;
	status_t status = gUSBModule->send_request(fDevice->USBDevice(),
		USB_REQTYPE_CLASS | USB_REQTYPE_ENDPOINT_OUT,
		USB_AUDIO_SET_CUR, USB_AUDIO_SAMPLING_FREQ_CONTROL << 8,
		alternate->EndpointAddress(), sizeof(freq), &freq, &actualLength);
	TRACE(status == B_OK ? INF : ERR, "set rate %u for ep %#x: %s\n",
		(unsigned)fRate, alternate->EndpointAddress(), strerror(status));
	return B_OK;
}


//	#pragma mark - streaming


uint32
Stream::NominalFramesPerPacket(uint32 rate, uint32 packetsPerSecond)
{
	if (packetsPerSecond == 0)
		return 0;
	return (uint32)(((uint64)rate << 16) / packetsPerSecond);
}


uint32
Stream::NextPacketFrames(uint32& accumulator, uint32 framesPerPacket)
{
	accumulator += framesPerPacket;
	uint32 frames = accumulator >> 16;
	accumulator &= 0xffff;
	return frames;
}


uint32
Stream::ParseFeedback(const uint8* data, size_t length, bool highSpeed,
	uint32 nominal, uint32 unitsPerPacket)
{
	if (length < 3 || nominal == 0 || unitsPerPacket == 0)
		return 0;

	uint32 value = data[0] | (data[1] << 8) | (data[2] << 16);
	if (length >= 4)
		value |= (uint32)data[3] << 24;

	// Candidates in 16.16 frames per (micro)frame: the format the speed
	// calls for first, then the usual mistakes (a full speed 10.14 value
	// at high speed, frames per millisecond at high speed).
	uint64 candidates[3];
	int count = 0;
	if (highSpeed) {
		candidates[count++] = value;
		candidates[count++] = (uint64)value << 2;
		candidates[count++] = ((uint64)value << 2) / 8;
	} else {
		candidates[count++] = ((uint64)(value & 0xffffff)) << 2;
		candidates[count++] = value;
	}

	for (int i = 0; i < count; i++) {
		uint64 perPacket = candidates[i] * unitsPerPacket;
		if (perPacket >= (uint64)nominal * 3 / 4
			&& perPacket <= (uint64)nominal * 5 / 4) {
			return (uint32)perPacket;
		}
	}
	return 0;
}


/*!	Moves the ring position \a frames further and reports the buffers whose
	end it passed: to the client through ExchangeBuffer() and, if this
	stream paces the device, through the exchange semaphore.
*/
void
Stream::_Advance(uint32 frames, bigtime_t now)
{
	if (fBufferFrames == 0 || fBufferCount == 0)
		return;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&fLock);

	uint32 ringFrames = fBufferFrames * fBufferCount;
	uint32 oldBuffer = fRingPosition / fBufferFrames;
	uint32 total = fRingPosition + frames;
	uint32 crossings = total / fBufferFrames - oldBuffer;
	for (uint32 i = 0; i < crossings; i++) {
		uint32 finished = (oldBuffer + i) % fBufferCount;
		fCycle = fIsInput ? finished : (finished + 1) % fBufferCount;
		fCycleFrames += fBufferFrames;
		fCycles++;
	}
	if (crossings > 0)
		fCycleTime = now;
	fRingPosition = total % ringFrames;

	release_spinlock(&fLock);
	restore_interrupts(state);

	if (crossings > 0 && fDevice->IsPacer(this)) {
		release_sem_etc(fDevice->fBuffersReadySem, crossings,
			B_DO_NOT_RESCHEDULE);
	}
}


/*!	Playback: fills \a transfer with the next frames of the ring, as many
	per packet as the rate (or the device's feedback) asks for, converting
	4-byte samples to 3-byte subslots where needed.
*/
void
Stream::_FillTransfer(StreamTransfer* transfer)
{
	uint32 ringFrames = fBufferFrames * fBufferCount;
	uint32 position = fRingPosition;
	uint32 rate = fFeedbackFrames != 0 ? fFeedbackFrames : fNominalFrames;
	uint8* out = transfer->data;
	uint32 total = 0;

	for (uint32 p = 0; p < transfer->packetCount; p++) {
		uint32 frames = NextPacketFrames(fAccumulator, rate);
		if (frames > fMaxPacketFrames)
			frames = fMaxPacketFrames;
		if (frames == 0) {
			// The host controllers do not take empty packets; one frame
			// early is inaudible.
			frames = 1;
		}

		for (uint32 f = 0; f < frames; f++) {
			const uint8* in = fKernelBuffers + (size_t)position
				* fRingFrameSize;
			if (fSubslotSize == 3) {
				for (uint32 c = 0; c < fChannels; c++) {
					// left-justified: the three high bytes of the sample
					out[0] = in[1];
					out[1] = in[2];
					out[2] = in[3];
					out += 3;
					in += 4;
				}
			} else {
				memcpy(out, in, fFrameSize);
				out += fFrameSize;
			}
			position = (position + 1) % ringFrames;
		}

		transfer->packets[p].request_length = frames * fFrameSize;
		transfer->packets[p].actual_length = 0;
		transfer->packets[p].status = B_NO_INIT;
		total += frames;
	}

	transfer->length = out - transfer->data;
	transfer->frames = total;
	_Advance(total, system_time());
}


/*!	Capture: copies what each packet of \a transfer brought into the ring.
	The packets lie at their request offsets (every request is
	fMaxPacketBytes), and each says how much it carries.
*/
void
Stream::_DrainTransfer(StreamTransfer* transfer)
{
	uint32 ringFrames = fBufferFrames * fBufferCount;
	uint32 position = fRingPosition;
	uint32 total = 0;

	for (uint32 p = 0; p < transfer->packetCount; p++) {
		usb_iso_packet_descriptor& packet = transfer->packets[p];
		if (packet.status != B_OK)
			continue;
		size_t bytes = min_c(packet.actual_length, packet.request_length);
		uint32 frames = bytes / fFrameSize;
		const uint8* in = transfer->data + (size_t)p * fMaxPacketBytes;

		for (uint32 f = 0; f < frames; f++) {
			uint8* out = fKernelBuffers + (size_t)position * fRingFrameSize;
			if (fSubslotSize == 3) {
				for (uint32 c = 0; c < fChannels; c++) {
					out[0] = 0;
					out[1] = in[0];
					out[2] = in[1];
					out[3] = in[2];
					out += 4;
					in += 3;
				}
			} else {
				memcpy(out, in, fFrameSize);
				in += fFrameSize;
			}
			position = (position + 1) % ringFrames;
		}
		total += frames;
	}

	if (total > 0)
		_Advance(total, system_time());
}


status_t
Stream::_QueueTransfer(StreamTransfer* transfer)
{
	if (fIsInput) {
		for (uint32 p = 0; p < transfer->packetCount; p++) {
			transfer->packets[p].request_length = fMaxPacketBytes;
			transfer->packets[p].actual_length = 0;
			transfer->packets[p].status = B_NO_INIT;
		}
		transfer->length = (size_t)fMaxPacketBytes * transfer->packetCount;
	}

	atomic_add(&fOutstanding, 1);
	status_t status = gUSBModule->queue_isochronous(fStreamEndpoint,
		transfer->data, transfer->length, transfer->packets,
		transfer->packetCount, NULL, USB_ISO_ASAP, Stream::_TransferCallback,
		transfer);
	if (status != B_OK)
		atomic_add(&fOutstanding, -1);
	return status;
}


status_t
Stream::_QueueFeedback()
{
	if (fFeedbackEndpoint == 0 || fFeedbackData == NULL)
		return B_OK;

	AudioStreamAlternate* alternate = ActiveAlternate();
	size_t length = alternate->FeedbackMaxPacketSize();
	if (length < 3)
		length = 3;
	if (length > 4)
		length = 4;
	fFeedbackTransfer.packets[0].request_length = length;
	fFeedbackTransfer.packets[0].actual_length = 0;
	fFeedbackTransfer.packets[0].status = B_NO_INIT;
	fFeedbackTransfer.length = length;
	atomic_add(&fOutstanding, 1);
	status_t status = gUSBModule->queue_isochronous(fFeedbackEndpoint,
		fFeedbackData, length, fFeedbackTransfer.packets, 1, NULL,
		USB_ISO_ASAP, Stream::_FeedbackCallback, &fFeedbackTransfer);
	if (status != B_OK)
		atomic_add(&fOutstanding, -1);
	return status;
}


status_t
Stream::Start()
{
	if (fIsRunning)
		return B_OK;
	if (fDevice->IsRemoved())
		return B_DEV_NOT_READY;
	if (fArea < 0 || fConfiguration == NULL)
		return B_NO_INIT;

	usb_interface_info* interface
		= &fConfiguration->interface[fInterface].alt[
			ActiveAlternate()->Alternate()];
	status_t status = gUSBModule->set_alt_interface(fDevice->USBDevice(),
		interface);
	if (status != B_OK) {
		TRACE(ERR, "set_alt_interface %d: %s\n",
			(int)ActiveAlternate()->Alternate(), strerror(status));
		return status;
	}
	status = _FindPipes();
	if (status == B_OK)
		status = _SetRate();
	if (status == B_OK)
		status = _SetupTransfers();
	if (status != B_OK) {
		_FreeTransfers();
		return status;
	}

	cpu_status state = disable_interrupts();
	acquire_spinlock(&fLock);
	fRingPosition = 0;
	fCycle = fIsInput ? -1 : 0;
	fCycleTime = system_time();
	fReportedCycles = fCycles;
	release_spinlock(&fLock);
	restore_interrupts(state);

	fStopping = false;
	fIsRunning = true;

	// Fill all transfers before queueing the first: once one is queued,
	// its completion may run (and fill it again) at any time.
	if (!fIsInput) {
		for (uint32 i = 0; i < fTransferCount; i++)
			_FillTransfer(&fTransfers[i]);
	}
	for (uint32 i = 0; i < fTransferCount && status == B_OK; i++)
		status = _QueueTransfer(&fTransfers[i]);
	if (status == B_OK && !fIsInput && _QueueFeedback() != B_OK)
		TRACE(INF, "feedback endpoint not queued; nominal rate used\n");

	if (status != B_OK) {
		TRACE(ERR, "queue_isochronous: %s\n", strerror(status));
		Stop();
	}
	return status;
}


status_t
Stream::Stop()
{
	if (!fIsRunning && fTransferArea < 0)
		return B_OK;

	fStopping = true;
	if (!fDevice->IsRemoved()) {
		if (fStreamEndpoint != 0)
			gUSBModule->cancel_queued_transfers(fStreamEndpoint);
		if (fFeedbackEndpoint != 0)
			gUSBModule->cancel_queued_transfers(fFeedbackEndpoint);
	}
	// wait until possible notification handling finished...
	while (atomic_get(&fInsideNotify) != 0)
		snooze(100);
	bool drained = _WaitForCompletions(1000000);
	fIsRunning = false;

	if (!fDevice->IsRemoved() && fConfiguration != NULL) {
		// back to the zero bandwidth setting
		gUSBModule->set_alt_interface(fDevice->USBDevice(),
			&fConfiguration->interface[fInterface].alt[0]);
	}
	fStreamEndpoint = fFeedbackEndpoint = 0;
	if (drained)
		_FreeTransfers();
	else {
		// A completion is still to come: leaking the buffers beats
		// handing freed memory to its callback.
		TRACE(ERR, "%" B_PRId32 " transfer(s) never completed; their "
			"buffers are kept\n", atomic_get(&fOutstanding));
		_ForgetTransfers();
	}
	return B_OK;
}


/*!	Renga: waits until every queued transfer's callback has returned.
	fInsideNotify alone does not do: the stack takes a finished transfer
	off its queue before it calls the callback, so cancel_queued_transfers()
	does not see it and the callback may not have begun when Stop() looks
	(xhci's finisher thread; the uac2 test's mock bus has the same window).
	Freeing the transfers then hands freed memory to the callback.
*/
bool
Stream::_WaitForCompletions(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (atomic_get(&fOutstanding) > 0) {
		if (system_time() > deadline)
			return false;
		snooze(100);
	}
	return true;
}


void
Stream::_TransferCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	StreamTransfer* transfer = (StreamTransfer*)cookie;
	Stream* stream = transfer->stream;

	atomic_add(&stream->fInsideNotify, 1);
	if (status == B_CANCELED || stream->fStopping || !stream->fIsRunning
		|| stream->fDevice->IsRemoved()) {
		atomic_add(&stream->fInsideNotify, -1);
		atomic_add(&stream->fOutstanding, -1);
			// the last access to the stream
		return;
	}

	if (status != B_OK) {
		// Isochronous errors (a missed interval, a CRC error on capture)
		// lose a packet, not the stream: go on.
		TRACE(DTA, "transfer status %#x\n", status);
	}

	if (stream->fIsInput)
		stream->_DrainTransfer(transfer);
	else
		stream->_FillTransfer(transfer);

	status = stream->_QueueTransfer(transfer);
	if (status != B_OK)
		TRACE(ERR, "requeueing a transfer failed: %s\n", strerror(status));

	atomic_add(&stream->fInsideNotify, -1);
	atomic_add(&stream->fOutstanding, -1);
}


void
Stream::_FeedbackCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	StreamTransfer* transfer = (StreamTransfer*)cookie;
	Stream* stream = transfer->stream;

	atomic_add(&stream->fInsideNotify, 1);
	if (status == B_CANCELED || stream->fStopping || !stream->fIsRunning
		|| stream->fDevice->IsRemoved()) {
		atomic_add(&stream->fInsideNotify, -1);
		atomic_add(&stream->fOutstanding, -1);
			// the last access to the stream
		return;
	}

	usb_iso_packet_descriptor& packet = transfer->packets[0];
	if (status == B_OK && packet.status == B_OK) {
		uint32 interval = stream->ActiveAlternate()->Interval();
		if (interval < 1)
			interval = 1;
		uint32 unitsPerPacket = 1 << min_c(interval - 1, 15);
		uint32 value = ParseFeedback(stream->fFeedbackData,
			packet.actual_length, stream->fHighSpeed, stream->fNominalFrames,
			unitsPerPacket);
		if (value != 0)
			stream->fFeedbackFrames = value;
	}

	stream->_QueueFeedback();
	atomic_add(&stream->fInsideNotify, -1);
	atomic_add(&stream->fOutstanding, -1);
}


//	#pragma mark - hmulti


status_t
Stream::GetEnabledChannels(uint32& offset, multi_channel_enable* Enable)
{
	for (size_t i = 0; i < fChannels; i++) {
		B_SET_CHANNEL(Enable->enable_bits, offset, true);
		offset++;
	}
	return B_OK;
}


status_t
Stream::SetEnabledChannels(uint32& offset, multi_channel_enable* Enable)
{
	// All channels of a USB stream are always transferred.
	offset += fChannels;
	return B_OK;
}


status_t
Stream::GetGlobalFormat(multi_format_info* Format)
{
	_multi_format* format = fIsInput ? &Format->input : &Format->output;
	format->cvsr = fRate;
	format->rate = AudioStreamAlternate::SamplingRateId(fRate);
	format->format = ActiveAlternate()->FormatId();
	// What is queued on the bus at any time (see kTransferCount).
	bigtime_t latency = (bigtime_t)kTransferCount * kTransferMilliseconds
		* 1000;
	if (fIsInput)
		Format->input_latency = latency;
	else
		Format->output_latency = latency;
	return B_OK;
}


/*!	Sets rate and format. A format another alternate of the same channel
	count has switches to that alternate (16 and 24-bit settings, say). The
	stream stops; the next buffer exchange starts it again with the new
	settings, after the client has asked for the (new) buffers.
*/
status_t
Stream::SetGlobalFormat(multi_format_info* Format)
{
	_multi_format* format = fIsInput ? &Format->input : &Format->output;

	uint32 rate = format->rate != 0
		? AudioStreamAlternate::SamplingRateFromId(format->rate) : fRate;
	if (format->rate == B_SR_CVSR && format->cvsr > 0)
		rate = (uint32)format->cvsr;
	if (rate == 0)
		rate = fRate;

	int32 chosen = fActiveAlternate;
	AudioStreamAlternate* active = ActiveAlternate();
	if (format->format != 0 && format->format != active->FormatId()) {
		chosen = -1;
		for (int32 i = 0; i < fAlternates.Count(); i++) {
			AudioStreamAlternate* alternate = fAlternates[i];
			if (alternate->IsUsable() && alternate->IsInput() == fIsInput
				&& alternate->Channels() == active->Channels()
				&& alternate->FormatId() == format->format
				&& alternate->SupportsRate(rate)) {
				chosen = i;
				break;
			}
		}
		if (chosen < 0) {
			TRACE(INF, "format %#x at %u Hz is not available\n",
				(unsigned)format->format, (unsigned)rate);
			return B_BAD_VALUE;
		}
	}
	if (!fAlternates[chosen]->SupportsRate(rate)) {
		TRACE(INF, "%u Hz is not available\n", (unsigned)rate);
		return B_BAD_VALUE;
	}

	if (chosen == (int32)fActiveAlternate && rate == fRate)
		return B_OK;

	Stop();
	fActiveAlternate = chosen;
	fRate = rate;
	fSubslotSize = ActiveAlternate()->SubslotSize();
	fFrameSize = fChannels * fSubslotSize;
	TRACE(INF, "%s: %u Hz, format %#x (alternate %d)\n",
		fIsInput ? "input" : "output", (unsigned)fRate,
		(unsigned)ActiveAlternate()->FormatId(), (int)chosen);
	// The client asks for the new buffers (B_MULTI_GET_BUFFERS) next.
	ReleaseBuffers();
	return B_OK;
}


void
Stream::ReleaseBuffers()
{
	Stop();
	if (fArea >= 0)
		delete_area(fArea);
	if (fKernelArea >= 0)
		delete_area(fKernelArea);
	fArea = fKernelArea = -1;
	fAreaSize = 0;
	fBuffers = fKernelBuffers = NULL;
}


status_t
Stream::GetBuffers(multi_buffer_list* List)
{
	bool record = fIsInput;
	int32 requestFrames = record ? List->request_record_buffer_size
		: List->request_playback_buffer_size;
	uint32 frames = kSamplesBufferSize;
	if (requestFrames >= (int32)kMinBufferFrames
		&& requestFrames <= (int32)kMaxBufferFrames) {
		frames = requestFrames;
	}
	// A buffer has to last longer than a transfer, or the client gets two
	// buffers at once and has no time to fill one.
	uint32 transferFrames = fRate * kTransferMilliseconds / 1000 + 1;
	if (frames < 2 * transferFrames)
		frames = 2 * transferFrames;

	if (frames != fBufferFrames)
		Stop();
	fBufferFrames = frames;
	fBufferCount = kSamplesBufferCount;
	status_t status = _SetupBuffers();
	if (status != B_OK)
		return status;

	int32 startChannel;
	buffer_desc** buffers;
	if (record) {
		List->flags |= B_MULTI_BUFFER_RECORD;
		List->return_record_buffer_size = fBufferFrames;
		List->return_record_buffers = fBufferCount;
		startChannel = List->return_record_channels;
		buffers = List->record_buffers;
	} else {
		List->flags |= B_MULTI_BUFFER_PLAYBACK;
		List->return_playback_buffer_size = fBufferFrames;
		List->return_playback_buffers = fBufferCount;
		startChannel = List->return_playback_channels;
		buffers = List->playback_buffers;
	}

	int32 requestBuffers = record ? List->request_record_buffers
		: List->request_playback_buffers;
	int32 requestChannels = record ? List->request_record_channels
		: List->request_playback_channels;
	if (requestBuffers < (int32)fBufferCount
		|| requestChannels < startChannel + (int32)fChannels) {
		TRACE(ERR, "client asked for %d buffers of %d channels, needs %d "
			"of %d\n", (int)requestBuffers, (int)requestChannels,
			(int)fBufferCount, (int)(startChannel + fChannels));
		return B_BAD_VALUE;
	}

	// [buffer][channel]: each channel of a buffer starts at its sample of
	// the buffer's first frame; all share the frame stride.
	for (uint32 buffer = 0; buffer < fBufferCount; buffer++) {
		buffer_desc descs[8];
		for (uint32 channel = 0; channel < fChannels; channel++) {
			descs[channel].base = (char*)fBuffers
				+ (size_t)buffer * fBufferFrames * fRingFrameSize
				+ channel * fRingSampleSize;
			descs[channel].stride = fRingFrameSize;
		}
		if (!IS_USER_ADDRESS(buffers[buffer])
			|| user_memcpy(buffers[buffer] + startChannel, descs,
				sizeof(buffer_desc) * fChannels) < B_OK) {
			return B_BAD_ADDRESS;
		}
	}

	if (record)
		List->return_record_channels += fChannels;
	else
		List->return_playback_channels += fChannels;
	return B_OK;
}


/*!	Reports the stream's position: for playback the buffer now being sent
	(the client fills the one before it), for capture the buffer filled
	last, or -1 if no new one was filled since the last exchange.
*/
bool
Stream::ExchangeBuffer(multi_buffer_info* Info)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&fLock);

	if (fIsInput) {
		if (fCycles != fReportedCycles) {
			Info->record_buffer_cycle = fCycle;
			Info->recorded_frames_count = fCycleFrames;
			Info->recorded_real_time = fCycleTime;
			fReportedCycles = fCycles;
		} else
			Info->record_buffer_cycle = -1;
	} else {
		Info->playback_buffer_cycle = fCycle;
		Info->played_frames_count = fCycleFrames;
		Info->played_real_time = fCycleTime;
		fReportedCycles = fCycles;
	}

	release_spinlock(&fLock);
	restore_interrupts(state);
	return true;
}
