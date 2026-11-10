/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test of the usb_audio driver with QEMU's usb-audio device (USB
	Audio Class 1: 48 kHz, 16-bit stereo playback, a feature unit with a
	master mute and a volume per channel), plugged in and out while the
	guest runs.

	It asks the harness to plug the device in ("CE-HOST usb_audio add"),
	waits for /dev/audio/hmulti/usb/<n>, and drives it with the same
	ioctl sequence and buffer-cycle rules as the multi_audio media add-on,
	like vsnd_test does for virtio_sound. QEMU records what the device
	plays into usb-<n>.wav (one file per plug-in), where the harness looks
	for these phases (USB_AUDIO_PHASES in ce-test.py):

		play      600/900 Hz, 2 s, volume 0 dB (the level reference)
		volume    650/950 Hz, 1.5 s, both channels at -40 dB
		mute      700/1000 Hz, 1 s, muted: must not be in the recording
		restart   750/1050 Hz, 1.5 s, after a force stop, 0 dB
		unplug    800/1100 Hz, played until the device is pulled out
		replug    850/1150 Hz, 1.5 s, on the device plugged in again
		          (usb-1.wav)

	Between them it checks the description (the device's product string as
	the name, the formats and rates of the alternate setting), the mix
	controls the feature unit gives and their values read back, that rates
	the device does not have are refused, buffer cycles, frame counters,
	real-time pacing, a force stop from a second thread while an exchange
	waits, and what happens when the device is pulled out while it plays:
	the waiting exchange returns an error within a second, the device node
	goes, and a device plugged in again is published and plays.
*/


#include <OS.h>

#include <hmulti_audio.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define SUITE "usb_audio"
#define MAX_TEST_BUFFERS 32
#define MAX_TEST_CHANNELS 16
#define DEVICE_DIRECTORY "/dev/audio/hmulti/usb"


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


static void
host_request(const char* request)
{
	printf("CE-HOST %s\n", request);
	fflush(stdout);
}


/*!	The first device in DEVICE_DIRECTORY other than \a except, or an empty
	string.
*/
static void
find_device(char* path, size_t size, const char* except = NULL)
{
	path[0] = '\0';
	DIR* dir = opendir(DEVICE_DIRECTORY);
	if (dir == NULL)
		return;
	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		char child[PATH_MAX];
		snprintf(child, sizeof(child), DEVICE_DIRECTORY "/%s", entry->d_name);
		if (except != NULL && strcmp(child, except) == 0)
			continue;
		strlcpy(path, child, size);
		break;
	}
	closedir(dir);
}


//!	Waits up to \a timeout for a device to appear; returns how long it took.
static bigtime_t
wait_for_device(char* path, size_t size, bigtime_t timeout,
	const char* except = NULL)
{
	bigtime_t start = system_time();
	while (system_time() - start < timeout) {
		find_device(path, size, except);
		if (path[0] != '\0')
			return system_time() - start;
		snooze(100000);
	}
	return -1;
}


static bool
device_exists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0;
}


struct Tone {
	double	leftHertz;
	double	rightHertz;
	double	leftPhase;
	double	rightPhase;
	double	amplitude;
};


struct Stream {
	int					fd;
	multi_buffer_list	list;
	buffer_desc			bufferList[MAX_TEST_BUFFERS * MAX_TEST_CHANNELS];
	buffer_desc*		buffers[MAX_TEST_BUFFERS];
	uint32				rateHertz;
};


