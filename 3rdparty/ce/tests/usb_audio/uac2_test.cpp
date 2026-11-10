/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	USB Audio Class 2 test of the usb_audio driver, without UAC2 hardware.

	QEMU's usb-audio device is UAC1 only (and playback only), so the parts
	of the driver that only UAC2 devices use cannot be tested in QEMU: clock
	entities, RANGE requests, the cluster in the AS interface, 3-byte
	subslots, high speed packet schedules, explicit feedback, capture. This
	program builds the driver's own sources (Driver.cpp, Device.cpp,
	AudioControlInterface.cpp, AudioStreamingInterface.cpp, Stream.cpp,
	Settings.cpp, unchanged; see shim/ for the few kernel functions it
	needs) into a userland program, and gives it a mock USB stack with one
	synthetic device: "Renga UAC2 Test DAC", a high speed Audio 2.0 device
	laid out as the specification's examples and common UAC2 DACs are:

	- a clock source (internal, programmable, 44.1 to 192 kHz in six
	  discrete subranges) behind a clock selector,
	- playback: USB streaming input terminal -> feature unit (master mute
	  and volume, volume per channel) -> speaker, streaming interface with
	  a 24-bit (3-byte subslot) and a 16-bit alternate, an asynchronous OUT
	  endpoint served every microframe and an explicit feedback endpoint
	  every millisecond,
	- capture: microphone -> feature unit -> USB streaming output
	  terminal, a 24-bit alternate, an asynchronous IN endpoint.

	The mock answers only the UAC2 forms of the class requests (CUR and
	RANGE, bRequest 0x01/0x02; a UAC1 GET_CUR stalls), checks every
	isochronous transfer as Haiku's xhci driver does (packet lengths, their
	sum), runs its endpoints in real time, plays and records a counter (each
	frame one more) so that any lost, repeated or misplaced frame shows, and
	runs its clock 1000 ppm fast, which its feedback endpoint reports.

	What it does not show: real host controller timing and real devices'
	quirks. Those need the maintainer's UAC2 hardware.
*/


#include <OS.h>

#include <hmulti_audio.h>
#include <module.h>
#include <USB3.h>
#include <usb/USB_audio.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "Driver.h"
#include "Stream.h"

#include "shim_dprintf.h"


#define SUITE "usb_audio_uac2"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


// #pragma mark - kernel functions the driver uses


extern "C" void
usb_audio_test_dprintf(const char* format, ...)
{
	// Not "usb_audio:" at the start, so that the harness does not take the
	// driver's messages here for the kernel driver's.
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	printf("CE-INFO uac2 driver says: %s", line);
	if (line[0] != '\0' && line[strlen(line) - 1] != '\n')
		printf("\n");
}


extern "C" status_t
user_memcpy(void* to, const void* from, size_t size)
{
	memcpy(to, from, size);
	return B_OK;
}


static pthread_mutex_t sSpinlocks = PTHREAD_MUTEX_INITIALIZER;


extern "C" cpu_status
disable_interrupts(void)
{
	return 0;
}


extern "C" void
restore_interrupts(cpu_status status)
{
}


extern "C" void
acquire_spinlock(spinlock* lock)
{
	pthread_mutex_lock(&sSpinlocks);
}


extern "C" void
release_spinlock(spinlock* lock)
{
	pthread_mutex_unlock(&sSpinlocks);
}


// #pragma mark - the device


enum {
	kInterfaceControl = 0,
	kInterfacePlayback = 1,
	kInterfaceRecord = 2,

	kClockSource = 0x29,
	kClockSelector = 0x28,
	kPlaybackTerminal = 0x02,
	kPlaybackFeature = 0x0a,
	kSpeaker = 0x14,
	kMicrophone = 0x0b,
	kRecordFeature = 0x0c,
	kRecordTerminal = 0x16,

	kPlaybackEndpoint = 0x01,
	kFeedbackEndpoint = 0x81,
	kRecordEndpoint = 0x82,
	kDataMaxPacket = 156,		// 26 frames of 6 bytes
	kDeviceID = 1
};

static const uint32 kRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
static const double kDeviceClockError = 1.001;	// 1000 ppm fast


struct Builder {
	std::vector<uint8> bytes;

	void Add(std::initializer_list<int> values)
	{
		for (int value : values)
			bytes.push_back((uint8)value);
	}
	void Add16(int value) { Add({ value & 0xff, (value >> 8) & 0xff }); }
	void Add32(uint32 value)
	{
		Add({ (int)(value & 0xff), (int)((value >> 8) & 0xff),
			(int)((value >> 16) & 0xff), (int)((value >> 24) & 0xff) });
	}
};


