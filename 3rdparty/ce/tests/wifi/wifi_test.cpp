/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Wi-Fi without a Wi-Fi card (roadmap F-34; QEMU has no wireless device
	model, so this is what can be checked in the harness):

	  wifi.groups_mesh        four access points of one network (a mesh with
	                          2.4 and 5 GHz radios) and one other network:
	                          two entries, the strongest access point's
	                          signal shown, the count of access points
	  wifi.groups_security    the same name with other security is another
	                          network
	  wifi.groups_hidden      networks without a name are never folded
	  wifi.groups_associated  an entry is "joined" when any of its access
	                          points is
	  wifi.groups_each        "Show each access point": every access point
	                          its own entry
	  wifi.option_setting     the option's setting written and read back
	                          (~/config/settings/network_wireless; put back
	                          as it was)
	  wifi.wpa_supplicant     Renga images: the wpa_supplicant package is
	                          installed and the supplicant can be launched
	                          (net_server launches it to join a protected
	                          network; Renga images had none, so joining
	                          failed with "Permission denied")

	The grouping code is WirelessNetworkGroups.h, the one NetworkStatus and
	the Network preferences use.
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
#include <WirelessNetworkGroups.h>


#define SUITE "wifi"

using BPrivate::wireless_network_group;

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


static wireless_network
network(const char* name, uint8 lastByte, uint8 signal, uint32 authentication)
{
	wireless_network result;
	result.name[0] = '\0';
	result.noise_level = 0;
	result.flags = 0;
	result.cipher = 0;
	result.group_cipher = 0;
	result.key_mode = 0;
	strlcpy(result.name, name, sizeof(result.name));
	uint8 mac[6] = { 0x02, 0x00, 0x5e, 0x10, 0xe9, lastByte };
	result.address.SetToLinkLevel(mac, 6);
	result.signal_strength = signal;
	result.authentication_mode = authentication;
	return result;
}


static void
test_groups()
{
	const uint32 wpa2 = B_NETWORK_AUTHENTICATION_WPA2;
	wireless_network scan[] = {
		network("Home Mesh", 0x3e, 60, wpa2),
		network("Home Mesh", 0x3f, 72, wpa2),
		network("Neighbour", 0x01, 40, wpa2),
		network("Home Mesh", 0x40, 35, wpa2),
		network("Home Mesh", 0x41, 30, wpa2),
	};
	std::set<BNetworkAddress> associated;
	std::vector<wireless_network_group> groups;
	BPrivate::group_wireless_networks(scan, 5, associated, groups);
	bool mesh = groups.size() == 2
		&& strcmp(groups[0].network.name, "Home Mesh") == 0
		&& groups[0].access_points == 4
		&& groups[0].network.signal_strength == 72
		&& groups[0].network.address == scan[1].address
		&& strcmp(groups[1].network.name, "Neighbour") == 0
		&& groups[1].access_points == 1;
	report("groups_mesh", mesh, "%zu entries; first \"%s\" with %d access "
		"points, signal %u (want 2 entries, \"Home Mesh\" x4 at 72)",
		groups.size(), groups.empty() ? "" : groups[0].network.name,
		groups.empty() ? 0 : (int)groups[0].access_points,
		groups.empty() ? 0 : groups[0].network.signal_strength);

	wireless_network security[] = {
		network("Cafe", 0x01, 50, B_NETWORK_AUTHENTICATION_NONE),
		network("Cafe", 0x02, 55, wpa2),
		network("Cafe", 0x03, 45, wpa2),
	};
	BPrivate::group_wireless_networks(security, 3, associated, groups);
	report("groups_security", groups.size() == 2
			&& groups[0].access_points == 1 && groups[1].access_points == 2,
		"%zu entries (want 2: the open \"Cafe\" and the WPA2 one with 2 "
		"access points)", groups.size());

	wireless_network hidden[] = {
		network("", 0x01, 50, wpa2),
		network("", 0x02, 55, wpa2),
		network("Visible", 0x03, 45, wpa2),
	};
	BPrivate::group_wireless_networks(hidden, 3, associated, groups);
	report("groups_hidden", groups.size() == 3, "%zu entries (want 3: hidden "
		"networks are not folded)", groups.size());

	associated.insert(scan[4].address);
	BPrivate::group_wireless_networks(scan, 5, associated, groups);
	report("groups_associated", groups.size() == 2 && groups[0].associated
			&& !groups[1].associated, "joined to the weakest access point: "
		"the mesh entry is %s, the other %s", groups.size() > 0
			&& groups[0].associated ? "marked" : "not marked",
		groups.size() > 1 && groups[1].associated ? "marked" : "not marked");

	BPrivate::ungrouped_wireless_networks(scan, 5, associated, groups);
	report("groups_each", groups.size() == 5 && groups[4].associated
			&& !groups[0].associated, "%zu entries (want 5, the joined one "
		"marked)", groups.size());
}


static void
test_setting()
{
	BPath path;
	BPrivate::wireless_settings_path(path);
	bool existed = access(path.Path(), F_OK) == 0;
	bool before = BPrivate::wireless_show_access_points();
	BPrivate::set_wireless_show_access_points(true);
	bool on = BPrivate::wireless_show_access_points();
	BPrivate::set_wireless_show_access_points(false);
	bool off = BPrivate::wireless_show_access_points();
	if (existed)
		BPrivate::set_wireless_show_access_points(before);
	else
		unlink(path.Path());
	report("option_setting", !existed ? !before && on && !off : on && !off,
		"default %s, on %s, off %s (%s)", before ? "on" : "off",
		on ? "on" : "off", off ? "on" : "off", path.Path());
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
	test_groups();
	test_setting();
	test_supplicant();
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
