/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Audio preferences (roadmap F-32, F-31 phase 6): every page shown
	and saved as a screenshot, and every control checked against what it
	changes, as a click does it (the window's 'AuCt' message: a control's
	value set and invoked, a menu item chosen):

	- Output page: the output device menu (the default output moves to the
	  second card and back), the master volume and mute, the test sound,
	  the buffer size (advanced); the host finds the phases in what the
	  cards played (sound_audio.*, SOUND_PHASES in ce-test.py).
	- Input page: the input device menu, the level meter showing the tone
	  the harness feeds, the recording gain halving the level recorded.
	- Applications page: an application's volume and mute through the
	  loopback, its output device (advanced; host phase), its "Low latency"
	  switch (advanced) in the mixer.
	- Devices page: both cards listed; a USB device plugged in appears in
	  the list and leaves it when it is pulled out (hot-plug).
	- Search: words lead to the right page, advanced settings shown when
	  only they match.

	Runs with the media services up, before media_test (whose state it
	leaves as it found it: default devices, gains, routes, master volume).
	Players are copies of media_test ("Music", "Browser"), as in
	media_ui_test.
*/


#include <Application.h>
#include <Autolock.h>
#include <Locker.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <ParameterWeb.h>
#include <Roster.h>
#include <String.h>

#include <AudioStreamControl.h>

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>


#define SUITE "sound_settings"

using BPrivate::AudioStreamControl;

static const char* kPlayer = "/boot/system/non-packaged/bin/ce-tests-media/"
	"media_test";
static const char* kAudio = "/boot/system/preferences/Audio";
static const char* kAudioSignature = "application/x-vnd.Renga-Audio";
static const char* kMusic = "application/x-vnd.Renga-sound-music";
static const char* kBrowser = "application/x-vnd.Renga-sound-browser";

// media_test's player amplitude
static const double kAmplitude = 0.25;
// the harness's capture tone (CE-HOST audio_in default)
static const double kFedAmplitude = 12000.0 / 32768;

enum {
	kMsgControl		= 'AuCt',
	kMsgState		= 'AuSt',
	kMsgPage		= 'AuPg',
	kMsgAdvanced	= 'AuAv',
	kMsgSearch		= 'AuSe'
};

static int sFailures = 0;
static BMessenger sWindow;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
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
host(const char* request)
{
	printf("CE-HOST %s\n", request);
	fflush(stdout);
}


static void
screenshot(const char* name)
{
	BString request("screenshot ");
	request << name;
	host(request.String());
	snooze(1500000);
}


static void
phase(const char* name)
{
	printf("CE-INFO sound_settings phase %s\n", name);
	fflush(stdout);
}


// #pragma mark - players


static FILE*
start_player(const char* name, const char* signature)
{
	BString copy("/tmp/");
	copy << name;
	BString command;
	command << "cp " << kPlayer << " '" << copy << "' && '" << copy
		<< "' player " << signature;
	return popen(command.String(), "w");
}


static void
tone(FILE* player, int left, int right)
{
	if (player == NULL)
		return;
	fprintf(player, "tone %d %d\n", left, right);
	fflush(player);
}


static void
stop_player(FILE* player)
{
	if (player == NULL)
		return;
	fprintf(player, "quit\n");
	fflush(player);
	pclose(player);
}


// #pragma mark - recording (as media_test)


struct Recording {
	BLocker				lock;
	std::vector<float>	left;
	std::vector<float>	right;
	float				rate;

	Recording() : lock("recording"), rate(0) {}
};


static void
record_buffer(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	Recording* recording = (Recording*)cookie;
	const media_raw_audio_format& raw = format.u.raw_audio;
	size_t sampleSize = raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
	if (sampleSize == 0 || raw.channel_count == 0)
		return;
	size_t frames = size / (sampleSize * raw.channel_count);
	BAutolock locker(recording->lock);
	recording->rate = raw.frame_rate;
	for (size_t i = 0; i < frames; i++) {
		float values[2] = { 0, 0 };
		for (uint32 c = 0; c < 2 && c < raw.channel_count; c++) {
			size_t index = i * raw.channel_count + c;
			switch (raw.format) {
				case media_raw_audio_format::B_AUDIO_FLOAT:
					values[c] = ((float*)data)[index];
					break;
				case media_raw_audio_format::B_AUDIO_SHORT:
					values[c] = ((int16*)data)[index] / 32768.0f;
					break;
				case media_raw_audio_format::B_AUDIO_INT:
					values[c] = ((int32*)data)[index] / 2147483648.0f;
					break;
				default:
					break;
			}
		}
		if (raw.channel_count == 1)
			values[1] = values[0];
		recording->left.push_back(values[0]);
		recording->right.push_back(values[1]);
	}
}