static status_t
get_buffers(Stream& stream)
{
	for (int i = 0; i < MAX_TEST_BUFFERS; i++)
		stream.buffers[i] = &stream.bufferList[i * MAX_TEST_CHANNELS];

	memset(&stream.list, 0, sizeof(stream.list));
	stream.list.info_size = sizeof(stream.list);
	stream.list.request_playback_buffer_size = 0;
	stream.list.request_playback_buffers = MAX_TEST_BUFFERS;
	stream.list.request_playback_channels = 2;
	stream.list.playback_buffers = stream.buffers;
	stream.list.request_record_buffers = 0;
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


struct PlayResult {
	int32		exchanges;
	int32		badCycles;
	int32		badFrameSteps;
	double		ratio;
	status_t	error;
	bigtime_t	errorDelay;	// from \a stopAt (if given) to the failure
};


/*!	Plays \a seconds of \a tone by the multi_audio add-on's rules. If
	\a unplugAfter is > 0, asks the harness to pull the device out after
	that many seconds and goes on until an exchange fails.
*/
static PlayResult
play(Stream& stream, Tone tone, double seconds, double unplugAfter = 0)
{
	PlayResult result = { 0, 0, 0, 0, B_OK, -1 };
	int32 count = stream.list.return_playback_buffers;
	uint32 frames = stream.list.return_playback_buffer_size;
	int32 exchanges = (int32)(seconds * stream.rateHertz / frames) + 1;

	// The add-on fills every buffer before it starts.
	for (int32 i = 0; i < count; i++)
		fill_buffer(stream, i, tone);

	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);

	int32 lastCycle = -1;
	int64 firstFrames = -1;
	int64 lastFrames = 0;
	bigtime_t firstTime = 0;
	bigtime_t lastTime = 0;
	bigtime_t start = system_time();
	bigtime_t unplugAt = 0;

	for (; unplugAfter > 0 || result.exchanges < exchanges;
			result.exchanges++) {
		if (unplugAfter > 0 && unplugAt == 0
			&& system_time() - start > unplugAfter * 1000000) {
			unplugAt = system_time();
			host_request("usb_audio remove");
		}
		if (unplugAt != 0 && system_time() - unplugAt > 10000000) {
			// the device never went away
			result.error = B_TIMED_OUT;
			break;
		}
		if (ioctl(stream.fd, B_MULTI_BUFFER_EXCHANGE, &info,
				sizeof(info)) != 0) {
			result.error = errno;
			if (unplugAt != 0)
				result.errorDelay = system_time() - unplugAt;
			break;
		}

		int32 cycle = info.playback_buffer_cycle;
		if (cycle < 0 || cycle >= count
			|| (lastCycle >= 0 && cycle != (lastCycle + 1) % count)) {
			result.badCycles++;
		}
		if (firstFrames >= 0
			&& info.played_frames_count - lastFrames != (int64)frames) {
			result.badFrameSteps++;
		}
		if (firstFrames < 0) {
			firstFrames = info.played_frames_count;
			firstTime = info.played_real_time;
		}
		lastFrames = info.played_frames_count;
		lastTime = info.played_real_time;
		lastCycle = cycle;

		fill_buffer(stream, (cycle - 1 + count) % count, tone);
	}

	double elapsed = (lastTime - firstTime) / 1000000.0;
	double measured = elapsed > 0 ? (lastFrames - firstFrames) / elapsed : 0;
	result.ratio = measured / stream.rateHertz;
	return result;
}


