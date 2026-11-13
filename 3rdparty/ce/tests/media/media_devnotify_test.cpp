/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: a new audio device (roadmap F-59; the agent in
	src/preferences/audio/AudioDeviceAgent.h, the policies in
	AudioDevicePolicy.h).

	When the audio devices change, media_server starts the agent
	(/boot/system/servers/audio_device_agent) once they settle; for a new device it applies the
	device's policy (ask, switch automatically, do nothing) to the default
	output and input separately and switches back when a device it switched
	to goes. QEMU's usb-audio (playback only) is plugged in and out through
	the harness ("CE-HOST usb_audio add|remove").

	  devnotify.baseline         the first run records the devices present
	                             and asks nothing
	  devnotify.prompt_shown     plugged in: the prompt "New audio device"
	                             appears (screenshot audio_device_prompt)
	  devnotify.prompt_switch    "Switch to it": the default output is the
	                             new device
	  devnotify.switch_back      pulled out: the default output is the one
	                             before again
	  devnotify.prompt_not_now   plugged in again, "Not now": nothing changes
	  devnotify.policy_set       the Audio preferences' Devices page: "When
	                             connected, as output: Switch to it
	                             automatically" for the device (screenshot
	                             audio_device_policy)
	  devnotify.auto_switch      plugged in: no prompt, the default output
	                             switches by itself
	  devnotify.auto_switch_back and back when it is pulled out
	  devnotify.ignore           "Do nothing": no prompt, no switch
	  devnotify.restore_defaults "Restore defaults" sets every device back
	                             to "Ask"
	  devnotify.input_untouched  the default input never changed (the
	                             device has no input; inputs are handled on
	                             their own)

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <MediaAddOn.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <Roster.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "AudioDevicePolicy.h"


#define SUITE "devnotify"

static const char* kAudioSignature = "application/x-vnd.Renga-Audio";
static const char* kAgentSignature
	= "application/x-vnd.Renga-AudioDeviceAgent";
static const char* kAgentPath = "/boot/system/servers/audio_device_agent";
static const char* kAudioPath = "/boot/system/preferences/Audio";
static const char* kDevice = "QEMU USB Audio";
static const uint32 kMsgControl = 'AuCt';
static const uint32 kMsgPromptSwitch = 'AdSw';
static const uint32 kMsgPromptNotNow = 'AdNn';

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[320];
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


static bool
wait_default_output(const char* name, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		if (default_output_name() == name)
			return true;
		snooze(250000);
	}
	return false;
}


//!	The window named \a title of the Audio preferences, or of the agent
//	for the prompt.
static BMessenger
window(const char* title)
{
	BMessenger app(strcmp(title, "Audio") == 0 ? kAudioSignature
		: kAgentSignature);
	if (!app.IsValid())
		return BMessenger();
	BMessage get(B_GET_PROPERTY);
	get.AddSpecifier("Messenger");
	get.AddSpecifier("Window", title);
	BMessage reply;
	BMessenger result;
	if (app.SendMessage(&get, &reply, 1000000, 1000000) == B_OK)
		reply.FindMessenger("result", &result);
	return result;
}


static BMessenger
wait_window(const char* title, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		BMessenger found = window(title);
		if (found.IsValid())
			return found;
		snooze(250000);
	}
	return BMessenger();
}


static team_id
start(const char* const* args)
{
	const char* argv[8];
	int count = 0;
	argv[count++] = kAudioPath;
	while (args != NULL && *args != NULL && count < 7)
		argv[count++] = *args++;
	argv[count] = NULL;
	pid_t child = fork();
	if (child == 0) {
		execv(kAudioPath, (char* const*)argv);
		_exit(127);
	}
	return child;
}


static status_t
choose(BMessenger& target, const char* name, const char* item)
{
	// the device's settings: select it first (the list is rebuilt when
	// devices come and go)
	BMessage select('AuDe');
	select.AddString("device", kDevice);
	target.SendMessage(&select);
	snooze(500000);
	BMessage message(kMsgControl);
	message.AddString("control", name);
	message.AddString("item", item);
	BMessage reply;
	status_t status = target.SendMessage(&message, &reply, 3000000, 3000000);
	return status == B_OK ? reply.GetInt32("error", B_ERROR) : status;
}