static status_t
record_from(const media_node& node, double seconds, Recording& recording)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_output output;
	int32 count;
	status_t status = roster->GetFreeOutputsFor(node, &output, 1, &count,
		B_MEDIA_RAW_AUDIO);
	if (status != B_OK)
		return status;
	if (count < 1)
		return B_BUSY;
	BMediaRecorder recorder("sound_settings recorder", B_MEDIA_RAW_AUDIO);
	if (recorder.InitCheck() != B_OK)
		return recorder.InitCheck();
	recorder.SetHooks(record_buffer, NULL, &recording);
	media_format format = output.format;
	status = recorder.Connect(node, &output, &format);
	if (status != B_OK)
		return status;
	status = recorder.Start();
	if (status == B_OK) {
		snooze((bigtime_t)(seconds * 1000000));
		recorder.Stop();
	}
	recorder.Disconnect();
	return status;
}


static status_t
record_loopback(const char* signature, double seconds, Recording& recording)
{
	media_node node;
	status_t status = AudioStreamControl::StartLoopback(signature, &node);
	if (status != B_OK)
		return status;
	status = record_from(node, seconds, recording);
	BMediaRoster::Roster()->ReleaseNode(node);
	return status;
}


static double
goertzel(const float* samples, size_t count, double rate, double hertz)
{
	double coefficient = 2 * cos(2 * M_PI * hertz / rate);
	double s1 = 0, s2 = 0;
	for (size_t i = 0; i < count; i++) {
		double s0 = samples[i] + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}


/*!	Amplitude (from the RMS) of a channel after \a skip seconds, and the
	share of its energy at \a hertz.
*/
static double
level(const std::vector<float>& samples, double rate, double skip,
	double hertz, double* _purity = NULL)
{
	size_t start = (size_t)(skip * rate);
	if (samples.size() <= start + 1 || rate <= 0) {
		if (_purity != NULL)
			*_purity = 0;
		return 0;
	}
	double energy = 0;
	for (size_t i = start; i < samples.size(); i++)
		energy += (double)samples[i] * samples[i];
	size_t count = samples.size() - start;
	if (_purity != NULL) {
		*_purity = energy > 0 ? 2 * goertzel(&samples[start], count, rate,
			hertz) / (count * energy) : 0;
	}
	return sqrt(energy / count) * sqrt(2.0);
}


/*!	The gain the system mixer applies for a gain value (its bent "dB"
	scale; media_test's mixer_gain()).
*/
static double
mixer_gain(double db)
{
	double maximum = db > 0 ? 18.0 : 60.0;
	double exponent = db > 0 ? 1.4 : 1.8;
	double scaled = pow(fabs(db) * pow(maximum, 1 / exponent) / maximum,
		exponent);
	return pow(10, (db > 0 ? scaled : -scaled) / 20);
}


// #pragma mark - the Audio preferences


static status_t
control(const char* name, int32 value)
{
	BMessage message(kMsgControl);
	message.AddString("control", name);
	message.AddInt32("value", value);
	BMessage reply;
	status_t status = sWindow.SendMessage(&message, &reply, 3000000,
		3000000);
	return status == B_OK ? reply.GetInt32("error", B_ERROR) : status;
}


static status_t
choose(const char* name, const char* item)
{
	BMessage message(kMsgControl);
	message.AddString("control", name);
	message.AddString("item", item);
	BMessage reply;
	status_t status = sWindow.SendMessage(&message, &reply, 3000000,
		3000000);
	return status == B_OK ? reply.GetInt32("error", B_ERROR) : status;
}


static BMessage
state()
{
	BMessage message(kMsgState);
	BMessage reply;
	if (sWindow.SendMessage(&message, &reply, 3000000, 3000000) != B_OK)
		reply.MakeEmpty();
	return reply;
}


static void
show_page(const char* page)
{
	BMessage message(kMsgPage);
	message.AddString("page", page);
	sWindow.SendMessage(&message);
	snooze(800000);
}


static void
set_advanced(bool advanced)
{
	BMessage message(kMsgAdvanced);
	message.AddBool("advanced", advanced);
	sWindow.SendMessage(&message);
	snooze(800000);
}


static BMessage
search(const char* text)
{
	BMessage message(kMsgSearch);
	message.AddString("text", text);
	sWindow.SendMessage(&message);
	snooze(500000);
	return state();
}


static bool
has_string(const BMessage& message, const char* field, const char* value)
{
	const char* string;
	for (int32 i = 0; message.FindString(field, i, &string) == B_OK; i++) {
		if (strcmp(string, value) == 0)
			return true;
	}
	return false;
}


static BString
strings(const BMessage& message, const char* field)
{
	BString result;
	const char* string;
	for (int32 i = 0; message.FindString(field, i, &string) == B_OK; i++)
		result << (i > 0 ? ", " : "") << string;
	return result;
}


static BString
default_output_name()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	int32 channel;
	BString channelName;
	dormant_node_info info;
	BString name;
	if (roster->GetAudioOutput(&node, &channel, &channelName) == B_OK) {
		if (roster->GetDormantNodeFor(node, &info) == B_OK)
			name = info.name;
		roster->ReleaseNode(node);
	}
	return name;
}


