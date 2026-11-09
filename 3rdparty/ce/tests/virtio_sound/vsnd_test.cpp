/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest functional test for the virtio_sound driver.

	It talks to /dev/audio/hmulti/virtio_sound/<n> directly, using the same
	ioctl sequence and buffer-cycle rules as the multi_audio media add-on
	(src/add-ons/media/media-add-ons/multi_audio), so it exercises the
	driver without needing the Media Kit in the image.

	Every check prints one machine-readable line:

		CE-TEST <suite>.<name> PASS|FAIL <detail>

	and the run ends with "CE-SUITE virtio_sound DONE". The host harness
	(3rdparty/ce/harness/ce-test.py) collects these from the serial console.

	It also plays four tone phases that the harness looks for in the WAV
	file QEMU records from the virtual sound card:

		phase 1: 48 kHz, left 440 Hz / right 880 Hz, 3 s
		phase 2: after B_MULTI_BUFFER_FORCE_STOP + restart,
		         left 660 Hz / right 330 Hz, 1.5 s
		phase 3: after switching to 44.1 kHz while running (new buffers),
		         1000 Hz on both channels, 1.5 s
		phase 4: after closing and reopening the device, 48 kHz,
		         left 500 Hz / right 250 Hz, 1 s

	Between phases it checks the paths that are easy to get wrong:
	exclusive open, B_MULTI_BUFFER_FORCE_STOP from a second thread while an
	exchange is blocked, and rejection of kernel pointers.
*/


#include <Debug.h>
#include <OS.h>

#include <hmulti_audio.h>

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define SUITE "virtio_sound"
#define MAX_TEST_BUFFERS 32
#define MAX_TEST_CHANNELS 16


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


static bool
find_device(const char* directory, char* path, size_t size)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return false;

	bool found = false;
	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		char child[PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", directory, entry->d_name);
		struct stat st;
		if (stat(child, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (find_device(child, path, size)) {
				found = true;
				break;
			}
		} else if (strstr(child, "virtio_sound") != NULL) {
			strlcpy(path, child, size);
			found = true;
			break;
		}
	}
	closedir(dir);
	return found;
}


struct Tone {
	double	leftHertz;
	double	rightHertz;
	double	leftPhase;
	double	rightPhase;
	double	amplitude;	// 0 plays silence
};


struct Stream {
	int					fd;
	multi_buffer_list	list;
	buffer_desc			bufferList[MAX_TEST_BUFFERS * MAX_TEST_CHANNELS];
	buffer_desc*		buffers[MAX_TEST_BUFFERS];
	uint32				rateHertz;
};


static status_t
get_buffers(Stream& stream, uint32 frames = 0)
{
	for (int i = 0; i < MAX_TEST_BUFFERS; i++)
		stream.buffers[i] = &stream.bufferList[i * MAX_TEST_CHANNELS];

	memset(&stream.list, 0, sizeof(stream.list));
	stream.list.info_size = sizeof(stream.list);
	stream.list.request_playback_buffer_size = frames;	// 0: driver default
	stream.list.request_playback_buffers = MAX_TEST_BUFFERS;
	stream.list.request_playback_channels = 2;
	stream.list.playback_buffers = stream.buffers;
	stream.list.request_record_buffers = MAX_TEST_BUFFERS;
	stream.list.request_record_channels = 0;
	stream.list.record_buffers = NULL;

	if (ioctl(stream.fd, B_MULTI_GET_BUFFERS, &stream.list,
			sizeof(stream.list)) != 0) {
		return errno;
	}
	return B_OK;
}


static void
fill_buffer(Stream& stream, int32 index, Tone& tone)
{
	uint32 frames = stream.list.return_playback_buffer_size;
	buffer_desc* desc = stream.buffers[index];
	double rate = stream.rateHertz;

	if (tone.amplitude == 0) {
		// Silence: keep this fast, so that tests timing a blocked exchange
		// are not skewed by sin() under emulation.
		for (uint32 frame = 0; frame < frames; frame++) {
			*(int16*)(desc[0].base + frame * desc[0].stride) = 0;
			*(int16*)(desc[1].base + frame * desc[1].stride) = 0;
		}
		return;
	}

	for (uint32 frame = 0; frame < frames; frame++) {
		int16* left = (int16*)(desc[0].base + frame * desc[0].stride);
		int16* right = (int16*)(desc[1].base + frame * desc[1].stride);
		*left = (int16)(tone.amplitude * sin(tone.leftPhase));
		*right = (int16)(tone.amplitude * sin(tone.rightPhase));
		tone.leftPhase += 2 * M_PI * tone.leftHertz / rate;
		tone.rightPhase += 2 * M_PI * tone.rightHertz / rate;
	}
	tone.leftPhase = fmod(tone.leftPhase, 2 * M_PI);
	tone.rightPhase = fmod(tone.rightPhase, 2 * M_PI);
}