/*!	The configuration descriptor of the synthetic device (UAC2 4.6-4.10,
	Frmts20 2.3.1.6).
*/
static std::vector<uint8>
build_configuration()
{
	Builder b;
	// configuration (wTotalLength fixed up below)
	b.Add({ 9, 2, 0, 0, 3, 1, 0, 0x80, 50 });
	// interface association
	b.Add({ 8, 0x0b, 0, 3, 1, 0, 0x20, 0 });

	// AC interface 0
	b.Add({ 9, 4, kInterfaceControl, 0, 0, 1, 1, 0x20, 0 });
	size_t acHeader = b.bytes.size();
	b.Add({ 9, 0x24, 1 });
	b.Add16(0x0200);
	b.Add({ 0x0a });			// bCategory: pro audio
	b.Add16(0);					// wTotalLength, below
	b.Add({ 0 });				// bmControls
	// clock source: internal programmable, frequency programmable (11),
	// validity readable (01)
	b.Add({ 8, 0x24, 0x0a, kClockSource, 0x03, 0x07, 0, 0 });
	// clock selector with one input
	b.Add({ 8, 0x24, 0x0b, kClockSelector, 1, kClockSource, 0x03, 0 });
	// playback: USB streaming input terminal, 2 channels (L, R)
	b.Add({ 17, 0x24, 2, kPlaybackTerminal });
	b.Add16(0x0101);
	b.Add({ 0, kClockSelector, 2 });
	b.Add32(0x3);
	b.Add({ 0 });
	b.Add16(0);
	b.Add({ 0 });
	// feature unit: master mute + volume, volume on 1 and 2
	b.Add({ 18, 0x24, 6, kPlaybackFeature, kPlaybackTerminal });
	b.Add32(0x0f);
	b.Add32(0x0c);
	b.Add32(0x0c);
	b.Add({ 0 });
	// speaker
	b.Add({ 12, 0x24, 3, kSpeaker });
	b.Add16(0x0301);
	b.Add({ 0, kPlaybackFeature, kClockSelector });
	b.Add16(0);
	b.Add({ 0 });
	// capture: microphone, feature unit, USB streaming output terminal
	b.Add({ 17, 0x24, 2, kMicrophone });
	b.Add16(0x0201);
	b.Add({ 0, kClockSelector, 2 });
	b.Add32(0x3);
	b.Add({ 0 });
	b.Add16(0);
	b.Add({ 0 });
	b.Add({ 18, 0x24, 6, kRecordFeature, kMicrophone });
	b.Add32(0x0f);
	b.Add32(0x0c);
	b.Add32(0x0c);
	b.Add({ 0 });
	b.Add({ 12, 0x24, 3, kRecordTerminal });
	b.Add16(0x0101);
	b.Add({ 0, kRecordFeature, kClockSelector });
	b.Add16(0);
	b.Add({ 0 });
	size_t acLength = b.bytes.size() - acHeader;
	b.bytes[acHeader + 6] = acLength & 0xff;
	b.bytes[acHeader + 7] = acLength >> 8;

	// AS playback interface 1: alternate 0 (zero bandwidth), 1 (24-bit),
	// 2 (16-bit)
	b.Add({ 9, 4, kInterfacePlayback, 0, 0, 1, 2, 0x20, 0 });
	for (int alt = 1; alt <= 2; alt++) {
		b.Add({ 9, 4, kInterfacePlayback, alt, 2, 1, 2, 0x20, 0 });
		b.Add({ 16, 0x24, 1, kPlaybackTerminal, 0, 1 });
		b.Add32(1);				// PCM
		b.Add({ 2 });
		b.Add32(0x3);
		b.Add({ 0 });
		if (alt == 1)
			b.Add({ 6, 0x24, 2, 1, 3, 24 });
		else
			b.Add({ 6, 0x24, 2, 1, 2, 16 });
		// data endpoint: isochronous, asynchronous, every microframe
		b.Add({ 7, 5, kPlaybackEndpoint, 0x05 });
		b.Add16(kDataMaxPacket);
		b.Add({ 1 });
		b.Add({ 8, 0x25, 1, 0, 0, 0 });
		b.Add16(0);
		// explicit feedback endpoint, every millisecond (8 microframes)
		b.Add({ 7, 5, kFeedbackEndpoint, 0x11 });
		b.Add16(4);
		b.Add({ 4 });
	}

	// AS capture interface 2: alternate 0 and 1 (24-bit)
	b.Add({ 9, 4, kInterfaceRecord, 0, 0, 1, 2, 0x20, 0 });
	b.Add({ 9, 4, kInterfaceRecord, 1, 1, 1, 2, 0x20, 0 });
	b.Add({ 16, 0x24, 1, kRecordTerminal, 0, 1 });
	b.Add32(1);
	b.Add({ 2 });
	b.Add32(0x3);
	b.Add({ 0 });
	b.Add({ 6, 0x24, 2, 1, 3, 24 });
	b.Add({ 7, 5, kRecordEndpoint, 0x05 });
	b.Add16(kDataMaxPacket);
	b.Add({ 1 });
	b.Add({ 8, 0x25, 1, 0, 0, 0 });
	b.Add16(0);

	b.bytes[2] = b.bytes.size() & 0xff;
	b.bytes[3] = b.bytes.size() >> 8;
	return b.bytes;
}


// #pragma mark - the mock USB stack


struct MockTransfer {
	usb_pipe					pipe;
	uint8*						data;
	size_t						length;
	usb_iso_packet_descriptor*	packets;
	uint32						count;
	usb_callback_func			callback;
	void*						cookie;
	bigtime_t					due;
};

struct MockPipe {
	usb_pipe	handle;
	uint8		address;
	uint16		maxPacket;
	bigtime_t	packetTime;
	bigtime_t	nextFree;
};


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<uint8> sConfigurationData;
static usb_configuration_info sConfiguration;
static usb_device_descriptor sDeviceDescriptor;
static std::vector<MockPipe> sPipes;
static std::vector<MockTransfer> sTransfers;
static usb_pipe sNextPipe = 100;
static volatile bool sBusQuit = false;
static int32 sActiveAlternate[3];

// device state
static uint32 sClockRate = 48000;
static int32 sClockSets = 0;
static int32 sUAC1Requests = 0;		// class requests in UAC1 form
static int16 sVolume[2][3];			// [feature unit][channel]
static uint8 sMute[2];
static int32 sTransferViolations = 0;
static char sViolation[160];

// what the device received and sent
static std::vector<int32> sPlayed;			// left channel, sign-extended
static std::vector<int32> sPlayedRight;
static std::vector<uint32> sPacketFrames;
static uint32 sPlaySubslot = 3;
static uint32 sRecordCounter = 0;
static uint32 sRecordAccumulator = 0;
static uint32 sPlaybackPackets = 0;


static MockPipe*
find_pipe(usb_pipe handle)
{
	for (size_t i = 0; i < sPipes.size(); i++) {
		if (sPipes[i].handle == handle)
			return &sPipes[i];
	}
	return NULL;
}


static const usb_device_descriptor*
mock_get_device_descriptor(usb_device device)
{
	return &sDeviceDescriptor;
}


static const usb_configuration_info*
mock_get_configuration(usb_device device)
{
	return &sConfiguration;
}


static const usb_configuration_info*
mock_get_nth_configuration(usb_device device, uint32 index)
{
	return index == 0 ? &sConfiguration : NULL;
}


static status_t
mock_set_configuration(usb_device device,
	const usb_configuration_info* configuration)
{
	for (size_t i = 0; i < sConfiguration.interface_count; i++)
		sConfiguration.interface[i].active = sConfiguration.interface[i].alt;
	return B_OK;
}


static status_t
mock_set_alt_interface(usb_device device, const usb_interface_info* interface)
{
	pthread_mutex_lock(&sLock);
	for (size_t i = 0; i < sConfiguration.interface_count; i++) {
		usb_interface_list& list = sConfiguration.interface[i];
		for (size_t a = 0; a < list.alt_count; a++) {
			if (&list.alt[a] != interface)
				continue;
			list.active = &list.alt[a];
			if (i < 3)
				sActiveAlternate[i] = a;
			for (size_t e = 0; e < list.alt[a].endpoint_count; e++) {
				usb_endpoint_info& endpoint = list.alt[a].endpoint[e];
				MockPipe pipe;
				pipe.handle = endpoint.handle = sNextPipe++;
				pipe.address = endpoint.descr->endpoint_address;
				pipe.maxPacket = endpoint.descr->max_packet_size & 0x7ff;
				pipe.packetTime = 125 << (endpoint.descr->interval - 1);
				pipe.nextFree = 0;
				sPipes.push_back(pipe);
			}
		}
	}
	pthread_mutex_unlock(&sLock);
	return B_OK;
}