static BString
default_input_name()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	dormant_node_info info;
	BString name;
	if (roster->GetAudioInput(&node) == B_OK) {
		if (roster->GetDormantNodeFor(node, &info) == B_OK)
			name = info.name;
		roster->ReleaseNode(node);
	}
	return name;
}


//!	The name of the device the system mixer's output is connected to.
static BString
mixer_destination_name()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node mixer;
	BString name;
	if (roster->GetAudioMixer(&mixer) != B_OK)
		return name;
	media_output outputs[4];
	int32 count = 0;
	status_t status = roster->GetConnectedOutputsFor(mixer, outputs, 4,
		&count);
	roster->ReleaseNode(mixer);
	if (status != B_OK || count < 1)
		return name;

	// the physical outputs' running nodes, by the port they receive on
	dormant_node_info infos[16];
	int32 infoCount = 16;
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	if (roster->GetDormantNodes(infos, &infoCount, &format, NULL, NULL,
			B_PHYSICAL_OUTPUT) != B_OK) {
		return name;
	}
	for (int32 i = 0; i < infoCount && name.Length() == 0; i++) {
		media_node_id ids[4];
		int32 instances = 4;
		if (roster->GetInstancesFor(infos[i].addon, infos[i].flavor_id, ids,
				&instances) != B_OK) {
			continue;
		}
		for (int32 j = 0; j < instances && name.Length() == 0; j++) {
			media_node node;
			if (roster->GetNodeFor(ids[j], &node) != B_OK)
				continue;
			for (int32 k = 0; k < count; k++) {
				if (outputs[k].destination.port == node.port)
					name = infos[i].name;
			}
			roster->ReleaseNode(node);
		}
	}
	return name;
}


/*!	Waits up to \a timeout for the default output to be \a name and the
	mixer to play on it (the media services reconnect the mixer some time
	after the default changed).
*/
static bool
wait_default_output(const char* name, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		if (default_output_name() == name && mixer_destination_name() == name)
			return true;
		snooze(200000);
	}
	return false;
}


/*!	The default output node's "Buffer size" parameter (frames, 0
	automatic), read from the node itself; -1 if it has none.
*/
static int32
device_buffer_frames()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node output;
	int32 channel;
	BString channelName;
	int32 frames = -1;
	if (roster->GetAudioOutput(&output, &channel, &channelName) != B_OK)
		return frames;
	BParameterWeb* web;
	if (roster->GetParameterWebFor(output, &web) == B_OK) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (strcmp(parameter->Kind(), "renga:buffer_frames") != 0)
				continue;
			int32 value;
			size_t size = sizeof(value);
			bigtime_t when;
			if (parameter->GetValue(&value, &size, &when) == B_OK)
				frames = value;
			break;
		}
		delete web;
	}
	roster->ReleaseNode(output);
	return frames;
}


static int32
wait_buffer_frames(int32 frames, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	int32 value = device_buffer_frames();
	while (value != frames && system_time() < deadline) {
		snooze(200000);
		value = device_buffer_frames();
	}
	return value;
}