/*!	Plays \a seconds of the tone, following the multi_audio add-on's
	buffer-cycle protocol, and checks the timing information.
*/
static void
play(Stream& stream, const char* phase, Tone tone, double seconds)
{
	int32 count = stream.list.return_playback_buffers;
	uint32 frames = stream.list.return_playback_buffer_size;
	int32 exchanges = (int32)(seconds * stream.rateHertz / frames) + 1;

	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);

	int32 lastCycle = -1;
	int64 firstFrames = -1;
	int64 lastFrames = 0;
	bigtime_t firstTime = 0;
	bigtime_t lastTime = 0;
	int32 badCycles = 0;
	int32 badFrameSteps = 0;
	status_t error = B_OK;
	int32 done = 0;

	for (; done < exchanges; done++) {
		if (ioctl(stream.fd, B_MULTI_BUFFER_EXCHANGE, &info,
				sizeof(info)) != 0) {
			error = errno;
			break;
		}

		// Buffers complete in the order they were queued, so the cycle has
		// to advance by exactly one each time.
		int32 cycle = info.playback_buffer_cycle;
		if (cycle < 0 || cycle >= count
			|| (lastCycle >= 0 && cycle != (lastCycle + 1) % count)) {
			badCycles++;
		}
		if (firstFrames >= 0
			&& info.played_frames_count - lastFrames != (int64)frames) {
			badFrameSteps++;
		}
		if (firstFrames < 0) {
			firstFrames = info.played_frames_count;
			firstTime = info.played_real_time;
		}
		lastFrames = info.played_frames_count;
		lastTime = info.played_real_time;
		lastCycle = cycle;

		// Same rule as MultiAudioNode::_OutputThread().
		fill_buffer(stream, (cycle - 1 + count) % count, tone);
	}

	char name[64];
	snprintf(name, sizeof(name), "%s.exchange", phase);
	report(name, error == B_OK, "%" B_PRId32 "/%" B_PRId32 " exchanges (%s)",
		done, exchanges, strerror(error));

	snprintf(name, sizeof(name), "%s.cycles", phase);
	report(name, badCycles == 0, "%" B_PRId32 " bad buffer cycles", badCycles);

	snprintf(name, sizeof(name), "%s.frame_count", phase);
	report(name, badFrameSteps == 0,
		"%" B_PRId32 " exchanges did not advance by %" B_PRIu32 " frames",
		badFrameSteps, frames);

	// The device paces playback; the measured rate must be close to nominal.
	// The tolerance is generous because the guest may run under emulation.
	double elapsed = (lastTime - firstTime) / 1000000.0;
	double measured = elapsed > 0 ? (lastFrames - firstFrames) / elapsed : 0;
	double ratio = measured / stream.rateHertz;
	snprintf(name, sizeof(name), "%s.pacing", phase);
	report(name, ratio > 0.75 && ratio < 1.25,
		"%.0f frames/s measured, %" B_PRIu32 " nominal (ratio %.3f)",
		measured, stream.rateHertz, ratio);
}


struct StopperArgs {
	int			fd;
	bigtime_t	delay;
	bigtime_t	stoppedAt;
	status_t	result;
};


static status_t
stopper_thread(void* data)
{
	StopperArgs* args = (StopperArgs*)data;
	snooze(args->delay);
	args->stoppedAt = system_time();
	args->result = ioctl(args->fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0) == 0
		? B_OK : errno;
	return B_OK;
}