static status_t
press(BMessenger& target, const char* name)
{
	BMessage message(kMsgControl);
	message.AddString("control", name);
	BMessage reply;
	status_t status = target.SendMessage(&message, &reply, 3000000, 3000000);
	return status == B_OK ? reply.GetInt32("error", B_ERROR) : status;
}


static status_t
answer(uint32 what)
{
	BMessenger prompt = window("New audio device");
	if (!prompt.IsValid())
		return B_NAME_NOT_FOUND;
	BMessage message(what);
	message.AddBool("remember", false);
	BMessage reply;
	return prompt.SendMessage(&message, &reply, 5000000, 5000000);
}


//!	Waits until the device is listed by the Media Kit (or gone).
static bool
wait_device(bool present, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		BMediaRoster* roster = BMediaRoster::Roster();
		dormant_node_info infos[64];
		int32 count = 64;
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		bool found = false;
		if (roster->GetDormantNodes(infos, &count, &format, NULL, NULL,
				B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT) == B_OK) {
			for (int32 i = 0; i < count; i++) {
				if (strcmp(infos[i].name, kDevice) == 0)
					found = true;
			}
		}
		if (found == present)
			return true;
		snooze(250000);
	}
	return false;
}


//!	Waits until the agent recorded \a present for the device (it runs about
//	3 s after a device came, 10 s after one went, once they settle).
static bool
wait_known(bool present, bigtime_t timeout = 45000000)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		AudioDevicePolicies policies;
		bool known = false;
		if (policies.Load() == B_OK) {
			const BStringList& outputs = policies.KnownOutputs();
			for (int32 i = 0; i < outputs.CountStrings(); i++) {
				if (outputs.StringAt(i).StartsWith(BString(kDevice) << "\t"))
					known = true;
			}
			if (known == present)
				return true;
		}
		snooze(250000);
	}
	return false;
}