//!	Reads, or sets with \a set, the default input's recording gain (dB).
static float
input_gain(float set = NAN)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node input;
	float value = NAN;
	if (roster->GetAudioInput(&input) != B_OK)
		return value;
	BParameterWeb* web;
	if (roster->GetParameterWebFor(input, &web) == B_OK) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (parameter->ID() != 0x52474e01)	// multi_audio's input gain
				continue;
			if (!isnan(set))
				parameter->SetValue(&set, sizeof(set), system_time());
			size_t size = sizeof(value);
			bigtime_t when;
			parameter->GetValue(&value, &size, &when);
			break;
		}
		delete web;
	}
	roster->ReleaseNode(input);
	return value;
}


//!	The mixer's "Resampling algorithm" (PARAM_ETC(70) in AudioMixer.cpp).
static int32
resampling_algorithm()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node mixer;
	int32 value = -1;
	if (roster->GetAudioMixer(&mixer) != B_OK)
		return value;
	BParameterWeb* web;
	if (roster->GetParameterWebFor(mixer, &web) == B_OK) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (parameter->ID() != ((70 << 16) | 0x4))
				continue;
			size_t size = sizeof(value);
			bigtime_t when;
			parameter->GetValue(&value, &size, &when);
			break;
		}
		delete web;
	}
	roster->ReleaseNode(mixer);
	return value;
}


static float
master_gain()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node mixer;
	float value = -1000;
	if (roster->GetAudioMixer(&mixer) != B_OK)
		return value;
	BParameterWeb* web;
	if (roster->GetParameterWebFor(mixer, &web) == B_OK) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter* parameter = web->ParameterAt(i);
			if (strcmp(parameter->Kind(), B_MASTER_GAIN) != 0)
				continue;
			float values[16];
			size_t size = sizeof(values);
			bigtime_t when;
			if (parameter->GetValue(values, &size, &when) == B_OK)
				value = values[0];
			break;
		}
		delete web;
	}
	roster->ReleaseNode(mixer);
	return value;
}


static team_id
start(const char* path, const char* const* args)
{
	const char* argv[8];
	int count = 0;
	argv[count++] = path;
	while (args != NULL && *args != NULL && count < 7)
		argv[count++] = *args++;
	argv[count] = NULL;
	pid_t child = fork();
	if (child == 0) {
		execv(path, (char* const*)argv);
		_exit(127);
	}
	return child;
}


static bool
find_window(team_id team)
{
	for (int i = 0; i < 60; i++) {
		BMessenger app(NULL, team);
		if (app.IsValid()) {
			BMessage get(B_GET_PROPERTY);
			get.AddSpecifier("Messenger");
			get.AddSpecifier("Window", (int32)0);
			BMessage reply;
			if (app.SendMessage(&get, &reply, 2000000, 2000000) == B_OK
				&& reply.FindMessenger("result", &sWindow) == B_OK
				&& sWindow.IsValid()) {
				return true;
			}
		}
		snooze(250000);
	}
	return false;
}


static void
quit(team_id team)
{
	BMessenger messenger(NULL, team);
	if (messenger.IsValid())
		messenger.SendMessage(B_QUIT_REQUESTED);
	for (int i = 0; i < 20; i++) {
		if (waitpid(team, NULL, WNOHANG) == team)
			return;
		snooze(250000);
	}
	kill(team, SIGKILL);
	waitpid(team, NULL, 0);
}


// #pragma mark - checks


static void
check_pages()
{
	const char* pages[] = { "output", "input", "applications", "devices" };
	BString shown;
	bool all = true;
	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		show_page(pages[i]);
		BMessage current = state();
		const char* page = current.GetString("page", "");
		all = all && strcmp(page, pages[i]) == 0;
		shown << (i > 0 ? ", " : "") << page;
		BString name("sound_");
		name << pages[i];
		snooze(700000);
		screenshot(name.String());
	}
	BMessage current = state();
	report("pages", all && current.GetBool("advanced", true) == false,
		"pages shown: %s; advanced %s; sidebar: %s", shown.String(),
		current.GetBool("advanced", false) ? "on" : "off",
		strings(current, "sidebar").String());
}