/*!	Exchanges silence while another thread calls B_MULTI_BUFFER_FORCE_STOP.
	The blocked exchange must return promptly with an error, not run into
	the driver's two second timeout.
*/
static void
concurrent_stop(Stream& stream)
{
	// Long periods (about 186 ms at 44.1 kHz) so that the main thread is
	// almost certainly blocked in the exchange when the stop arrives: if it
	// is between two exchanges instead, the next one legitimately restarts
	// the stream and nothing is tested.
	status_t status = get_buffers(stream, 8192);
	if (status != B_OK) {
		report("concurrent_stop", false, "get_buffers: %s", strerror(status));
		return;
	}

	StopperArgs args = { stream.fd, 300000, 0, B_ERROR };
	thread_id thread = spawn_thread(stopper_thread, "vsnd stopper",
		B_NORMAL_PRIORITY, &args);
	resume_thread(thread);

	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	Tone silence = { 0, 0, 0, 0, 0 };
	int32 count = stream.list.return_playback_buffers;

	bigtime_t deadline = system_time() + 3000000;
	status_t error = B_OK;
	while (system_time() < deadline) {
		if (ioctl(stream.fd, B_MULTI_BUFFER_EXCHANGE, &info,
				sizeof(info)) != 0) {
			error = errno;
			break;
		}
		fill_buffer(stream, (info.playback_buffer_cycle - 1 + count) % count,
			silence);
	}
	bigtime_t returnedAt = system_time();

	status_t threadResult;
	wait_for_thread(thread, &threadResult);

	bigtime_t delay = args.stoppedAt > 0 ? returnedAt - args.stoppedAt : -1;
	report("concurrent_stop", args.result == B_OK && error != B_OK
			&& delay >= 0 && delay < 500000,
		"force stop %s, exchange returned '%s' %" B_PRIdBIGTIME
		" us after it", strerror(args.result), strerror(error), delay);
}


static status_t
set_format(int fd, uint32 rate, uint32 format)
{
	multi_format_info formatInfo;
	memset(&formatInfo, 0, sizeof(formatInfo));
	formatInfo.info_size = sizeof(formatInfo);
	formatInfo.output.rate = rate;
	formatInfo.output.format = format;
	if (ioctl(fd, B_MULTI_SET_GLOBAL_FORMAT, &formatInfo,
			sizeof(formatInfo)) != 0) {
		return errno;
	}
	return B_OK;
}


