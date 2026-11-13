/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Wi-Fi without a Wi-Fi card (roadmap F-34; QEMU has no wireless device
	model, so this is what can be checked in the harness):

	  wifi.wpa_supplicant     Renga images: the wpa_supplicant package is
	                          installed and the supplicant can be launched
	                          (net_server launches it to join a protected
	                          network; Renga images had none, so joining
	                          failed with "Permission denied")
*/


#include <Application.h>
#include <Messenger.h>
#include <NetworkDevice.h>
#include <Roster.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <WPASupplicant.h>


#define SUITE "wifi"

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
test_supplicant()
{
	// Renga images (the renga-* profiles; boot volume "Renga") carry the
	// nightly packages; the harness's minimum images do not.
	BVolume boot;
	char volumeName[B_FILE_NAME_LENGTH] = "";
	if (BVolumeRoster().GetBootVolume(&boot) != B_OK
		|| boot.GetName(volumeName) != B_OK
		|| strcmp(volumeName, "Renga") != 0) {
		printf("CE-INFO wifi: not a Renga image (boot volume \"%s\"), "
			"wpa_supplicant not expected\n", volumeName);
		return;
	}
	bool package = false;
	DIR* packages = opendir("/boot/system/packages");
	while (packages != NULL) {
		dirent* entry = readdir(packages);
		if (entry == NULL)
			break;
		if (strncmp(entry->d_name, "wpa_supplicant-", 15) == 0)
			package = true;
	}
	if (packages != NULL)
		closedir(packages);
	bool binary = access("/boot/system/bin/wpa_supplicant", X_OK) == 0;
	team_id team = -1;
	status_t status = be_roster->Launch(kWPASupplicantSignature,
		(BMessage*)NULL, &team);
	bool running = false;
	for (int i = 0; i < 50 && (status == B_OK || status == B_ALREADY_RUNNING)
			&& !running; i++) {
		running = BMessenger(kWPASupplicantSignature).IsValid();
		if (!running)
			snooze(100000);
	}
	if (status == B_OK && team >= 0) {
		BMessenger(NULL, team).SendMessage(B_QUIT_REQUESTED);
		snooze(500000);
		kill(team, SIGKILL);
	}
	report("wpa_supplicant", package && binary && running, "package %s, "
		"binary %s, launch %s, %s", package ? "installed" : "missing",
		binary ? "there" : "missing", strerror(status),
		running ? "running" : "not running");
}


int
main()
{
	BApplication app("application/x-vnd.Renga-wifi-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);
	test_supplicant();
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