static void
check_output(FILE* music, FILE* browser, const BString& first,
	const BString& second)
{
	show_page("output");
	BMessage current = state();
	report("default_output_shown",
		first == current.GetString("default_output", ""),
		"page shows \"%s\", the Media Kit's default output is \"%s\"",
		current.GetString("default_output", ""), first.String());

	// Master volume: a reference, -12 on the slider, muted (host phases).
	phase("master_reference");
	tone(music, 4100, 0);
	tone(browser, 0, 4150);
	snooze(1600000);

	status_t status = control("masterVolume", -120);
	snooze(300000);
	float gain = master_gain();
	report("master_volume", status == B_OK && fabsf(gain + 12.0f) < 0.2f,
		"slider set to -12.0: %s; the mixer's master gain is %.2f",
		strerror(status), gain);
	phase("master_volume");
	tone(music, 4200, 0);
	tone(browser, 0, 4250);
	snooze(1600000);

	status = control("masterMute", 1);
	snooze(300000);
	phase("master_mute");
	tone(music, 4300, 0);
	tone(browser, 0, 4350);
	snooze(1200000);
	report("master_mute", status == B_OK, "Mute checked: %s",
		strerror(status));
	// silence first: the host finds the muted phase only if it never
	// plays
	tone(music, 0, 0);
	tone(browser, 0, 0);
	snooze(300000);
	control("masterMute", 0);
	control("masterVolume", 0);
	snooze(400000);

	// The output device menu.
	status = choose("outputDevice", second.String());
	bool moved = status == B_OK && wait_default_output(second.String(),
		20000000);
	snooze(1500000);
	phase("default_output_second");
	tone(music, 4400, 0);
	tone(browser, 0, 4450);
	snooze(1600000);
	current = state();
	report("output_device_menu", moved
			&& second == current.GetString("default_output", ""),
		"chose \"%s\": %s; the Media Kit's default output is \"%s\" (the "
		"mixer plays on \"%s\"), the page shows \"%s\"", second.String(),
		strerror(status), default_output_name().String(),
		mixer_destination_name().String(),
		current.GetString("default_output", ""));
	screenshot("sound_output_second");

	status = choose("outputDevice", first.String());
	moved = status == B_OK && wait_default_output(first.String(), 45000000);
	snooze(1500000);
	phase("default_output_back");
	tone(music, 4500, 0);
	tone(browser, 0, 4550);
	snooze(1600000);
	report("output_device_back", moved, "chose \"%s\" again: %s; the mixer "
		"plays on \"%s\"", first.String(), strerror(status),
		mixer_destination_name().String());

	// The test sound plays through the mixer as the Audio preferences'
	// own stream.
	tone(music, 0, 0);
	tone(browser, 0, 0);
	status = control("testSound", 0);
	Recording recording;
	snooze(150000);
	status_t recorded = record_loopback(kAudioSignature, 0.9, recording);
	double purity;
	double amplitude = level(recording.left, recording.rate, 0.2, 1250,
		&purity);
	report("test_sound", status == B_OK && recorded == B_OK
			&& amplitude > 0.15 && purity > 0.9,
		"button: %s; the Audio preferences' loopback: %s, 1250 Hz at %.3f "
		"(want about 0.25), purity %.3f", strerror(status),
		strerror(recorded), amplitude, purity);
	snooze(1000000);

	// Buffer size (advanced): 512 frames and back to automatic.
	set_advanced(true);
	current = state();
	BString choice512;
	BString automatic;
	const char* choice;
	for (int32 i = 0; current.FindString("output_buffer_choice", i, &choice)
			== B_OK; i++) {
		if (strncmp(choice, "512 ", 4) == 0)
			choice512 = choice;
		if (i == 0)
			automatic = choice;
	}
	status = choose("outputBufferSize", choice512.String());
	int32 frames = wait_buffer_frames(512, 5000000);
	snooze(500000);
	current = state();
	int32 shown = -1;
	const char* device;
	for (int32 i = 0; current.FindString("device", i, &device) == B_OK;
			i++) {
		if (first == device)
			shown = current.GetInt32("device_buffer_frames", i, -1);
	}
	screenshot("sound_output_advanced");
	status_t back = choose("outputBufferSize", automatic.String());
	int32 automaticFrames = wait_buffer_frames(0, 5000000);
	report("buffer_size", status == B_OK && frames == 512 && shown == 512
			&& back == B_OK && automaticFrames == 0,
		"chose \"%s\": %s; the card's buffer size parameter is %" B_PRId32
		", the page shows %" B_PRId32 "; \"%s\" again: %s, parameter %"
		B_PRId32, choice512.String(), strerror(status), frames, shown,
		automatic.String(), strerror(back), automaticFrames);
	snooze(1000000);

	// Resampling (advanced): the mixer's algorithm follows the menu.
	status = choose("resampling", "High quality (linear interpolation)");
	snooze(800000);
	int32 linear = resampling_algorithm();
	status_t best = choose("resampling", "Best quality (windowed sinc)");
	snooze(800000);
	int32 sinc = resampling_algorithm();
	report("resampling_menu", status == B_OK && best == B_OK && linear == 2
			&& sinc == 3,
		"linear interpolation: %s, the mixer's setting %" B_PRId32 "; windowed "
		"sinc: %s, %" B_PRId32, strerror(status), linear, strerror(best),
		sinc);
	set_advanced(false);
}