int
main(int argc, char** argv)
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	char path[PATH_MAX];
	bool found = find_device("/dev/audio/hmulti", path, sizeof(path));
	report("device_published", found, "%s", found ? path : "not found");
	if (!found) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	Stream stream;
	stream.fd = open(path, O_WRONLY);
	report("open", stream.fd >= 0, "%s", strerror(stream.fd >= 0 ? 0 : errno));
	if (stream.fd < 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	int second = open(path, O_WRONLY);
	report("exclusive_open", second < 0 && errno == B_BUSY,
		"second open: %s", second < 0 ? strerror(errno) : "succeeded");
	if (second >= 0)
		close(second);

	// Description
	multi_channel_info channels[MAX_TEST_CHANNELS];
	multi_description description;
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = MAX_TEST_CHANNELS;
	description.channels = channels;
	bool ok = ioctl(stream.fd, B_MULTI_GET_DESCRIPTION, &description,
		sizeof(description)) == 0;
	report("description", ok
			&& description.output_channel_count == 2
			&& description.input_channel_count == 0
			&& (description.output_rates & B_SR_48000) != 0
			&& (description.output_rates & B_SR_44100) != 0
			&& description.output_formats == B_FMT_16BIT
			&& channels[0].kind == B_MULTI_OUTPUT_CHANNEL
			&& (channels[0].designations & B_CHANNEL_LEFT) != 0
			&& (channels[1].designations & B_CHANNEL_RIGHT) != 0,
		"name '%s' out=%" B_PRId32 " in=%" B_PRId32 " rates=0x%" B_PRIx32
		" formats=0x%" B_PRIx32, description.friendly_name,
		description.output_channel_count, description.input_channel_count,
		description.output_rates, description.output_formats);

	// Enabled channels
	uint32 enableBits = 0;
	multi_channel_enable enable;
	memset(&enable, 0, sizeof(enable));
	enable.info_size = sizeof(enable);
	enable.enable_bits = (uchar*)&enableBits;
	ok = ioctl(stream.fd, B_MULTI_GET_ENABLED_CHANNELS, &enable,
		sizeof(enable)) == 0;
	report("enabled_channels", ok && (enableBits & 0x3) == 0x3,
		"bits=0x%" B_PRIx32, enableBits);
	ok = ioctl(stream.fd, B_MULTI_SET_ENABLED_CHANNELS, &enable,
		sizeof(enable)) == 0;
	report("set_enabled_channels", ok, "");

	// Formats
	report("reject_bad_rate", set_format(stream.fd, B_SR_192000, B_FMT_16BIT)
		!= B_OK, "192 kHz must be refused");
	status_t status = set_format(stream.fd, B_SR_48000, B_FMT_16BIT);
	report("set_format_48k", status == B_OK, "%s", strerror(status));

	multi_format_info formatInfo;
	memset(&formatInfo, 0, sizeof(formatInfo));
	formatInfo.info_size = sizeof(formatInfo);
	ok = ioctl(stream.fd, B_MULTI_GET_GLOBAL_FORMAT, &formatInfo,
		sizeof(formatInfo)) == 0;
	report("get_format", ok && formatInfo.output.rate == B_SR_48000
			&& formatInfo.output.format == B_FMT_16BIT
			&& formatInfo.output_latency > 0,
		"rate=0x%" B_PRIx32 " format=0x%" B_PRIx32 " latency=%" B_PRIdBIGTIME
		"us", formatInfo.output.rate, formatInfo.output.format,
		formatInfo.output_latency);

	// Buffers: the media add-on asks for 32 buffers and the default size.
	stream.rateHertz = 48000;
	status = get_buffers(stream);
	bool layoutOk = status == B_OK
		&& stream.list.return_playback_buffers >= 3
		&& stream.list.return_playback_buffers <= 8
		&& stream.list.return_playback_channels == 2
		&& stream.list.return_playback_buffer_size > 0
		&& stream.buffers[0][0].stride == 4
		&& stream.buffers[0][1].base == stream.buffers[0][0].base + 2;
	report("get_buffers", layoutOk, "%s: %" B_PRId32 " buffers x %" B_PRIu32
		" frames, %" B_PRId32 " channels", strerror(status),
		stream.list.return_playback_buffers,
		stream.list.return_playback_buffer_size,
		stream.list.return_playback_channels);

	if (layoutOk) {
		Tone tone1 = { 440, 880, 0, 0, 16000 };
		play(stream, "phase1", tone1, 3.0);

		ok = ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0) == 0;
		report("force_stop", ok, "");
		snooze(300000);

		Tone tone2 = { 660, 330, 0, 0, 16000 };
		play(stream, "phase2", tone2, 1.5);

		// Like MultiAudioDevice::SetOutputFrameRate(): change the format
		// while the stream is running, then ask for new buffers.
		status = set_format(stream.fd, B_SR_44100, B_FMT_16BIT);
		report("set_format_44k_running", status == B_OK, "%s",
			strerror(status));
		stream.rateHertz = 44100;
		status = get_buffers(stream);
		report("get_buffers_44k", status == B_OK, "%s", strerror(status));
		if (status == B_OK) {
			Tone tone3 = { 1000, 1000, 0, 0, 16000 };
			play(stream, "phase3", tone3, 1.5);
			concurrent_stop(stream);
		}
	}

	// Kernel pointers inside the ioctl structures must be refused.
	multi_description badDescription;
	memset(&badDescription, 0, sizeof(badDescription));
	badDescription.info_size = sizeof(badDescription);
	badDescription.request_channel_count = 4;
	badDescription.channels = (multi_channel_info*)0xffffffff80001000UL;
	bool refused = ioctl(stream.fd, B_MULTI_GET_DESCRIPTION, &badDescription,
		sizeof(badDescription)) != 0;
	report("reject_kernel_pointer", refused, "%s",
		refused ? strerror(errno) : "accepted");

	close(stream.fd);

	// Reopen: the transport must survive uninit_device()/init_device(), and
	// play again.
	stream.fd = open(path, O_WRONLY);
	report("reopen", stream.fd >= 0, "%s", strerror(stream.fd >= 0 ? 0 : errno));
	if (stream.fd >= 0) {
		status = set_format(stream.fd, B_SR_48000, B_FMT_16BIT);
		stream.rateHertz = 48000;
		if (status == B_OK)
			status = get_buffers(stream);
		report("reopen_buffers", status == B_OK, "%s", strerror(status));
		if (status == B_OK) {
			Tone tone4 = { 500, 250, 0, 0, 16000 };
			play(stream, "phase4", tone4, 1.0);
			ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
		}
		close(stream.fd);
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
