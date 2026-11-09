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

	Capture. The harness attaches two sound cards: the one recording into
	audio.wav (QEMU's wav backend, which has no input), and a second one on
	QEMU's SDL backend, whose capture input reads what the harness writes
	into a FIFO: a stereo sine whose frequencies the test chooses with
	"CE-HOST audio_in <left Hz> <right Hz> <amplitude>". The test tells the
	cards apart by recording briefly from each: the playback phases above run
	on the card whose input stays silent, the capture checks on the one that
	delivers the fed tone. The capture checks record through the same
	exchange protocol as the multi_audio add-on (record cycle = buffer just
	filled, -1 if none) and check, on the samples put back together in
	order, that each channel carries the requested frequency at the
	requested level with no gaps (Goertzel purity per 100 ms window, as the
	host's audio analysis does), that the record cycles and frame counters
	advance one period at a time, and that capture runs in real time; at
	48 and 44.1 kHz, after a tone change, after a force-stop, and after
	reopening. On the silent card, a capture stream must not disturb
	playback.
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


#define MAX_DEVICES 8


//!	Collects the paths of every virtio_sound device below \a directory.
static void
find_devices(const char* directory, char paths[][PATH_MAX], int& count)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return;

	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		char child[PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", directory, entry->d_name);
		struct stat st;
		if (stat(child, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode))
			find_devices(child, paths, count);
		else if (strstr(child, "virtio_sound") != NULL && count < MAX_DEVICES)
			strlcpy(paths[count++], child, PATH_MAX);
	}
	closedir(dir);
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


static buffer_desc sRecordBufferList[MAX_TEST_BUFFERS * MAX_TEST_CHANNELS];
static buffer_desc* sRecordBuffers[MAX_TEST_BUFFERS];


static status_t
get_buffers(Stream& stream, uint32 frames = 0, bool record = false)
{
	for (int i = 0; i < MAX_TEST_BUFFERS; i++) {
		stream.buffers[i] = &stream.bufferList[i * MAX_TEST_CHANNELS];
		sRecordBuffers[i] = &sRecordBufferList[i * MAX_TEST_CHANNELS];
	}

	memset(&stream.list, 0, sizeof(stream.list));
	stream.list.info_size = sizeof(stream.list);
	stream.list.request_playback_buffer_size = frames;	// 0: driver default
	stream.list.request_playback_buffers = MAX_TEST_BUFFERS;
	stream.list.request_playback_channels = 2;
	stream.list.playback_buffers = stream.buffers;
	// Like the multi_audio add-on, which asks for as many record buffers
	// and channels as for playback.
	stream.list.request_record_buffer_size = frames;
	stream.list.request_record_buffers = MAX_TEST_BUFFERS;
	stream.list.request_record_channels = record ? 2 : 0;
	stream.list.record_buffers = record ? sRecordBuffers : NULL;

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




// #pragma mark - capture


/*!	What a capture run delivered: the recorded samples put back together in
	order, and what the exchanges reported along the way.
*/
struct Capture {
	int16*		left;
	int16*		right;
	uint32		frames;			// recorded
	uint32		capacity;
	int32		exchanges;
	int32		recordCycles;	// exchanges that reported a record cycle
	int32		badCycles;		// cycle not the successor of the last one
	int32		badFrameSteps;	// frame counter not one period further
	int32		badTimes;		// record time not after the last one
	int64		firstFrames;
	int64		lastFrames;
	bigtime_t	firstTime;
	bigtime_t	lastTime;
	status_t	error;

	Capture(uint32 maxFrames)
		:
		left(new int16[maxFrames]),
		right(new int16[maxFrames]),
		capacity(maxFrames)
	{
		Reset();
	}

	~Capture()
	{
		delete[] left;
		delete[] right;
	}

	void Reset()
	{
		frames = 0;
		exchanges = recordCycles = badCycles = badFrameSteps = badTimes = 0;
		firstFrames = lastFrames = -1;
		firstTime = lastTime = 0;
		error = B_OK;
	}
};


/*!	Exchanges buffers for \a seconds, playing silence and collecting what the
	record cycles deliver, by the multi_audio add-on's rules: a record cycle
	>= 0 that differs from the last one names the buffer just filled.
*/
static void
record(Stream& stream, Capture& capture, double seconds)
{
	capture.Reset();
	int32 count = stream.list.return_playback_buffers;
	int32 recordCount = stream.list.return_record_buffers;
	uint32 frames = stream.list.return_playback_buffer_size;
	uint32 recordFrames = stream.list.return_record_buffer_size;
	int32 exchanges = (int32)(seconds * stream.rateHertz / frames) + 1;
	Tone silence = { 0, 0, 0, 0, 0 };

	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	int32 lastCycle = -1;

	for (; capture.exchanges < exchanges; capture.exchanges++) {
		if (ioctl(stream.fd, B_MULTI_BUFFER_EXCHANGE, &info,
				sizeof(info)) != 0) {
			capture.error = errno;
			break;
		}
		fill_buffer(stream, (info.playback_buffer_cycle - 1 + count) % count,
			silence);

		int32 cycle = info.record_buffer_cycle;
		if (cycle < 0 || cycle >= recordCount || cycle == lastCycle)
			continue;

		capture.recordCycles++;
		if (lastCycle >= 0 && cycle != (lastCycle + 1) % recordCount)
			capture.badCycles++;
		if (capture.firstFrames >= 0
			&& info.recorded_frames_count - capture.lastFrames
				!= (int64)recordFrames) {
			capture.badFrameSteps++;
		}
		if (capture.firstFrames >= 0
			&& info.recorded_real_time <= capture.lastTime) {
			capture.badTimes++;
		}
		if (capture.firstFrames < 0) {
			capture.firstFrames = info.recorded_frames_count;
			capture.firstTime = info.recorded_real_time;
		}
		capture.lastFrames = info.recorded_frames_count;
		capture.lastTime = info.recorded_real_time;
		lastCycle = cycle;

		buffer_desc* desc = sRecordBuffers[cycle];
		for (uint32 frame = 0; frame < recordFrames
				&& capture.frames < capture.capacity; frame++) {
			capture.left[capture.frames]
				= *(int16*)(desc[0].base + frame * desc[0].stride);
			capture.right[capture.frames]
				= *(int16*)(desc[1].base + frame * desc[1].stride);
			capture.frames++;
		}
	}
}


static double
goertzel_power(const int16* samples, uint32 count, double rate, double hertz)
{
	double coefficient = 2 * cos(2 * M_PI * hertz / rate);
	double s1 = 0, s2 = 0;
	for (uint32 i = 0; i < count; i++) {
		double s0 = samples[i] + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}


/*!	Checks one recorded channel against the tone the host fed: in every
	100 ms window after \a skip frames, nearly all energy at \a hertz
	(purity, as in the host's audio analysis; a gap or a dropped period
	shows as a window below 0.9) and the RMS level near amplitude / sqrt(2).
*/
struct ToneCheck {
	int32	windows;
	double	minPurity;
	double	minLevel;		// RMS / expected RMS
	double	maxLevel;
};


static ToneCheck
check_tone(const int16* samples, uint32 frames, uint32 skip, double rate,
	double hertz, double amplitude)
{
	ToneCheck result = { 0, 1.0, 1e9, 0 };
	uint32 window = (uint32)(rate / 10);
	double expected = amplitude / sqrt(2.0);
	for (uint32 start = skip; start + window <= frames; start += window) {
		const int16* chunk = samples + start;
		double energy = 0;
		for (uint32 i = 0; i < window; i++)
			energy += (double)chunk[i] * chunk[i];
		double purity = energy > 0
			? 2 * goertzel_power(chunk, window, rate, hertz)
				/ (window * energy) : 0;
		double level = sqrt(energy / window) / expected;
		result.windows++;
		result.minPurity = min_c(result.minPurity, purity);
		result.minLevel = min_c(result.minLevel, level);
		result.maxLevel = max_c(result.maxLevel, level);
	}
	if (result.windows == 0)
		result.minPurity = result.minLevel = 0;
	return result;
}


static double
rms(const int16* samples, uint32 count)
{
	double energy = 0;
	for (uint32 i = 0; i < count; i++)
		energy += (double)samples[i] * samples[i];
	return count > 0 ? sqrt(energy / count) : 0;
}


//!	Asks the harness to feed this stereo tone into the SDL card's input.
static void
feed_tone(int left, int right, int amplitude)
{
	printf("CE-HOST audio_in %d %d %d\n", left, right, amplitude);
	fflush(stdout);
}


/*!	Reports the checks of one capture run: exchanges, record cycles, frame
	counters, real-time pacing and both channels' tones. The first 200 ms
	are skipped: they may still hold what the host's and QEMU's buffers had
	before the tone changed.
*/
static void
report_capture(const char* phase, Stream& stream, Capture& capture,
	double seconds, double left, double right, double amplitude)
{
	char name[64];
	int32 expected = (int32)(seconds * stream.rateHertz
		/ stream.list.return_playback_buffer_size) + 1;

	snprintf(name, sizeof(name), "%s.exchange", phase);
	report(name, capture.error == B_OK && capture.exchanges == expected,
		"%" B_PRId32 "/%" B_PRId32 " exchanges (%s)", capture.exchanges,
		expected, strerror(capture.error));

	// Nearly every exchange delivers one record cycle; the first one or two
	// may not, while the device fills the first capture period.
	snprintf(name, sizeof(name), "%s.record_cycles", phase);
	report(name, capture.recordCycles >= capture.exchanges - 3
			&& capture.badCycles == 0 && capture.badTimes == 0,
		"%" B_PRId32 " record cycles in %" B_PRId32 " exchanges, %" B_PRId32
		" out of order, %" B_PRId32 " with times not increasing",
		capture.recordCycles, capture.exchanges, capture.badCycles,
		capture.badTimes);

	snprintf(name, sizeof(name), "%s.frame_count", phase);
	report(name, capture.badFrameSteps == 0 && capture.recordCycles > 0,
		"%" B_PRId32 " record cycles did not advance by %" B_PRIu32 " frames",
		capture.badFrameSteps, stream.list.return_record_buffer_size);

	double elapsed = (capture.lastTime - capture.firstTime) / 1000000.0;
	double measured = elapsed > 0
		? (capture.lastFrames - capture.firstFrames) / elapsed : 0;
	double ratio = measured / stream.rateHertz;
	snprintf(name, sizeof(name), "%s.pacing", phase);
	report(name, ratio > 0.75 && ratio < 1.25,
		"%.0f frames/s captured, %" B_PRIu32 " nominal (ratio %.3f)",
		measured, stream.rateHertz, ratio);

	uint32 skip = stream.rateHertz / 5;
	ToneCheck leftCheck = check_tone(capture.left, capture.frames, skip,
		stream.rateHertz, left, amplitude);
	ToneCheck rightCheck = check_tone(capture.right, capture.frames, skip,
		stream.rateHertz, right, amplitude);
	int32 minimumWindows = (int32)((seconds - 0.4) * 10);
	snprintf(name, sizeof(name), "%s.signal", phase);
	report(name, leftCheck.windows >= minimumWindows
			&& rightCheck.windows >= minimumWindows
			&& leftCheck.minPurity >= 0.9 && rightCheck.minPurity >= 0.9
			&& leftCheck.minLevel > 0.8 && leftCheck.maxLevel < 1.2
			&& rightCheck.minLevel > 0.8 && rightCheck.maxLevel < 1.2,
		"L %.0f Hz: %" B_PRId32 " windows, purity >= %.3f, level %.2f-%.2f; "
		"R %.0f Hz: %" B_PRId32 " windows, purity >= %.3f, level %.2f-%.2f",
		left, leftCheck.windows, leftCheck.minPurity, leftCheck.minLevel,
		leftCheck.maxLevel, right, rightCheck.windows, rightCheck.minPurity,
		rightCheck.minLevel, rightCheck.maxLevel);
}


/*!	Records about half a second from the card at \a path and returns the
	level of what it delivered (0 if nothing). Used to find the card whose
	input the harness feeds.
*/
static double
probe_input_level(const char* path, bool& hasInput)
{
	hasInput = false;
	Stream stream;
	stream.fd = open(path, O_WRONLY);
	if (stream.fd < 0)
		return 0;

	multi_channel_info channels[MAX_TEST_CHANNELS];
	multi_description description;
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = MAX_TEST_CHANNELS;
	description.channels = channels;
	double level = 0;
	if (ioctl(stream.fd, B_MULTI_GET_DESCRIPTION, &description,
			sizeof(description)) == 0
		&& description.input_channel_count == 2) {
		hasInput = true;
		stream.rateHertz = 48000;
		if (get_buffers(stream, 0, true) == B_OK
			&& stream.list.return_record_buffers > 0) {
			Capture capture(48000);
			record(stream, capture, 0.5);
			level = max_c(rms(capture.left, capture.frames),
				rms(capture.right, capture.frames));
			ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
		}
	}
	close(stream.fd);
	return level;
}


static status_t
set_formats(int fd, uint32 rate, uint32 format, uint32 inputFormat)
{
	multi_format_info formatInfo;
	memset(&formatInfo, 0, sizeof(formatInfo));
	formatInfo.info_size = sizeof(formatInfo);
	formatInfo.output.rate = rate;
	formatInfo.output.format = format;
	formatInfo.input.rate = rate;
	formatInfo.input.format = inputFormat;
	if (ioctl(fd, B_MULTI_SET_GLOBAL_FORMAT, &formatInfo,
			sizeof(formatInfo)) != 0) {
		return errno;
	}
	return B_OK;
}


/*!	The capture checks, on the card whose input the harness feeds. */
static void
test_capture(const char* path)
{
	Stream stream;
	stream.fd = open(path, O_WRONLY);
	report("capture.open", stream.fd >= 0, "%s: %s", path,
		strerror(stream.fd >= 0 ? 0 : errno));
	if (stream.fd < 0)
		return;

	multi_channel_info channels[MAX_TEST_CHANNELS];
	multi_description description;
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = MAX_TEST_CHANNELS;
	description.channels = channels;
	bool ok = ioctl(stream.fd, B_MULTI_GET_DESCRIPTION, &description,
		sizeof(description)) == 0;
	report("capture.description", ok
			&& description.input_channel_count == 2
			&& description.input_bus_channel_count == 2
			&& (description.interface_flags & B_MULTI_INTERFACE_RECORD) != 0
			&& (description.input_rates & B_SR_48000) != 0
			&& (description.input_rates & B_SR_44100) != 0
			&& description.input_formats == B_FMT_16BIT
			&& channels[2].kind == B_MULTI_INPUT_CHANNEL
			&& (channels[2].designations & B_CHANNEL_LEFT) != 0
			&& channels[3].kind == B_MULTI_INPUT_CHANNEL
			&& (channels[3].designations & B_CHANNEL_RIGHT) != 0
			&& channels[6].kind == B_MULTI_INPUT_BUS
			&& channels[7].kind == B_MULTI_INPUT_BUS,
		"in=%" B_PRId32 " in bus=%" B_PRId32 " flags=0x%" B_PRIx32 " rates=0x%"
		B_PRIx32 " formats=0x%" B_PRIx32, description.input_channel_count,
		description.input_bus_channel_count, description.interface_flags,
		description.input_rates, description.input_formats);

	// An input rate different from the output rate is not refused, but the
	// input runs at the output rate, and GET_GLOBAL_FORMAT says so.
	multi_format_info formatInfo;
	memset(&formatInfo, 0, sizeof(formatInfo));
	formatInfo.info_size = sizeof(formatInfo);
	formatInfo.output.rate = B_SR_48000;
	formatInfo.output.format = B_FMT_16BIT;
	formatInfo.input.rate = B_SR_44100;
	formatInfo.input.format = B_FMT_16BIT;
	status_t status = ioctl(stream.fd, B_MULTI_SET_GLOBAL_FORMAT, &formatInfo,
		sizeof(formatInfo)) == 0 ? B_OK : errno;
	memset(&formatInfo, 0, sizeof(formatInfo));
	formatInfo.info_size = sizeof(formatInfo);
	ok = ioctl(stream.fd, B_MULTI_GET_GLOBAL_FORMAT, &formatInfo,
		sizeof(formatInfo)) == 0;
	report("capture.format_follows_output", status == B_OK && ok
			&& formatInfo.input.rate == B_SR_48000
			&& formatInfo.input.format == B_FMT_16BIT
			&& formatInfo.input_latency > 0,
		"set %s; input rate 0x%" B_PRIx32 " format 0x%" B_PRIx32 " latency %"
		B_PRIdBIGTIME " us", strerror(status), formatInfo.input.rate,
		formatInfo.input.format, formatInfo.input_latency);

	// Without record buffers, nothing is recorded and playback works.
	stream.rateHertz = 48000;
	status = get_buffers(stream, 0, false);
	report("capture.playback_only_buffers", status == B_OK
			&& stream.list.return_record_buffers == 0
			&& (stream.list.flags & B_MULTI_BUFFER_RECORD) == 0,
		"%s, %" B_PRId32 " record buffers, flags 0x%" B_PRIx32,
		strerror(status), stream.list.return_record_buffers,
		stream.list.flags);
	if (status == B_OK) {
		Capture capture(48000);
		record(stream, capture, 0.3);
		report("capture.playback_only_exchange", capture.error == B_OK
				&& capture.recordCycles == 0,
			"%" B_PRId32 " exchanges (%s), %" B_PRId32 " record cycles",
			capture.exchanges, strerror(capture.error), capture.recordCycles);
	}

	status = get_buffers(stream, 0, true);
	bool layoutOk = status == B_OK
		&& stream.list.return_record_buffers
			== stream.list.return_playback_buffers
		&& stream.list.return_record_channels == 2
		&& stream.list.return_record_buffer_size
			== stream.list.return_playback_buffer_size
		&& (stream.list.flags & B_MULTI_BUFFER_RECORD) != 0
		&& sRecordBuffers[0][0].stride == 4
		&& sRecordBuffers[0][1].base == sRecordBuffers[0][0].base + 2
		&& sRecordBuffers[1][0].base != stream.buffers[1][0].base;
	report("capture.get_buffers", layoutOk, "%s: %" B_PRId32 " record buffers "
		"x %" B_PRIu32 " frames, %" B_PRId32 " channels", strerror(status),
		stream.list.return_record_buffers,
		stream.list.return_record_buffer_size,
		stream.list.return_record_channels);
	if (!layoutOk) {
		close(stream.fd);
		return;
	}

	Capture capture(4 * 48000);

	// The tone the harness feeds from the start.
	record(stream, capture, 2.0);
	report_capture("capture.phase1", stream, capture, 2.0, 700, 1300, 12000);

	// A different tone: what is recorded follows what the host feeds. The
	// stream keeps running while the host switches.
	feed_tone(440, 880, 8000);
	record(stream, capture, 0.6);
	record(stream, capture, 1.5);
	report_capture("capture.phase2", stream, capture, 1.5, 440, 880, 8000);

	// Force-stop while recording, then record again.
	ok = ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0) == 0;
	report("capture.force_stop", ok, "");
	snooze(200000);
	feed_tone(1000, 500, 16000);
	record(stream, capture, 0.6);
	record(stream, capture, 1.5);
	report_capture("capture.phase3", stream, capture, 1.5, 1000, 500, 16000);

	// 44.1 kHz: QEMU resamples what the host feeds at 48 kHz.
	status = set_formats(stream.fd, B_SR_44100, B_FMT_16BIT, B_FMT_16BIT);
	stream.rateHertz = 44100;
	if (status == B_OK)
		status = get_buffers(stream, 0, true);
	report("capture.buffers_44k", status == B_OK
			&& stream.list.return_record_buffers > 0, "%s", strerror(status));
	if (status == B_OK) {
		record(stream, capture, 0.6);
		record(stream, capture, 1.5);
		report_capture("capture.phase4", stream, capture, 1.5, 1000, 500,
			16000);
	}
	ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
	close(stream.fd);

	// Reopen at 48 kHz with longer periods.
	stream.fd = open(path, O_WRONLY);
	stream.rateHertz = 48000;
	status = stream.fd >= 0
		? set_formats(stream.fd, B_SR_48000, B_FMT_16BIT, B_FMT_16BIT) : errno;
	if (status == B_OK)
		status = get_buffers(stream, 2048, true);
	report("capture.reopen", status == B_OK
			&& stream.list.return_record_buffer_size == 2048,
		"%s, %" B_PRIu32 " frames per period", strerror(status),
		stream.list.return_record_buffer_size);
	if (status == B_OK) {
		feed_tone(600, 1200, 10000);
		record(stream, capture, 0.6);
		record(stream, capture, 1.5);
		report_capture("capture.phase5", stream, capture, 1.5, 600, 1200,
			10000);
		ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0);
	}
	if (stream.fd >= 0)
		close(stream.fd);

	// Back to the default tone for whatever runs next.
	feed_tone(700, 1300, 12000);
}


/*!	On a card whose input nothing feeds (QEMU's wav backend has none),
	capture must not get in the way of playback.
*/
static void
test_unfed_capture(const char* path)
{
	Stream stream;
	stream.fd = open(path, O_WRONLY);
	if (stream.fd < 0) {
		report("capture.unfed_card", false, "open: %s", strerror(errno));
		return;
	}
	stream.rateHertz = 48000;
	status_t status = set_formats(stream.fd, B_SR_48000, B_FMT_16BIT,
		B_FMT_16BIT);
	if (status == B_OK)
		status = get_buffers(stream, 0, true);
	Capture capture(48000);
	bigtime_t started = system_time();
	if (status == B_OK)
		record(stream, capture, 0.5);
	bigtime_t elapsed = system_time() - started;
	bool stopped = ioctl(stream.fd, B_MULTI_BUFFER_FORCE_STOP, NULL, 0) == 0;
	close(stream.fd);
	report("capture.unfed_card", status == B_OK && capture.error == B_OK
			&& stopped && elapsed < 1500000
			&& rms(capture.left, capture.frames) < 50,
		"%s, %" B_PRId32 " exchanges (%s) in %" B_PRIdBIGTIME " us, %"
		B_PRId32 " record cycles, level %.0f, stop %s", strerror(status),
		capture.exchanges, strerror(capture.error), elapsed,
		capture.recordCycles, rms(capture.left, capture.frames),
		stopped ? "ok" : "failed");
}


int
main(int argc, char** argv)
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	char paths[MAX_DEVICES][PATH_MAX];
	int deviceCount = 0;
	find_devices("/dev/audio/hmulti", paths, deviceCount);
	report("device_published", deviceCount > 0, "%d device(s)%s%s",
		deviceCount, deviceCount > 0 ? ", first " : "",
		deviceCount > 0 ? paths[0] : "");
	if (deviceCount == 0) {
		printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
		return 1;
	}

	// Which card is which: the harness feeds the input of one of them (see
	// the file comment). Playback runs on a card whose input stays silent,
	// so that it is the one recording into audio.wav.
	feed_tone(700, 1300, 12000);
	int playbackCard = -1;
	int fedCard = -1;
	int inputCards = 0;
	for (int i = 0; i < deviceCount; i++) {
		bool hasInput;
		double level = probe_input_level(paths[i], hasInput);
		if (hasInput)
			inputCards++;
		printf("CE-INFO %s: %s, input level %.0f\n", paths[i],
			hasInput ? "has capture" : "playback only", level);
		if (hasInput && level > 1000 && fedCard < 0)
			fedCard = i;
		else if (playbackCard < 0)
			playbackCard = i;
	}
	if (playbackCard < 0)
		playbackCard = 0;
	report("capture.cards", deviceCount < 2
			|| (inputCards == deviceCount && fedCard >= 0),
		"%d card(s), %d with an input stream, fed input on %s",
		deviceCount, inputCards, fedCard >= 0 ? paths[fedCard] : "none");

	char path[PATH_MAX];
	strlcpy(path, paths[playbackCard], sizeof(path));
	printf("CE-INFO playback tests on %s\n", path);

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
			&& (description.input_channel_count == 0
				|| description.input_channel_count == 2)
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

	if (fedCard >= 0) {
		test_capture(paths[fedCard]);
		test_unfed_capture(path);
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