static status_t
mock_get_descriptor(usb_device device, uint8 type, uint8 index,
	uint16 languageID, void* data, size_t length, size_t* _actualLength)
{
	if (type != USB_DESCRIPTOR_STRING)
		return B_ERROR;
	uint8 buffer[128];
	if (index == 0) {
		uint8 languages[] = { 4, 3, 0x09, 0x04 };
		memcpy(buffer, languages, sizeof(languages));
	} else if (index == 2) {
		const char* name = "Renga UAC2 Test DAC";
		size_t count = strlen(name);
		buffer[0] = 2 + 2 * count;
		buffer[1] = 3;
		for (size_t i = 0; i < count; i++) {
			buffer[2 + 2 * i] = name[i];
			buffer[3 + 2 * i] = 0;
		}
	} else
		return B_ERROR;
	size_t copy = min_c(length, (size_t)buffer[0]);
	memcpy(data, buffer, copy);
	*_actualLength = copy;
	return B_OK;
}


/*!	The device's class requests. Only the UAC2 forms are answered: CUR
	(0x01) and RANGE (0x02) both ways, with the entity in the high byte of
	wIndex. UAC1 requests (0x81 GET_CUR and friends) stall, as on real UAC2
	devices.
*/
static status_t
mock_send_request(usb_device device, uint8 requestType, uint8 request,
	uint16 value, uint16 index, uint16 length, void* data,
	size_t* _actualLength)
{
	*_actualLength = 0;
	bool in = (requestType & USB_REQTYPE_DEVICE_IN) != 0;
	if ((requestType & 0x60) != USB_REQTYPE_CLASS)
		return B_DEV_STALLED;
	if ((requestType & 0x1f) != 0x01) {
		// endpoint requests (the UAC1 sampling frequency control)
		sUAC1Requests++;
		return B_DEV_STALLED;
	}
	if (request != 0x01 && request != 0x02) {
		sUAC1Requests++;
		return B_DEV_STALLED;
	}

	uint8 entity = index >> 8;
	uint8 selector = value >> 8;
	uint8 channel = value & 0xff;
	uint8* bytes = (uint8*)data;

	pthread_mutex_lock(&sLock);
	status_t status = B_DEV_STALLED;
	size_t actual = 0;

	if (entity == kClockSource && selector == 0x01) {
		if (request == 0x01 && in && length >= 4) {
			memcpy(bytes, &sClockRate, 4);
			actual = 4;
			status = B_OK;
		} else if (request == 0x01 && !in && length == 4) {
			uint32 rate;
			memcpy(&rate, bytes, 4);
			bool known = false;
			for (size_t i = 0; i < B_COUNT_OF(kRates); i++)
				known |= kRates[i] == rate;
			if (known) {
				sClockRate = rate;
				sClockSets++;
				actual = 4;
				status = B_OK;
			}
		} else if (request == 0x02 && in) {
			uint8 range[2 + 12 * B_COUNT_OF(kRates)];
			uint16 count = B_COUNT_OF(kRates);
			memcpy(range, &count, 2);
			for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
				uint32 triple[3] = { kRates[i], kRates[i], 0 };
				memcpy(range + 2 + 12 * i, triple, 12);
			}
			actual = min_c((size_t)length, sizeof(range));
			memcpy(bytes, range, actual);
			status = B_OK;
		}
	} else if (entity == kClockSource && selector == 0x02 && request == 0x01
		&& in && length >= 1) {
		bytes[0] = 1;
		actual = 1;
		status = B_OK;
	} else if (entity == kClockSelector && selector == 0x01 && request == 0x01
		&& in && length >= 1) {
		bytes[0] = 1;
		actual = 1;
		status = B_OK;
	} else if ((entity == kPlaybackFeature || entity == kRecordFeature)
		&& channel <= 2) {
		int unit = entity == kPlaybackFeature ? 0 : 1;
		if (selector == 0x01 && channel == 0 && request == 0x01 && length >= 1) {
			if (in)
				bytes[0] = sMute[unit];
			else
				sMute[unit] = bytes[0];
			actual = 1;
			status = B_OK;
		} else if (selector == 0x02 && request == 0x01 && length >= 2) {
			if (in)
				memcpy(bytes, &sVolume[unit][channel], 2);
			else
				memcpy(&sVolume[unit][channel], bytes, 2);
			actual = 2;
			status = B_OK;
		} else if (selector == 0x02 && request == 0x02 && in && length >= 8) {
			uint16 count = 1;
			int16 low = -100 * 256, high = 0;
			uint16 step = 128;
			memcpy(bytes, &count, 2);
			memcpy(bytes + 2, &low, 2);
			memcpy(bytes + 4, &high, 2);
			memcpy(bytes + 6, &step, 2);
			actual = 8;
			status = B_OK;
		}
	}
	pthread_mutex_unlock(&sLock);
	*_actualLength = actual;
	return status;
}


static status_t
mock_queue_isochronous(usb_pipe pipe, void* data, size_t dataLength,
	usb_iso_packet_descriptor* packets, uint32 packetCount,
	uint32* startingFrameNumber, uint32 flags, usb_callback_func callback,
	void* cookie)
{
	pthread_mutex_lock(&sLock);
	MockPipe* mockPipe = find_pipe(pipe);
	if (mockPipe == NULL) {
		pthread_mutex_unlock(&sLock);
		return B_BAD_VALUE;
	}
	// what Haiku's xhci driver checks
	size_t total = 0;
	for (uint32 i = 0; i < packetCount; i++) {
		if (packets[i].request_length <= 0
			|| packets[i].request_length > mockPipe->maxPacket) {
			sTransferViolations++;
			snprintf(sViolation, sizeof(sViolation), "packet %u of %d bytes "
				"on endpoint %#x (max %u)", (unsigned)i,
				packets[i].request_length, mockPipe->address,
				mockPipe->maxPacket);
			pthread_mutex_unlock(&sLock);
			return B_BAD_VALUE;
		}
		total += packets[i].request_length;
	}
	if (total != dataLength) {
		sTransferViolations++;
		snprintf(sViolation, sizeof(sViolation), "%zu bytes in packets, "
			"%zu in the transfer", total, dataLength);
		pthread_mutex_unlock(&sLock);
		return B_BAD_VALUE;
	}

	bigtime_t now = system_time();
	bigtime_t start = max_c(now, mockPipe->nextFree);
	MockTransfer transfer = { pipe, (uint8*)data, dataLength, packets,
		packetCount, callback, cookie,
		start + mockPipe->packetTime * packetCount };
	mockPipe->nextFree = transfer.due;
	sTransfers.push_back(transfer);
	pthread_mutex_unlock(&sLock);
	return B_OK;
}