static void
check_applications(FILE* music, FILE* browser, const BString& second)
{
	show_page("applications");
	BString volume("volume:");
	volume << kMusic;
	BString mute("mute:");
	mute << kMusic;

	tone(music, 4700, 0);
	tone(browser, 0, 0);
	snooze(600000);
	Recording before;
	record_loopback(kMusic, 1.0, before);
	double reference = level(before.left, before.rate, 0.3, 4700);

	status_t status = control(volume.String(), -120);
	snooze(800000);
	Recording after;
	status_t recorded = record_loopback(kMusic, 1.0, after);
	double quieter = level(after.left, after.rate, 0.3, 4700);
	double ratio = reference > 0 ? quieter / reference : 0;
	double expected = mixer_gain(-12);
	report("app_volume", status == B_OK && recorded == B_OK
			&& fabs(ratio - expected) < 0.04,
		"Music's slider set to -12.0: %s; level %.3f -> %.3f, ratio %.3f "
		"(want %.3f, the mixer's scale)", strerror(status), reference,
		quieter, ratio, expected);
	screenshot("sound_applications_volume");

	status = control(mute.String(), 1);
	snooze(800000);
	Recording muted;
	recorded = record_loopback(kMusic, 1.0, muted);
	double silent = level(muted.left, muted.rate, 0.3, 4700);
	report("app_mute", status == B_OK && recorded == B_OK && silent < 0.002,
		"Music's Mute checked: %s; level %.4f", strerror(status), silent);
	control(mute.String(), 0);
	control(volume.String(), 0);
	snooze(500000);

	// Advanced: Browser's output device (host phase), Music's low latency.
	set_advanced(true);
	snooze(1200000);
	BString output("output:");
	output << kBrowser;
	status = choose(output.String(), second.String());
	snooze(1500000);
	phase("app_output");
	tone(music, 4600, 0);
	tone(browser, 0, 4650);
	snooze(1600000);
	AudioStreamControl streams;
	int32 routed = -1;
	if (streams.Update() == B_OK && streams.FindStream(kBrowser) != NULL)
		routed = streams.FindStream(kBrowser)->output;
	report("app_output_menu", status == B_OK && routed > 0
			&& streams.Outputs().StringAt(routed) == second,
		"Browser's output set to \"%s\": %s; the mixer routes it to \"%s\"",
		second.String(), strerror(status), routed >= 0
			? streams.Outputs().StringAt(routed).String() : "-");

	BString lowLatency("lowLatency:");
	lowLatency << kMusic;
	status = control(lowLatency.String(), 1);
	snooze(800000);
	bool on = streams.Update() == B_OK && streams.FindStream(kMusic) != NULL
		&& streams.FindStream(kMusic)->lowLatency;
	screenshot("sound_applications_advanced");
	report("app_low_latency", status == B_OK && on,
		"Music's Low latency checked: %s; the mixer's switch is %s",
		strerror(status), on ? "on" : "off");

	control(lowLatency.String(), 0);
	choose(output.String(), streams.Outputs().StringAt(0).String());
	snooze(800000);
	set_advanced(false);
	tone(music, 0, 0);
	tone(browser, 0, 0);
}