static AudioDevicePolicy
policy()
{
	AudioDevicePolicies policies;
	policies.Load();
	return policies.Get(kDevice);
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Renga-devnotify-test");
	printf("CE-SUITE %s START\n", SUITE);

	// The media services were just started: wait until the devices and
	// the default output are there.
	for (int32 i = 0; i < 120 && default_output_name().IsEmpty(); i++)
		snooze(250000);
	snooze(2000000);
	BString input = default_input_name();

	// 1. A fresh start: the first run only records.
	BString path;
	AudioDevicePolicies::Path(path);
	unlink(path.String());
	pid_t agent = fork();
	if (agent == 0) {
		execl(kAgentPath, kAgentPath, (char*)NULL);
		_exit(127);
	}
	bool recorded = false;
	for (int32 i = 0; i < 40 && !recorded; i++) {
		snooze(250000);
		AudioDevicePolicies policies;
		recorded = policies.Load() == B_OK && policies.HasKnown();
	}
	waitpid(agent, NULL, 0);
	AudioDevicePolicies baseline;
	baseline.Load();
	snooze(500000);
	report("baseline", recorded && baseline.KnownOutputs().CountStrings() > 0
			&& !window("New audio device").IsValid(),
		"%d outputs, %d inputs recorded, %s", (int)baseline.KnownOutputs()
			.CountStrings(), (int)baseline.KnownInputs().CountStrings(),
		window("New audio device").IsValid() ? "a prompt" : "no prompt");
	BString previous = default_output_name();

	// 2. Plugged in: the prompt; "Switch to it".
	host("usb_audio add");
	bool listed = wait_device(true, 20000000);
	BMessenger prompt = wait_window("New audio device", 20000000);
	report("prompt_shown", listed && prompt.IsValid(),
		"device listed %d, prompt %s", listed,
		prompt.IsValid() ? "shown" : "not shown within 20 s");
	if (prompt.IsValid()) {
		snooze(500000);
		host("screenshot audio_device_prompt");
	}
	status_t status = answer(kMsgPromptSwitch);
	bool switched = wait_default_output(kDevice, 10000000);
	report("prompt_switch", status == B_OK && switched,
		"answer 0x%x, default output \"%s\" (was \"%s\")", (unsigned)status,
		default_output_name().String(), previous.String());

	// 3. Pulled out: back.
	host("usb_audio remove");
	wait_device(false, 20000000);
	bool back = wait_default_output(previous, 20000000);
	wait_known(false);
	report("switch_back", back, "default output \"%s\" (want \"%s\")",
		default_output_name().String(), previous.String());

	// 4. Again, "Not now".
	host("usb_audio add");
	wait_device(true, 20000000);
	prompt = wait_window("New audio device", 20000000);
	bool prompted = prompt.IsValid();
	status = answer(kMsgPromptNotNow);
	snooze(3000000);
	report("prompt_not_now", prompted && status == B_OK
			&& default_output_name() == previous,
		"prompt %d, answer 0x%x, default output \"%s\"", prompted,
		(unsigned)status, default_output_name().String());

	// 5. The Audio preferences: switch automatically.
	const char* prefsArgs[] = { "--page", "devices", "--device", kDevice,
		NULL };
	team_id prefs = start(prefsArgs);
	BMessenger audio = wait_window("Audio", 15000000);
	snooze(1500000);
	status = choose(audio, "devicePolicyOutput",
		"Switch to it automatically");
	// (the input left alone: QEMU's usb-audio is listed as an input too)
	if (status == B_OK)
		status = choose(audio, "devicePolicyInput", "Do nothing");
	snooze(500000);
	host("screenshot audio_device_policy");
	report("policy_set", status == B_OK
			&& policy().output == AUDIO_DEVICE_SWITCH,
		"choose 0x%x, policy output %d (want %d)", (unsigned)status,
		(int)policy().output, AUDIO_DEVICE_SWITCH);
	host("usb_audio remove");
	wait_device(false, 20000000);
	// (nothing was switched to it: media_server does not start the agent
	// for this removal; plugged in again, it is new by its flavor id)
	snooze(4000000);

	// 6. Plugged in: switches by itself, and back.
	host("usb_audio add");
	wait_device(true, 20000000);
	switched = wait_default_output(kDevice, 15000000);
	prompted = window("New audio device").IsValid();
	report("auto_switch", switched && !prompted,
		"default output \"%s\", prompt %s", default_output_name().String(),
		prompted ? "shown" : "not shown");
	// "do nothing" for the next time, for both directions, while it is
	// listed
	status_t ignoreStatus = choose(audio, "devicePolicyOutput", "Do nothing");
	if (ignoreStatus == B_OK)
		ignoreStatus = choose(audio, "devicePolicyInput", "Do nothing");
	host("usb_audio remove");
	wait_device(false, 20000000);
	back = wait_default_output(previous, 20000000);
	report("auto_switch_back", back, "default output \"%s\" (want \"%s\")",
		default_output_name().String(), previous.String());
	wait_known(false);

	// 7. Do nothing.
	host("usb_audio add");
	wait_device(true, 20000000);
	snooze(8000000);
	prompted = window("New audio device").IsValid();
	report("ignore", ignoreStatus == B_OK
			&& policy().output == AUDIO_DEVICE_IGNORE && !prompted
			&& default_output_name() == previous,
		"choose 0x%x, policy output %d, prompt %s, default output \"%s\"",
		(unsigned)ignoreStatus, (int)policy().output,
		prompted ? "shown" : "not shown", default_output_name().String());

	// 8. Restore defaults.
	status = press(audio, "devicePolicyDefaults");
	snooze(500000);
	AudioDevicePolicies after;
	after.Load();
	report("restore_defaults", status == B_OK && after.IsDefault()
			&& policy().output == AUDIO_DEVICE_ASK,
		"press 0x%x, policies default %d", (unsigned)status,
		after.IsDefault());
	host("usb_audio remove");
	wait_device(false, 20000000);
	snooze(4000000);

	report("input_untouched", default_input_name() == input,
		"default input \"%s\" (was \"%s\")", default_input_name().String(),
		input.String());

	// The other tests plug this device in and out too; they test routing,
	// not this prompt: "do nothing" for it from here on.
	{
		AudioDevicePolicies policies;
		policies.Load();
		AudioDevicePolicy quiet;
		quiet.output = AUDIO_DEVICE_IGNORE;
		quiet.input = AUDIO_DEVICE_IGNORE;
		policies.Set(kDevice, quiet);
		policies.Save();
	}

	// Close the preferences and any prompt left.
	if (audio.IsValid())
		audio.SendMessage(B_QUIT_REQUESTED);
	for (int32 i = 0; i < 20; i++) {
		if (waitpid(prefs, NULL, WNOHANG) == prefs)
			break;
		snooze(250000);
	}
	kill(prefs, SIGKILL);
	waitpid(prefs, NULL, WNOHANG);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