static status_t
mock_cancel_queued_transfers(usb_pipe pipe)
{
	std::vector<MockTransfer> canceled;
	pthread_mutex_lock(&sLock);
	for (size_t i = 0; i < sTransfers.size();) {
		if (sTransfers[i].pipe == pipe) {
			canceled.push_back(sTransfers[i]);
			sTransfers.erase(sTransfers.begin() + i);
		} else
			i++;
	}
	MockPipe* mockPipe = find_pipe(pipe);
	if (mockPipe != NULL)
		mockPipe->nextFree = 0;
	pthread_mutex_unlock(&sLock);
	for (size_t i = 0; i < canceled.size(); i++)
		canceled[i].callback(canceled[i].cookie, B_CANCELED, NULL, 0);
	return B_OK;
}


static int32
sign_extend24(uint32 value)
{
	return (int32)(value << 8) >> 8;
}


/*!	What the device does with a transfer that is due: take the played
	frames, deliver recorded ones (as many per packet as its clock, 1000
	ppm fast, made), or report its feedback.
*/
static void
run_transfer(MockTransfer& transfer, MockPipe& pipe)
{
	size_t offset = 0;
	for (uint32 p = 0; p < transfer.count; p++) {
		usb_iso_packet_descriptor& packet = transfer.packets[p];
		uint8* bytes = transfer.data + offset;
		offset += packet.request_length;
		packet.status = B_OK;

		if (pipe.address == kPlaybackEndpoint) {
			uint32 frameSize = sPlaySubslot * 2;
			uint32 frames = packet.request_length / frameSize;
			if (packet.request_length % frameSize != 0) {
				sTransferViolations++;
				snprintf(sViolation, sizeof(sViolation),
					"packet of %d bytes is no whole number of frames",
					packet.request_length);
			}
			sPacketFrames.push_back(frames);
			sPlaybackPackets++;
			for (uint32 f = 0; f < frames; f++) {
				int32 sample[2];
				for (int c = 0; c < 2; c++) {
					const uint8* s = bytes + f * frameSize + c * sPlaySubslot;
					if (sPlaySubslot == 3)
						sample[c] = sign_extend24(s[0] | s[1] << 8 | s[2] << 16);
					else
						sample[c] = (int16)(s[0] | s[1] << 8);
				}
				sPlayed.push_back(sample[0]);
				sPlayedRight.push_back(sample[1]);
			}
			packet.actual_length = packet.request_length;
		} else if (pipe.address == kRecordEndpoint) {
			// 16.16 frames per microframe at the device's (fast) clock
			uint32 rate = (uint32)(sClockRate * kDeviceClockError * 65536.0
				/ 8000);
			sRecordAccumulator += rate;
			uint32 frames = sRecordAccumulator >> 16;
			sRecordAccumulator &= 0xffff;
			frames = min_c(frames, (uint32)packet.request_length / 6);
			for (uint32 f = 0; f < frames; f++) {
				uint32 left = (sRecordCounter * 2) & 0xffffff;
				uint32 right = (sRecordCounter * 2 + 1) & 0xffffff;
				uint8* d = bytes + f * 6;
				d[0] = left; d[1] = left >> 8; d[2] = left >> 16;
				d[3] = right; d[4] = right >> 8; d[5] = right >> 16;
				sRecordCounter++;
			}
			packet.actual_length = frames * 6;
		} else if (pipe.address == kFeedbackEndpoint) {
			// UAC2 / USB 2.0 5.12.4.2: 16.16 frames per microframe
			uint32 value = (uint32)(sClockRate * kDeviceClockError * 65536.0
				/ 8000);
			memcpy(bytes, &value, 4);
			packet.actual_length = 4;
		}
	}
}


static void*
bus_thread(void*)
{
	while (!sBusQuit) {
		snooze(250);
		bigtime_t now = system_time();
		std::vector<MockTransfer> done;
		pthread_mutex_lock(&sLock);
		for (size_t i = 0; i < sTransfers.size();) {
			if (sTransfers[i].due <= now) {
				MockPipe* pipe = find_pipe(sTransfers[i].pipe);
				if (pipe != NULL)
					run_transfer(sTransfers[i], *pipe);
				done.push_back(sTransfers[i]);
				sTransfers.erase(sTransfers.begin() + i);
			} else
				i++;
		}
		pthread_mutex_unlock(&sLock);
		// one completion at a time, as the host controller's finisher
		// thread does
		for (size_t i = 0; i < done.size(); i++) {
			size_t actual = 0;
			for (uint32 p = 0; p < done[i].count; p++)
				actual += done[i].packets[p].actual_length;
			done[i].callback(done[i].cookie, B_OK, done[i].data, actual);
		}
	}
	return NULL;
}