static void
report_play(const char* phase, const PlayResult& result)
{
	char name[64];
	snprintf(name, sizeof(name), "%s.exchange", phase);
	report(name, result.error == B_OK, "%" B_PRId32 " exchanges (%s)",
		result.exchanges, strerror(result.error));
	snprintf(name, sizeof(name), "%s.cycles", phase);
	report(name, result.badCycles == 0, "%" B_PRId32 " bad buffer cycles",
		result.badCycles);
	snprintf(name, sizeof(name), "%s.frame_count", phase);
	report(name, result.badFrameSteps == 0, "%" B_PRId32 " exchanges did not "
		"advance by one buffer", result.badFrameSteps);
	snprintf(name, sizeof(name), "%s.pacing", phase);
	report(name, result.ratio > 0.75 && result.ratio < 1.25,
		"ratio %.3f of 48 kHz", result.ratio);
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


// #pragma mark - mix controls


struct MixControls {
	multi_mix_control	controls[64];
	int32				count;
	int32				mute;		// index, -1 if none
	int32				gains[2];	// left and right gain
	int32				gainCount;
};


static status_t
list_controls(int fd, MixControls& mix)
{
	memset(&mix, 0, sizeof(mix));
	multi_mix_control_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	info.control_count = 64;
	info.controls = mix.controls;
	if (ioctl(fd, B_MULTI_LIST_MIX_CONTROLS, &info, sizeof(info)) != 0)
		return errno;

	mix.count = info.control_count;
	mix.mute = -1;
	mix.gains[0] = mix.gains[1] = -1;
	for (int32 i = 0; i < mix.count; i++) {
		const multi_mix_control& control = mix.controls[i];
		if ((control.flags & B_MULTI_MIX_ENABLE) != 0 && mix.mute < 0
			&& control.string == S_MUTE) {
			mix.mute = i;
		}
		if ((control.flags & B_MULTI_MIX_GAIN) != 0 && mix.gainCount < 2)
			mix.gains[mix.gainCount++] = i;
	}
	return B_OK;
}


static status_t
set_mix(int fd, MixControls& mix, int32 index, float gain, bool enable)
{
	multi_mix_value value;
	memset(&value, 0, sizeof(value));
	value.id = mix.controls[index].id;
	if ((mix.controls[index].flags & B_MULTI_MIX_GAIN) != 0)
		value.gain = gain;
	else
		value.enable = enable;
	multi_mix_value_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	info.item_count = 1;
	info.values = &value;
	if (ioctl(fd, B_MULTI_SET_MIX, &info, sizeof(info)) != 0)
		return errno;
	return B_OK;
}


static status_t
get_mix(int fd, MixControls& mix, int32 index, multi_mix_value& value)
{
	memset(&value, 0, sizeof(value));
	value.id = mix.controls[index].id;
	multi_mix_value_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	info.item_count = 1;
	info.values = &value;
	if (ioctl(fd, B_MULTI_GET_MIX, &info, sizeof(info)) != 0)
		return errno;
	return B_OK;
}


static bool
set_volume(int fd, MixControls& mix, float db)
{
	bool ok = mix.gainCount == 2;
	for (int32 i = 0; i < mix.gainCount; i++)
		ok &= set_mix(fd, mix, mix.gains[i], db, false) == B_OK;
	return ok;
}


// #pragma mark - force stop


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


static void
concurrent_stop(Stream& stream)
{
	StopperArgs args = { stream.fd, 300000, 0, B_ERROR };
	thread_id thread = spawn_thread(stopper_thread, "usb audio stopper",
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


// #pragma mark -


int
main(int argc, char** argv)
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	char path[PATH_MAX];
	find_device(path, sizeof(path));
	report("none_at_start", path[0] == '\0', "%s",
		path[0] != '\0' ? path : "no USB audio device before the plug-in");

	host_request("usb_audio add");
	bigtime_t took = wait_for_device(path, sizeof(path), 20000000);
	report("hotplug_published", took >= 0, "%s after %.1f s",
		path[0] != '\0' ? path : "nothing", took / 1000000.0);
	if (took < 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}
	// The driver publishes the node from its device_added hook, before the
	// stack is done with the device; give it a moment.
	snooze(500000);

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

	multi_channel_info channels[MAX_TEST_CHANNELS];
	multi_description description;
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = MAX_TEST_CHANNELS;
	description.channels = channels;
	bool ok = ioctl(stream.fd, B_MULTI_GET_DESCRIPTION, &description,
		sizeof(description)) == 0;
	report("description", ok
			&& strcmp(description.friendly_name, "QEMU USB Audio") == 0
			&& description.output_channel_count == 2
			&& description.input_channel_count == 0
			&& description.output_rates == B_SR_48000
			&& description.output_formats == B_FMT_16BIT
			&& channels[0].kind == B_MULTI_OUTPUT_CHANNEL
			&& (channels[0].designations & B_CHANNEL_LEFT) != 0
			&& (channels[1].designations & B_CHANNEL_RIGHT) != 0,
		"name '%s' out=%" B_PRId32 " in=%" B_PRId32 " rates=0x%" B_PRIx32
		" formats=0x%" B_PRIx32, description.friendly_name,
		description.output_channel_count, description.input_channel_count,
		description.output_rates, description.output_formats);

	uint32 enableBits = 0;
	multi_channel_enable enable;
	memset(&enable, 0, sizeof(enable));
	enable.info_size = sizeof(enable);
	enable.enable_bits = (uchar*)&enableBits;
	ok = ioctl(stream.fd, B_MULTI_GET_ENABLED_CHANNELS, &enable,
		sizeof(enable)) == 0;
	report("enabled_channels", ok && enableBits == 0x3, "bits=0x%" B_PRIx32,
		enableBits);

	report("reject_bad_rate", set_format(stream.fd, B_SR_44100, B_FMT_16BIT)
		!= B_OK, "44.1 kHz is not one of the device's rates");
	report("reject_bad_format", set_format(stream.fd, B_SR_48000,
		B_FMT_24BIT) != B_OK, "24-bit is not one of the device's formats");
	status_t status = set_format(stream.fd, B_SR_48000, B_FMT_16BIT);
	report("set_format", status == B_OK, "%s", strerror(status));

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

	// The feature unit: master mute, volume on each channel (QEMU's range
	// is 0x8001 to 0x0800 in 1/256 dB, steps of 0x88).
	MixControls mix;
	status = list_controls(stream.fd, mix);
	bool limitsOk = mix.gainCount == 2;
	for (int32 i = 0; i < mix.gainCount; i++) {
		const multi_mix_control& gain = mix.controls[mix.gains[i]];
		limitsOk &= fabs(gain.gain.min_gain - (-127.996f)) < 0.01
			&& fabs(gain.gain.max_gain - 8.0f) < 0.01
			&& fabs(gain.gain.granularity - 0.53125f) < 0.001;
	}
	report("mix_controls", status == B_OK && mix.mute >= 0 && limitsOk,
		"%s: %" B_PRId32 " controls, mute %s, %" B_PRId32 " gains, range "
		"%.3f..%.3f dB step %.4f", strerror(status), mix.count,
		mix.mute >= 0 ? "found" : "missing", mix.gainCount,
		mix.gainCount > 0 ? mix.controls[mix.gains[0]].gain.min_gain : 0,
		mix.gainCount > 0 ? mix.controls[mix.gains[0]].gain.max_gain : 0,
		mix.gainCount > 0 ? mix.controls[mix.gains[0]].gain.granularity : 0);

	ok = set_volume(stream.fd, mix, 0)
		&& (mix.mute < 0 || set_mix(stream.fd, mix, mix.mute, 0, false)
			== B_OK);
	report("mix_set", ok, "volume 0 dB, unmuted");

	stream.rateHertz = 48000;
	status = get_buffers(stream);
	bool layoutOk = status == B_OK
		&& stream.list.return_playback_buffers >= 2
		&& stream.list.return_playback_channels == 2
		&& stream.list.return_playback_buffer_size >= 768
		&& stream.buffers[0][0].stride == 4
		&& stream.buffers[0][1].base == stream.buffers[0][0].base + 2;
	report("get_buffers", layoutOk, "%s: %" B_PRId32 " buffers x %" B_PRIu32
		" frames, %" B_PRId32 " channels", strerror(status),
		stream.list.return_playback_buffers,
		stream.list.return_playback_buffer_size,
		stream.list.return_playback_channels);
	if (!layoutOk) {
		close(stream.fd);
		host_request("usb_audio remove");
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	Tone play1 = { 600, 900, 0, 0, 12000 };
	report_play("play", play(stream, play1, 2.0));

	ok = set_volume(stream.fd, mix, -40);
	multi_mix_value value;
	ok &= get_mix(stream.fd, mix, mix.gains[0], value) == B_OK;
	// QEMU keeps 8 bits: what is read back is the nearest of its steps.
	report("volume_set", ok && fabs(value.gain - (-40)) < 1.0,
		"left gain reads %.3f dB", value.gain);
	Tone volume = { 650, 950, 0, 0, 12000 };
	report_play("volume", play(stream, volume, 1.5));
	set_volume(stream.fd, mix, 0);

	if (mix.mute >= 0) {
		ok = set_mix(stream.fd, mix, mix.mute, 0, true) == B_OK
			&& get_mix(stream.fd, mix, mix.mute, value) == B_OK
			&& value.enable;
		report("mute_set", ok, "mute reads %s", value.enable ? "on" : "off");
		Tone muted = { 700, 1000, 0, 0, 12000 };
		report_play("mute", play(stream, muted, 1.0));
		ok = set_mix(stream.fd, mix, mix.mute, 0, false) == B_OK
			&& get_mix(stream.fd, mix, mix.mute, value) == B_OK
			&& !value.enable;
		report("unmute_set", ok, "mute reads %s", value.enable ? "on" : "off");
	}

	ok = ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0) == 0;
	report("force_stop", ok, "");
	snooze(200000);
	Tone restart = { 750, 1050, 0, 0, 12000 };
	report_play("restart", play(stream, restart, 1.5));
	concurrent_stop(stream);

	// Pull the device out while it plays: the waiting exchange has to fail
	// promptly, not wait for its timeout or forever.
	Tone unplug = { 800, 1100, 0, 0, 12000 };
	PlayResult result = play(stream, unplug, 0, 1.5);
	report("unplug_exchange", result.error != B_OK && result.error
			!= B_TIMED_OUT && result.errorDelay >= 0
			&& result.errorDelay < 3000000,
		"exchange failed with '%s' %.2f s after the request",
		strerror(result.error), result.errorDelay / 1000000.0);
	report("unplug_cycles", result.badCycles == 0 && result.exchanges > 10,
		"%" B_PRId32 " exchanges before, %" B_PRId32 " bad cycles",
		result.exchanges, result.badCycles);

	bigtime_t start = system_time();
	while (device_exists(path) && system_time() - start < 10000000)
		snooze(100000);
	report("unplug_unpublished", !device_exists(path),
		"%s %s after %.1f s", path, device_exists(path) ? "still there"
			: "gone", (system_time() - start) / 1000000.0);
	multi_description after;
	memset(&after, 0, sizeof(after));
	after.info_size = sizeof(after);
	after.request_channel_count = MAX_TEST_CHANNELS;
	after.channels = channels;
	report("unplug_ioctl_fails", ioctl(stream.fd, B_MULTI_GET_DESCRIPTION,
		&after, sizeof(after)) != 0, "%s on the removed device",
		strerror(errno));
	close(stream.fd);

	// Plug it in again: a new device, published and playing.
	host_request("usb_audio add");
	char newPath[PATH_MAX];
	took = wait_for_device(newPath, sizeof(newPath), 20000000);
	report("replug_published", took >= 0, "%s after %.1f s",
		newPath[0] != '\0' ? newPath : "nothing", took / 1000000.0);
	if (took >= 0) {
		snooze(500000);
		stream.fd = open(newPath, O_WRONLY);
		report("replug_open", stream.fd >= 0, "%s",
			strerror(stream.fd >= 0 ? 0 : errno));
		if (stream.fd >= 0) {
			set_format(stream.fd, B_SR_48000, B_FMT_16BIT);
			MixControls mix2;
			list_controls(stream.fd, mix2);
			set_volume(stream.fd, mix2, 0);
			status = get_buffers(stream);
			if (status == B_OK) {
				Tone replug = { 850, 1150, 0, 0, 12000 };
				report_play("replug", play(stream, replug, 1.5));
			} else
				report("replug.exchange", false, "%s", strerror(status));
			ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
			close(stream.fd);
		}
		host_request("usb_audio remove");
		start = system_time();
		while (device_exists(newPath) && system_time() - start < 10000000)
			snooze(100000);
		report("closed_unplug", !device_exists(newPath),
			"%s gone after %.1f s (unplugged while closed)", newPath,
			(system_time() - start) / 1000000.0);
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