static void
check_input(const BString& original)
{
	show_page("input");
	BMessage current = state();
	BStringList inputs;
	const char* device;
	for (int32 i = 0; current.FindString("device", i, &device) == B_OK;
			i++) {
		if (current.GetBool("device_input", i, false))
			inputs.Add(device);
	}
	report("default_input_shown",
		original == current.GetString("default_input", ""),
		"page shows \"%s\", the Media Kit's default input is \"%s\"; inputs: "
		"%" B_PRId32, current.GetString("default_input", ""),
		original.String(), inputs.CountStrings());

	// The harness feeds only one card's input: the meter finds it.
	BString fed;
	int32 fedLevel = 0;
	BString levels;
	for (int32 i = 0; i < inputs.CountStrings(); i++) {
		status_t status = choose("inputDevice",
			inputs.StringAt(i).String());
		// the meter starts with the next refresh (once a second)
		snooze(3500000);
		int32 value = state().GetInt32("input_level", 0);
		levels << (i > 0 ? ", " : "") << inputs.StringAt(i) << " "
			<< value << " (" << strerror(status) << ")";
		if (value > fedLevel) {
			fedLevel = value;
			fed = inputs.StringAt(i);
		}
	}
	if (fed.Length() > 0)
		choose("inputDevice", fed.String());
	snooze(3000000);
	int32 meter = state().GetInt32("input_level", 0);
	screenshot("sound_input");
	// the fed tone's peak is 12000 / 32768 = 0.366
	report("input_meter", fabs(meter / 1000.0 - kFedAmplitude) < 0.08,
		"meter on each input: %s; on \"%s\": %.3f (the fed tone's peak "
		"%.3f)", levels.String(), fed.String(), meter / 1000.0,
		kFedAmplitude);

	// Recording gain: -6 dB halves what the input records. The meter holds
	// the input while the page is shown, so record from another page.
	status_t status = control("recordingGain", -60);
	snooze(500000);
	show_page("output");
	snooze(800000);
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node input;
	Recording gained;
	status_t recorded = roster->GetAudioInput(&input);
	if (recorded == B_OK) {
		recorded = record_from(input, 1.0, gained);
		roster->ReleaseNode(input);
	}
	double amplitude = level(gained.left, gained.rate, 0.3, 700);
	double ratio = amplitude / kFedAmplitude;
	report("recording_gain", status == B_OK && recorded == B_OK
			&& fabs(ratio - 0.5) < 0.06,
		"slider set to -6.0 dB: %s; recorded %s, level %.3f, %.3f of the "
		"fed tone (want 0.5)", strerror(status), strerror(recorded),
		amplitude, ratio);

	show_page("input");
	control("recordingGain", 0);
	// the card's gain back at 0 dB (later suites record from it)
	float back = input_gain();
	for (int i = 0; i < 30 && fabs(back) > 0.05f; i++) {
		snooze(100000);
		back = input_gain();
	}
	report("recording_gain_back", fabs(back) <= 0.05f, "slider set to 0 dB "
		"again: the input's gain is %.1f dB", back);
	if (fabs(back) > 0.05f)
		input_gain(0.0f);
	if (original.Length() > 0)
		choose("inputDevice", original.String());
	snooze(1000000);
}


static void
check_devices(const BString& first, const BString& second)
{
	show_page("devices");
	BMessage current = state();
	report("devices_listed", has_string(current, "device_listed", first)
			&& has_string(current, "device_listed", second),
		"listed: %s", strings(current, "device_listed").String());

	set_advanced(true);
	screenshot("sound_devices_advanced");
	set_advanced(false);

	// Hot-plug: QEMU's usb-audio comes and goes.
	host("usb_audio add");
	bigtime_t started = system_time();
	bool listed = false;
	while (!listed && system_time() - started < 20000000) {
		snooze(250000);
		listed = has_string(state(), "device_listed", "QEMU USB Audio");
	}
	bigtime_t appeared = system_time() - started;
	// let the media services finish with the new card (its node, the
	// defaults) before it is pulled out again, as a person would
	snooze(5000000);
	screenshot("sound_devices_usb");
	report("hotplug_listed", listed, "QEMU USB Audio %s %.1f s after the "
		"plug-in; listed: %s", listed ? "listed" : "not listed within",
		appeared / 1000000.0, strings(state(), "device_listed").String());

	host("usb_audio remove");
	started = system_time();
	bool gone = false;
	while (!gone && system_time() - started < 30000000) {
		snooze(250000);
		gone = !has_string(state(), "device_listed", "QEMU USB Audio");
	}
	report("hotplug_removed", gone, "QEMU USB Audio %s %.1f s after the "
		"removal", gone ? "gone" : "still listed",
		(system_time() - started) / 1000000.0);
}