static status_t
mock_usb_ioctl(uint32 opcode, void* buffer, size_t size)
{
	if (opcode == 'DSPD' && size >= sizeof(uint32)) {
		*(uint32*)buffer = 2;	// high speed
		return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
mock_register_driver(const char* name, const usb_support_descriptor* d,
	size_t count, const char* republish)
{
	return B_OK;
}


static status_t
mock_install_notify(const char* name, const usb_notify_hooks* hooks)
{
	return B_OK;
}


static status_t
mock_uninstall_notify(const char* name)
{
	return B_OK;
}


static usb_module_info sModule;


extern "C" status_t
get_module(const char* path, module_info** _info)
{
	*_info = (module_info*)&sModule;
	return B_OK;
}


extern "C" status_t
put_module(const char* path)
{
	return B_OK;
}


/*!	Turns the configuration descriptor into usb_configuration_info the way
	Haiku's USB stack does (bus_managers/usb/Device.cpp): every interface
	descriptor starts an alternate of its interface, endpoints belong to the
	last one, anything else is one of its "generic" descriptors.
*/
static void
parse_configuration()
{
	sConfigurationData = build_configuration();
	uint8* data = sConfigurationData.data();
	sConfiguration.descr = (usb_configuration_descriptor*)data;
	sConfiguration.interface_count = data[4];
	sConfiguration.interface = (usb_interface_list*)calloc(data[4],
		sizeof(usb_interface_list));

	usb_interface_info* current = NULL;
	for (size_t offset = data[0]; offset < sConfigurationData.size();
			offset += data[offset]) {
		uint8 type = data[offset + 1];
		if (type == 4) {
			usb_interface_descriptor* descriptor
				= (usb_interface_descriptor*)(data + offset);
			usb_interface_list& list
				= sConfiguration.interface[descriptor->interface_number];
			list.alt = (usb_interface_info*)realloc(list.alt,
				(list.alt_count + 1) * sizeof(usb_interface_info));
			current = &list.alt[list.alt_count++];
			memset(current, 0, sizeof(*current));
			current->descr = descriptor;
			list.active = list.alt;
		} else if (type == 5 && current != NULL) {
			current->endpoint = (usb_endpoint_info*)realloc(current->endpoint,
				(current->endpoint_count + 1) * sizeof(usb_endpoint_info));
			usb_endpoint_info& endpoint
				= current->endpoint[current->endpoint_count++];
			endpoint.descr = (usb_endpoint_descriptor*)(data + offset);
			endpoint.handle = 0;
		} else if (type != 0x0b && current != NULL) {
			current->generic = (usb_descriptor**)realloc(current->generic,
				(current->generic_count + 1) * sizeof(usb_descriptor*));
			current->generic[current->generic_count++]
				= (usb_descriptor*)(data + offset);
		}
	}
	// list.active pointers moved with realloc
	for (size_t i = 0; i < sConfiguration.interface_count; i++)
		sConfiguration.interface[i].active = sConfiguration.interface[i].alt;
}


static void
setup_mock()
{
	memset(&sModule, 0, sizeof(sModule));
	sModule.get_device_descriptor = mock_get_device_descriptor;
	sModule.get_nth_configuration = mock_get_nth_configuration;
	sModule.get_configuration = mock_get_configuration;
	sModule.set_configuration = mock_set_configuration;
	sModule.set_alt_interface = mock_set_alt_interface;
	sModule.get_descriptor = mock_get_descriptor;
	sModule.send_request = mock_send_request;
	sModule.queue_isochronous = mock_queue_isochronous;
	sModule.cancel_queued_transfers = mock_cancel_queued_transfers;
	sModule.register_driver = mock_register_driver;
	sModule.install_notify = mock_install_notify;
	sModule.uninstall_notify = mock_uninstall_notify;
	sModule.usb_ioctl = mock_usb_ioctl;

	memset(&sDeviceDescriptor, 0, sizeof(sDeviceDescriptor));
	sDeviceDescriptor.length = 18;
	sDeviceDescriptor.descriptor_type = 1;
	sDeviceDescriptor.usb_version = 0x0200;
	sDeviceDescriptor.device_class = 0xef;
	sDeviceDescriptor.vendor_id = 0x1209;
	sDeviceDescriptor.product_id = 0x0001;
	sDeviceDescriptor.product = 2;
	sDeviceDescriptor.num_configurations = 1;

	parse_configuration();
}


// #pragma mark - the hmulti client


static device_hooks* sHooks;
static void* sCookie;


static status_t
control(uint32 op, void* data, size_t length)
{
	return sHooks->control(sCookie, op, data, length);
}


struct Client {
	multi_buffer_list	list;
	buffer_desc			playList[32 * 8];
	buffer_desc			recordList[32 * 8];
	buffer_desc*		play[32];
	buffer_desc*		record[32];
	uint32				playFormat;
	uint64				playFrame;		// next counter value to write
	std::vector<int32>	recorded;		// left channel, 24-bit value
	std::vector<int32>	recordedRight;
};


static status_t
get_buffers(Client& client, bool record)
{
	for (int i = 0; i < 32; i++) {
		client.play[i] = &client.playList[i * 8];
		client.record[i] = &client.recordList[i * 8];
	}
	memset(&client.list, 0, sizeof(client.list));
	client.list.info_size = sizeof(client.list);
	client.list.request_playback_buffers = 32;
	client.list.request_playback_channels = 2;
	client.list.playback_buffers = client.play;
	client.list.request_record_buffers = record ? 32 : 0;
	client.list.request_record_channels = record ? 2 : 0;
	client.list.record_buffers = record ? client.record : NULL;
	return control(B_MULTI_GET_BUFFERS, &client.list, sizeof(client.list));
}


/*!	Fills playback buffer \a index with the counter: frame n carries 2n on
	the left and 2n + 1 on the right, as 24-bit samples left-justified in
	32 bits (B_FMT_24BIT, what the multi_audio add-on writes) or as 16-bit
	samples.
*/
static void
fill(Client& client, int32 index)
{
	buffer_desc* desc = client.play[index];
	for (uint32 f = 0; f < (uint32)client.list.return_playback_buffer_size;
			f++) {
		uint64 n = client.playFrame++;
		for (int c = 0; c < 2; c++) {
			uint32 value = (uint32)(n * 2 + c);
			char* sample = desc[c].base + f * desc[c].stride;
			if (client.playFormat == B_FMT_16BIT)
				*(int16*)sample = (int16)(value & 0x7fff);
			else
				*(int32*)sample = (int32)((value & 0x7fffff) << 8);
		}
	}
}


static void
collect(Client& client, int32 index)
{
	buffer_desc* desc = client.record[index];
	for (uint32 f = 0; f < (uint32)client.list.return_record_buffer_size;
			f++) {
		int32 left = *(int32*)(desc[0].base + f * desc[0].stride);
		int32 right = *(int32*)(desc[1].base + f * desc[1].stride);
		client.recorded.push_back((uint32)left >> 8);
		client.recordedRight.push_back((uint32)right >> 8);
	}
}


struct RunResult {
	int32		exchanges;
	int32		badCycles;
	int32		recordBuffers;
	double		ratio;
	status_t	error;
};


static RunResult
run(Client& client, double seconds, uint32 rate, bool record)
{
	RunResult result = { 0, 0, 0, 0, B_OK };
	int32 count = client.list.return_playback_buffers;
	for (int32 i = 0; i < count; i++)
		fill(client, i);

	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	int32 lastCycle = -1;
	int64 firstFrames = -1, lastFrames = 0;
	bigtime_t firstTime = 0, lastTime = 0;
	bigtime_t end = system_time() + (bigtime_t)(seconds * 1000000);

	while (system_time() < end) {
		status_t status = control(B_MULTI_BUFFER_EXCHANGE, &info,
			sizeof(info));
		if (status != B_OK) {
			result.error = status;
			break;
		}
		result.exchanges++;
		int32 cycle = info.playback_buffer_cycle;
		if (cycle < 0 || cycle >= count
			|| (lastCycle >= 0 && cycle != (lastCycle + 1) % count)) {
			result.badCycles++;
		}
		lastCycle = cycle;
		if (firstFrames < 0) {
			firstFrames = info.played_frames_count;
			firstTime = info.played_real_time;
		}
		lastFrames = info.played_frames_count;
		lastTime = info.played_real_time;
		fill(client, (cycle - 1 + count) % count);
		if (record && info.record_buffer_cycle >= 0) {
			collect(client, info.record_buffer_cycle);
			result.recordBuffers++;
		}
	}
	double elapsed = (lastTime - firstTime) / 1000000.0;
	result.ratio = elapsed > 0
		? (lastFrames - firstFrames) / elapsed / rate : 0;
	return result;
}


static status_t
set_format(uint32 outputRate, uint32 outputFormat, uint32 inputRate,
	multi_format_info* _info = NULL)
{
	multi_format_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	info.output.rate = outputRate;
	info.output.format = outputFormat;
	info.input.rate = inputRate;
	info.input.format = B_FMT_24BIT;
	status_t status = control(B_MULTI_SET_GLOBAL_FORMAT, &info, sizeof(info));
	if (_info != NULL)
		*_info = info;
	return status;
}


/*!	Checks that the device received the counter without a lost, repeated or
	misplaced frame: from where the client's tone started (the first frame
	that is 0), every frame one more than the one before, on both channels.
*/
static void
check_played(const char* name, uint32 mask)
{
	pthread_mutex_lock(&sLock);
	size_t start = 0;
	while (start < sPlayed.size() && sPlayed[start] != 0)
		start++;
	size_t bad = 0;
	size_t frames = sPlayed.size() - start;
	for (size_t i = start; i < sPlayed.size(); i++) {
		uint32 n = i - start;
		if ((uint32)sPlayed[i] != ((n * 2) & mask)
			|| (uint32)sPlayedRight[i] != ((n * 2 + 1) & mask)) {
			bad++;
		}
	}
	pthread_mutex_unlock(&sLock);
	report(name, frames > 1000 && bad == 0, "%zu frames from frame 0, %zu "
		"out of sequence", frames, bad);
}


//!	The packet sizes after the first 100 ms: only the two neighbours of the
//!	average, and the average the device's feedback asked for.
static void
check_packets(const char* name, double wanted)
{
	pthread_mutex_lock(&sLock);
	size_t skip = min_c(sPacketFrames.size(), (size_t)800);
	uint32 low = ~0u, high = 0;
	uint64 total = 0;
	for (size_t i = skip; i < sPacketFrames.size(); i++) {
		low = min_c(low, sPacketFrames[i]);
		high = max_c(high, sPacketFrames[i]);
		total += sPacketFrames[i];
	}
	size_t count = sPacketFrames.size() - skip;
	pthread_mutex_unlock(&sLock);
	double average = count > 0 ? (double)total / count : 0;
	report(name, count > 2000 && high - low <= 1 && low == (uint32)wanted
			&& fabs(average - wanted) < 0.002 * wanted,
		"%zu packets of %u to %u frames, %.4f on average (feedback asks for "
		"%.4f, nominal %.4f)", count, low, high, average, wanted,
		wanted / kDeviceClockError);
}


static void
reset_device_log()
{
	pthread_mutex_lock(&sLock);
	sPlayed.clear();
	sPlayedRight.clear();
	sPacketFrames.clear();
	sPlaybackPackets = 0;
	pthread_mutex_unlock(&sLock);
}


static void
check_recorded(const char* name, Client& client)
{
	size_t bad = 0;
	size_t frames = client.recorded.size();
	// The ring starts with silence until the first packets arrive.
	size_t start = 0;
	while (start < frames && client.recorded[start] == 0
		&& client.recordedRight[start] == 0)
		start++;
	for (size_t i = start + 1; i < frames; i++) {
		if (((client.recorded[i] - client.recorded[i - 1]) & 0xffffff) != 2
			|| client.recordedRight[i] != ((client.recorded[i] + 1)
				& 0xffffff)) {
			bad++;
		}
	}
	report(name, frames - start > 1000 && bad == 0, "%zu frames recorded, "
		"%zu out of sequence", frames - start, bad);
}


// #pragma mark - force stop, removal


struct Stopper {
	bigtime_t	delay;
	bigtime_t	at;
	bool		remove;
	void*		deviceCookie;
};


static void*
stopper(void* data)
{
	Stopper* stop = (Stopper*)data;
	snooze(stop->delay);
	__atomic_store_n(&stop->at, system_time(), __ATOMIC_SEQ_CST);
	if (stop->remove)
		usb_audio_device_removed(stop->deviceCookie);
	else
		control(B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
	return NULL;
}


/*!	Exchanges until one fails. With \a stop, an exchange that began after
	the stop and succeeded means the stop came between two exchanges, not
	during the wait: the hmulti protocol then restarts the streams (the hda
	driver does the same), so this returns B_INTERRUPTED for the caller to
	try again.
*/
static status_t
exchange_until_error(Client& client, bigtime_t& returnedAt,
	Stopper* stop = NULL)
{
	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	int32 count = client.list.return_playback_buffers;
	bigtime_t end = system_time() + 5000000;
	status_t status = B_OK;
	while (system_time() < end) {
		bigtime_t began = system_time();
		status = control(B_MULTI_BUFFER_EXCHANGE, &info, sizeof(info));
		if (status != B_OK)
			break;
		bigtime_t stoppedAt = stop != NULL
			? __atomic_load_n(&stop->at, __ATOMIC_SEQ_CST) : 0;
		if (stoppedAt != 0 && began > stoppedAt) {
			status = B_INTERRUPTED;
			break;
		}
		fill(client, (info.playback_buffer_cycle - 1 + count) % count);
	}
	returnedAt = system_time();
	return status;
}


// #pragma mark -


static void
test_packet_math()
{
	// 44.1 kHz at full speed: 44 and 45 frames, 44100 every second.
	uint32 accumulator = 0;
	uint32 rate = Stream::NominalFramesPerPacket(44100, 1000);
	uint32 total = 0, low = ~0u, high = 0;
	for (int i = 0; i < 1000; i++) {
		uint32 frames = Stream::NextPacketFrames(accumulator, rate);
		total += frames;
		low = min_c(low, frames);
		high = max_c(high, frames);
	}
	report("packet_math.44k1_full_speed", total >= 44099 && total <= 44100
			&& low == 44 && high == 45,
		"%u frames in 1000 packets of %u to %u", (unsigned)total,
		(unsigned)low, (unsigned)high);

	// 11.025 kHz at high speed, one packet every microframe
	accumulator = 0;
	rate = Stream::NominalFramesPerPacket(11025, 8000);
	total = 0;
	for (int i = 0; i < 8000; i++)
		total += Stream::NextPacketFrames(accumulator, rate);
	report("packet_math.11k_high_speed", total >= 11024 && total <= 11025,
		"%u frames in 8000 packets", (unsigned)total);

	// Feedback: full speed 10.14 frames per frame, high speed 16.16 frames
	// per microframe, and a full speed value sent at high speed.
	uint32 nominal = Stream::NominalFramesPerPacket(48000, 1000);
	uint8 fs[3] = { 0x80, 0x00, 0x0c };	// 48.0078 frames per ms in 10.14
	uint32 value = Stream::ParseFeedback(fs, 3, false, nominal, 1);
	report("feedback.full_speed", fabs(value / 65536.0 - 48.0078) < 0.001,
		"10.14 value read as %.4f frames per packet", value / 65536.0);

	nominal = Stream::NominalFramesPerPacket(48000, 8000);
	uint32 hsValue = (uint32)(6.006 * 65536);
	uint8 hs[4] = { (uint8)hsValue, (uint8)(hsValue >> 8),
		(uint8)(hsValue >> 16), (uint8)(hsValue >> 24) };
	value = Stream::ParseFeedback(hs, 4, true, nominal, 1);
	report("feedback.high_speed", fabs(value / 65536.0 - 6.006) < 0.001,
		"16.16 value read as %.4f frames per packet", value / 65536.0);

	uint32 wrong = (uint32)(6.006 * 16384);	// 10.14 at high speed
	uint8 hs1014[4] = { (uint8)wrong, (uint8)(wrong >> 8),
		(uint8)(wrong >> 16), 0 };
	value = Stream::ParseFeedback(hs1014, 4, true, nominal, 1);
	report("feedback.high_speed_10_14", fabs(value / 65536.0 - 6.006) < 0.001,
		"10.14 value at high speed read as %.4f", value / 65536.0);

	uint8 nonsense[4] = { 0, 0, 0x40, 0 };
	value = Stream::ParseFeedback(nonsense, 4, true, nominal, 1);
	report("feedback.ignores_nonsense", value == 0,
		"a value 170 times too large %s", value == 0 ? "is ignored"
			: "was taken");
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_packet_math();

	setup_mock();
	pthread_t bus;
	pthread_create(&bus, NULL, bus_thread, NULL);

	init_driver();
	void* deviceCookie = NULL;
	status_t status = usb_audio_device_added(kDeviceID, &deviceCookie);
	report("device_added", status == B_OK, "%s", strerror(status));
	const char** names = publish_devices();
	report("published", status == B_OK && names[0] != NULL
			&& strcmp(names[0], "audio/hmulti/usb/1") == 0,
		"%s", names[0] != NULL ? names[0] : "nothing");
	if (status != B_OK || names[0] == NULL) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	char name[64];
	strlcpy(name, names[0], sizeof(name));
	sHooks = find_device(name);
	status = sHooks->open(name, O_RDWR, &sCookie);
	report("open", status == B_OK, "%s", strerror(status));

	multi_channel_info channels[16];
	multi_description description;
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = 16;
	description.channels = channels;
	status = control(B_MULTI_GET_DESCRIPTION, &description,
		sizeof(description));
	uint32 wantedRates = B_SR_44100 | B_SR_48000 | B_SR_88200 | B_SR_96000
		| B_SR_176400 | B_SR_192000;
	report("description", status == B_OK
			&& strcmp(description.friendly_name, "Renga UAC2 Test DAC") == 0
			&& description.output_channel_count == 2
			&& description.input_channel_count == 2
			&& description.output_rates == wantedRates
			&& description.input_rates == wantedRates
			&& description.output_formats == (B_FMT_24BIT | B_FMT_16BIT)
			&& description.input_formats == B_FMT_24BIT
			&& (channels[0].designations & B_CHANNEL_LEFT) != 0
			&& (channels[1].designations & B_CHANNEL_RIGHT) != 0
			&& channels[2].kind == B_MULTI_INPUT_CHANNEL,
		"'%s' out %d in %d, rates %#x/%#x, formats %#x/%#x",
		description.friendly_name, (int)description.output_channel_count,
		(int)description.input_channel_count,
		(unsigned)description.output_rates, (unsigned)description.input_rates,
		(unsigned)description.output_formats,
		(unsigned)description.input_formats);
	report("rates_from_clock_range", description.output_rates == wantedRates,
		"the six rates of the clock's RANGE answer");

	// Mix controls: two feature units, mute and volume in UAC2 requests.
	multi_mix_control controls[64];
	multi_mix_control_info mixInfo;
	memset(&mixInfo, 0, sizeof(mixInfo));
	mixInfo.info_size = sizeof(mixInfo);
	mixInfo.control_count = 64;
	mixInfo.controls = controls;
	status = control(B_MULTI_LIST_MIX_CONTROLS, &mixInfo, sizeof(mixInfo));
	int32 gains = 0, mutes = 0, firstGain = -1, firstMute = -1;
	bool rangeOk = true;
	for (int32 i = 0; i < mixInfo.control_count; i++) {
		if ((controls[i].flags & B_MULTI_MIX_GAIN) != 0) {
			if (firstGain < 0)
				firstGain = i;
			gains++;
			rangeOk &= controls[i].gain.min_gain == -100
				&& controls[i].gain.max_gain == 0
				&& controls[i].gain.granularity == 0.5;
		}
		if ((controls[i].flags & B_MULTI_MIX_ENABLE) != 0
			&& controls[i].string == S_MUTE) {
			if (firstMute < 0)
				firstMute = i;
			mutes++;
		}
	}
	report("mix_controls", status == B_OK && gains >= 4 && mutes == 2
			&& rangeOk,
		"%d controls: %d gains, %d mutes, ranges %s", (int)mixInfo.control_count,
		(int)gains, (int)mutes, rangeOk ? "-100..0 dB step 0.5 (RANGE)"
			: "wrong");
	report("uac2_requests_only", sUAC1Requests == 0, "%d request(s) in UAC1 "
		"form", (int)sUAC1Requests);

	if (firstGain >= 0 && firstMute >= 0) {
		multi_mix_value values[2];
		memset(values, 0, sizeof(values));
		values[0].id = controls[firstGain].id;
		values[0].gain = -20.5;
		values[1].id = controls[firstMute].id;
		values[1].enable = true;
		multi_mix_value_info valueInfo;
		memset(&valueInfo, 0, sizeof(valueInfo));
		valueInfo.info_size = sizeof(valueInfo);
		valueInfo.item_count = 2;
		valueInfo.values = values;
		status = control(B_MULTI_SET_MIX, &valueInfo, sizeof(valueInfo));
		bool set = false;
		for (int unit = 0; unit < 2; unit++) {
			for (int c = 0; c < 3; c++)
				set |= sVolume[unit][c] == -20.5 * 256;
		}
		bool muted = sMute[0] != 0 || sMute[1] != 0;
		memset(values, 0, sizeof(values));
		values[0].id = controls[firstGain].id;
		values[1].id = controls[firstMute].id;
		status_t getStatus = control(B_MULTI_GET_MIX, &valueInfo,
			sizeof(valueInfo));
		report("mix_set_get", status == B_OK && getStatus == B_OK && set
				&& muted && values[0].gain == -20.5f && values[1].enable,
			"set: device volume %s, mute %s; read back %.2f dB, mute %d",
			set ? "-20.5 dB" : "unchanged", muted ? "on" : "off",
			values[0].gain, values[1].enable);
		values[1].enable = false;
		valueInfo.item_count = 2;
		values[0].gain = 0;
		control(B_MULTI_SET_MIX, &valueInfo, sizeof(valueInfo));
	}

	// 44.1 kHz, 24-bit: a fractional packet schedule at high speed, 3-byte
	// subslots, feedback, capture at the same clock.
	multi_format_info format;
	status = set_format(B_SR_44100, B_FMT_24BIT, B_SR_48000, &format);
	report("format_44k1_24bit", status == B_OK
			&& format.input.rate == B_SR_44100,
		"%s; the input follows the output on the shared clock (input %#x)",
		strerror(status), (unsigned)format.input.rate);

	Client client;
	client.playFormat = B_FMT_24BIT;
	client.playFrame = 0;
	status = get_buffers(client, true);
	report("buffers", status == B_OK
			&& client.list.return_playback_channels == 2
			&& client.list.return_record_channels == 2
			&& client.play[0][0].stride == 8 && client.record[0][0].stride == 8,
		"%s: %d x %d frames, play stride %d, record stride %d",
		strerror(status), (int)client.list.return_playback_buffers,
		(int)client.list.return_playback_buffer_size,
		(int)client.play[0][0].stride, (int)client.record[0][0].stride);

	reset_device_log();
	sClockSets = 0;
	RunResult result = run(client, 2.0, 44100, true);
	report("play_44k1.exchange", result.error == B_OK && result.badCycles == 0
			&& result.ratio > 0.95 && result.ratio < 1.05,
		"%d exchanges, %d bad cycles, pacing %.3f (%s)", (int)result.exchanges,
		(int)result.badCycles, result.ratio, strerror(result.error));
	report("clock_set", sClockRate == 44100 && sClockSets >= 1,
		"clock at %u Hz after %d SET CUR", (unsigned)sClockRate,
		(int)sClockSets);
	report("alternates", sActiveAlternate[kInterfacePlayback] == 1
			&& sActiveAlternate[kInterfaceRecord] == 1,
		"playback alternate %d, capture alternate %d (24-bit ones)",
		(int)sActiveAlternate[kInterfacePlayback],
		(int)sActiveAlternate[kInterfaceRecord]);
	check_played("play_44k1.frames_24bit", 0xffffff);
	check_packets("play_44k1.packets", 44100.0 / 8000 * kDeviceClockError);
	check_recorded("record_44k1.frames_24bit", client);
	report("transfers_valid", sTransferViolations == 0, "%d violation(s)%s%s",
		(int)sTransferViolations, sTransferViolations ? ": " : "", sViolation);

	// 48 kHz, 16-bit: the other alternate.
	status = set_format(B_SR_48000, B_FMT_16BIT, B_SR_48000);
	sPlaySubslot = 2;
	client.playFormat = B_FMT_16BIT;
	client.playFrame = 0;
	client.recorded.clear();
	client.recordedRight.clear();
	if (status == B_OK)
		status = get_buffers(client, false);
	report("format_48k_16bit", status == B_OK
			&& client.play[0][0].stride == 4,
		"%s, play stride %d", strerror(status), (int)client.play[0][0].stride);
	reset_device_log();
	result = run(client, 1.5, 48000, false);
	report("play_48k.exchange", result.error == B_OK && result.badCycles == 0,
		"%d exchanges, %d bad cycles (%s)", (int)result.exchanges,
		(int)result.badCycles, strerror(result.error));
	report("alternate_16bit", sActiveAlternate[kInterfacePlayback] == 2
			&& sActiveAlternate[kInterfaceRecord] == 0,
		"playback alternate %d, capture %d (not wanted: zero bandwidth)",
		(int)sActiveAlternate[kInterfacePlayback],
		(int)sActiveAlternate[kInterfaceRecord]);
	check_played("play_48k.frames_16bit", 0x7fff);
	check_packets("play_48k.packets", 48000.0 / 8000 * kDeviceClockError);

	// A force stop from another thread while an exchange waits.
	// A stop that falls between two exchanges is missed by design (see
	// exchange_until_error()); on SMP that happens now and then, so up to
	// five tries.
	Stopper stop;
	pthread_t thread;
	bigtime_t returnedAt;
	int tries = 0;
	do {
		stop = (Stopper){ 300000, 0, false, deviceCookie };
		pthread_create(&thread, NULL, stopper, &stop);
		status = exchange_until_error(client, returnedAt, &stop);
		pthread_join(thread, NULL);
		tries++;
	} while (status == B_INTERRUPTED && tries < 5);
	report("force_stop", status == B_CANCELED
			&& returnedAt - stop.at < 200000
			&& sActiveAlternate[kInterfacePlayback] == 0,
		"exchange returned '%s' %d us after the stop, playback alternate %d"
		" (try %d)", strerror(status), (int)(returnedAt - stop.at),
		(int)sActiveAlternate[kInterfacePlayback], tries);

	// The device goes away while it plays.
	stop.remove = true;
	stop.delay = 400000;
	pthread_create(&thread, NULL, stopper, &stop);
	status = exchange_until_error(client, returnedAt);
	pthread_join(thread, NULL);
	report("removed_while_playing", status == B_DEV_NOT_READY
			&& returnedAt - stop.at < 200000,
		"exchange returned '%s' %d us after the removal", strerror(status),
		(int)(returnedAt - stop.at));
	names = publish_devices();
	report("removed_unpublished", names[0] == NULL, "%s",
		names[0] != NULL ? names[0] : "no device published");
	sHooks->close(sCookie);
	sHooks->free(sCookie);
	status = usb_audio_device_added(kDeviceID, &deviceCookie);
	names = publish_devices();
	report("added_again", status == B_OK && names[0] != NULL
			&& strcmp(names[0], "audio/hmulti/usb/1") == 0,
		"%s, %s (the removed device's slot is free again)", strerror(status),
		names[0] != NULL ? names[0] : "nothing");
	usb_audio_device_removed(deviceCookie);

	sBusQuit = true;
	pthread_join(bus, NULL);
	uninit_driver();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