static void
check_search()
{
	BMessage result = search("microphone");
	report("search_input", strcmp(result.GetString("page", ""), "input") == 0
			&& !result.GetBool("advanced", true)
			&& strings(result, "sidebar") == "input",
		"\"microphone\": page %s, sidebar [%s], advanced %s",
		result.GetString("page", ""), strings(result, "sidebar").String(),
		result.GetBool("advanced", false) ? "on" : "off");
	snooze(500000);

	result = search("low latency");
	report("search_advanced",
		strcmp(result.GetString("page", ""), "applications") == 0
			&& result.GetBool("advanced", false),
		"\"low latency\": page %s, sidebar [%s], advanced %s",
		result.GetString("page", ""), strings(result, "sidebar").String(),
		result.GetBool("advanced", false) ? "on" : "off");
	screenshot("sound_search");

	result = search("");
	report("search_cleared", strings(result, "sidebar")
			== "output, input, applications, devices",
		"sidebar [%s]", strings(result, "sidebar").String());
	set_advanced(false);
}


// #pragma mark -


int
main()
{
	signal(SIGPIPE, SIG_IGN);
	BApplication app("application/x-vnd.Renga-sound-settings-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	bool desktop = false;
	for (int i = 0; i < 300 && !desktop; i++) {
		desktop = be_roster->IsRunning("application/x-vnd.Be-input_server");
		if (!desktop)
			snooze(1000000);
	}
	if (!desktop) {
		printf("CE-INFO sound_settings: no desktop, nothing to show\n");
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}

	// The media services were just started (or restarted by the test
	// before): wait for the mixer and the default output.
	BMediaRoster* roster = BMediaRoster::Roster();
	bool ready = false;
	for (int i = 0; i < 120 && !ready; i++) {
		media_node mixer;
		if (roster != NULL && roster->GetAudioMixer(&mixer) == B_OK) {
			roster->ReleaseNode(mixer);
			ready = default_output_name().Length() > 0;
		}
		if (!ready)
			snooze(500000);
	}
	snooze(2000000);

	// The two virtio cards, in the Media Kit's order; the default output
	// is the first.
	BString first = default_output_name();
	BString originalInput = default_input_name();
	AudioStreamControl streams;
	BString second;
	FILE* music = start_player("Music", kMusic);
	FILE* browser = start_player("Browser", kBrowser);
	tone(music, 0, 0);
	tone(browser, 0, 0);
	bool found = false;
	for (int i = 0; i < 100 && !found; i++) {
		snooze(200000);
		found = streams.Update() == B_OK && streams.FindStream(kMusic) != NULL
			&& streams.FindStream(kBrowser) != NULL;
	}
	for (int32 i = 1; found && i < streams.Outputs().CountStrings(); i++) {
		if (streams.Outputs().StringAt(i) != first) {
			second = streams.Outputs().StringAt(i);
			break;
		}
	}
	float originalMaster = master_gain();
	report("setup", found && first.Length() > 0 && second.Length() > 0,
		"players %s; default output \"%s\", second card \"%s\", default "
		"input \"%s\", master gain %.1f", found ? "playing" : "not found",
		first.String(), second.String(), originalInput.String(),
		originalMaster);

	team_id audio = start(kAudio, NULL);
	bool window = find_window(audio);
	report("window", window, "the Audio preferences' window %s",
		window ? "is up" : "did not come up");
	if (window && found && second.Length() > 0) {
		snooze(2000000);
		check_pages();
		check_output(music, browser, first, second);
		check_applications(music, browser, second);
		check_input(originalInput);
		check_devices(first, second);
		check_search();
	}
	quit(audio);

	// The command line on a fresh start: a page and the advanced settings.
	const char* args[] = { "--page", "applications", "--advanced", NULL };
	audio = start(kAudio, args);
	window = find_window(audio);
	snooze(2500000);
	BMessage current = window ? state() : BMessage();
	report("command_line", window
			&& strcmp(current.GetString("page", ""), "applications") == 0
			&& current.GetBool("advanced", false),
		"Audio --page applications --advanced: page %s, advanced %s",
		current.GetString("page", "-"),
		current.GetBool("advanced", false) ? "on" : "off");
	if (window)
		set_advanced(false);
	quit(audio);

	// Leave things as they were for media_test.
	if (default_output_name() != first) {
		AudioStreamControl unused;
		BMediaRoster* roster = BMediaRoster::Roster();
		dormant_node_info infos[16];
		int32 count = 16;
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		if (roster->GetDormantNodes(infos, &count, &format, NULL, NULL,
				B_PHYSICAL_OUTPUT) == B_OK) {
			for (int32 i = 0; i < count; i++) {
				if (first == infos[i].name)
					roster->SetAudioOutput(infos[i]);
			}
		}
	}
	stop_player(music);
	stop_player(browser);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
