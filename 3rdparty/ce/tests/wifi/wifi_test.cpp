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

	The Deskbar network icon (NetworkStatus):

	  wifi.icon_logic         the icon for each kind of link and state:
	                          Wi-Fi bars by signal, the connecting
	                          animation, Wi-Fi off, wired and USB
	                          tethering, the "!" and lock badges, the cross
	                          (NetworkStatusIcon.h)
	  wifi.icon_kind          which interfaces are USB tethering or a VPN,
	                          and the band of a frequency
	  wifi.icons_hvif         every icon in NetworkStatus's resources is
	                          HVIF that draws (NetworkStatusIconsRenga.rdef)
	  wifi.connectivity_reply the internet check's verdict on replies
	                          (204, Mozilla's "success", a portal's redirect
	                          or page) and its URL parsing
	  wifi.connectivity_local the check against a local server: online,
	                          portal, nothing listening; and its settings
	                          read and written (NetworkConnectivity.h)
	  wifi.status_state       "NetworkStatus --state": the harness has no
	                          network card, so the icon is the greyed plug
	                          with the cross, the tooltip says so and the
	                          menu has "Network preferences..."; the check's
	                          switch is followed

	The grouping code is WirelessNetworkGroups.h, the one NetworkStatus and
	the Network preferences use.
*/


#include <Application.h>
#include <Bitmap.h>
#include <File.h>
#include <IconUtils.h>
#include <Messenger.h>
#include <NetworkDevice.h>
#include <Resources.h>
#include <Roster.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <String.h>

#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <NetworkConnectivity.h>
#include <NetworkStatusIcon.h>
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


//	#pragma mark - the Deskbar network icon


using namespace BPrivate;


static void
test_icon_logic()
{
	struct {
		network_link_kind	kind;
		network_link_state	state;
		int32				signal;
		int32				frame;
		bool				tunnel;
		int32				base;
		int32				badge;
	} cases[] = {
		// Wi-Fi: bars by signal (unknown: full)
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, 90, 0, false,
			kNetworkIconWifi0 + 4, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, 60, 0, false,
			kNetworkIconWifi0 + 3, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, 30, 0, false,
			kNetworkIconWifi0 + 2, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, 5, 0, false,
			kNetworkIconWifi0 + 1, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, 0, 0, false,
			kNetworkIconWifi0, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, -1, 0, false,
			kNetworkIconWifi0 + 4, kNetworkIconNone },
		// connecting: the bars fill, frame by frame, and start over
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTING, 90, 0, false,
			kNetworkIconWifi0, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTING, 90, 3, false,
			kNetworkIconWifi0 + 3, kNetworkIconNone },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTING, 90, 6, false,
			kNetworkIconWifi0 + 1, kNetworkIconNone },
		// no internet, VPN (no internet wins), disconnected, off
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_NO_INTERNET, 60, 0, false,
			kNetworkIconWifi0 + 3, kNetworkIconNoInternet },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_CONNECTED, 60, 0, true,
			kNetworkIconWifi0 + 3, kNetworkIconLock },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_NO_INTERNET, 60, 0, true,
			kNetworkIconWifi0 + 3, kNetworkIconNoInternet },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_DISCONNECTED, 60, 0, true,
			kNetworkIconWifi0, kNetworkIconCross },
		{ NETWORK_LINK_WIRELESS, NETWORK_STATE_RADIO_OFF, 60, 0, false,
			kNetworkIconWifiOff, kNetworkIconNone },
		// wired: no bars; connecting blinks
		{ NETWORK_LINK_WIRED, NETWORK_STATE_CONNECTED, 0, 0, false,
			kNetworkIconWired, kNetworkIconNone },
		{ NETWORK_LINK_WIRED, NETWORK_STATE_CONNECTING, 0, 0, false,
			kNetworkIconWiredDim, kNetworkIconNone },
		{ NETWORK_LINK_WIRED, NETWORK_STATE_CONNECTING, 0, 1, false,
			kNetworkIconWired, kNetworkIconNone },
		{ NETWORK_LINK_WIRED, NETWORK_STATE_NO_INTERNET, 0, 0, false,
			kNetworkIconWired, kNetworkIconNoInternet },
		{ NETWORK_LINK_WIRED, NETWORK_STATE_DISCONNECTED, 0, 0, false,
			kNetworkIconWiredDim, kNetworkIconCross },
		{ NETWORK_LINK_WIRED, NETWORK_STATE_CONNECTED, 0, 0, true,
			kNetworkIconWired, kNetworkIconLock },
		// USB tethering
		{ NETWORK_LINK_TETHER, NETWORK_STATE_CONNECTED, 0, 0, false,
			kNetworkIconTether, kNetworkIconNone },
		{ NETWORK_LINK_TETHER, NETWORK_STATE_DISCONNECTED, 0, 0, false,
			kNetworkIconTetherDim, kNetworkIconCross },
		{ NETWORK_LINK_TETHER, NETWORK_STATE_NO_INTERNET, 0, 0, false,
			kNetworkIconTether, kNetworkIconNoInternet },
	};
	int count = sizeof(cases) / sizeof(cases[0]);
	int wrong = 0;
	BString first;
	for (int i = 0; i < count; i++) {
		network_icon icon = network_icon_for(cases[i].kind, cases[i].state,
			cases[i].signal, cases[i].frame, cases[i].tunnel);
		if (icon.base != cases[i].base || icon.badge != cases[i].badge) {
			if (wrong++ == 0) {
				first.SetToFormat("; case %d: %" B_PRId32 "+%" B_PRId32
					", want %" B_PRId32 "+%" B_PRId32, i, icon.base,
					icon.badge, cases[i].base, cases[i].badge);
			}
		}
	}
	report("icon_logic", wrong == 0, "%d of %d cases right%s", count - wrong,
		count, first.String());
}


static void
test_icon_kind()
{
	bool tether = network_link_kind_for("/dev/net/usb_rndis/0", false)
			== NETWORK_LINK_TETHER
		&& network_link_kind_for("/dev/net/usb_ncm/0", false)
			== NETWORK_LINK_TETHER;
	bool wired = network_link_kind_for("/dev/net/usb_ecm/0", false)
			== NETWORK_LINK_WIRED
		&& network_link_kind_for("/dev/net/ipro1000/0", false)
			== NETWORK_LINK_WIRED;
	bool wireless = network_link_kind_for("/dev/net/mt7921e/0", true)
		== NETWORK_LINK_WIRELESS;
	bool tunnel = network_is_tunnel("tun/0") && network_is_tunnel("tap/1")
		&& !network_is_tunnel("/dev/net/virtio/0")
		&& !network_is_tunnel(NULL);
	const char* b24 = network_band_name(2437);
	const char* b5 = network_band_name(5180);
	const char* b6 = network_band_name(5955);
	bool bands = b24 != NULL && strcmp(b24, "2.4 GHz") == 0 && b5 != NULL
		&& strcmp(b5, "5 GHz") == 0 && b6 != NULL && strcmp(b6, "6 GHz") == 0
		&& network_band_name(0) == NULL;
	report("icon_kind", tether && wired && wireless && tunnel && bands,
		"tethering %s, wired %s, Wi-Fi %s, VPN %s, bands %s/%s/%s",
		tether ? "yes" : "no", wired ? "yes" : "no", wireless ? "yes" : "no",
		tunnel ? "yes" : "no", b24, b5, b6);
}


static void
test_icons_hvif()
{
	BFile file("/boot/system/apps/NetworkStatus", B_READ_ONLY);
	BResources resources(&file);
	int good = 0;
	BString bad;
	for (int32 i = 0; i < kNetworkIconCount; i++) {
		size_t size = 0;
		const void* data = resources.InitCheck() == B_OK
			? resources.LoadResource(B_VECTOR_ICON_TYPE, kNetworkIconFirst + i,
				&size) : NULL;
		BBitmap bitmap(BRect(0, 0, 31, 31), B_RGBA32);
		bool drawn = false;
		if (data != NULL && BIconUtils::GetVectorIcon((const uint8*)data,
				size, &bitmap) == B_OK) {
			// something visible was drawn
			const uint8* bits = (const uint8*)bitmap.Bits();
			int opaque = 0;
			for (int32 p = 0; p < (int32)bitmap.BitsLength(); p += 4)
				opaque += bits[p + 3] > 0x40 ? 1 : 0;
			drawn = opaque >= 8;
		}
		if (drawn)
			good++;
		else
			bad << " " << kNetworkIconFirst + i;
	}
	report("icons_hvif", good == kNetworkIconCount, "%d of %d icons draw%s%s",
		good, (int)kNetworkIconCount, bad.IsEmpty() ? "" : "; not:",
		bad.String());
}


static void
test_connectivity_reply()
{
	struct {
		const char*			reply;
		connectivity_result	want;
	} cases[] = {
		{ "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
			CONNECTIVITY_ONLINE },
		{ "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nsuccess\n",
			CONNECTIVITY_ONLINE },
		{ "HTTP/1.0 302 Found\r\nLocation: http://portal/login\r\n\r\n",
			CONNECTIVITY_CAPTIVE },
		{ "HTTP/1.1 200 OK\r\n\r\n<html>Sign in to the hotel Wi-Fi</html>",
			CONNECTIVITY_CAPTIVE },
		{ "garbage", CONNECTIVITY_CAPTIVE },
		{ "", CONNECTIVITY_OFFLINE },
	};
	int count = sizeof(cases) / sizeof(cases[0]);
	int right = 0;
	for (int i = 0; i < count; i++) {
		if (connectivity_result_for_reply(cases[i].reply,
				strlen(cases[i].reply)) == cases[i].want)
			right++;
	}
	BString host, path;
	uint16 port = 0;
	bool url = parse_connectivity_url(kDefaultConnectivityURL, host, port,
			path)
		&& host == "detectportal.firefox.com" && port == 80
		&& path == "/success.txt";
	BString host2, path2;
	uint16 port2 = 0;
	url = url && parse_connectivity_url("http://127.0.0.1:8080", host2, port2,
			path2)
		&& host2 == "127.0.0.1" && port2 == 8080 && path2 == "/"
		&& !parse_connectivity_url("https://example.org/", host2, port2,
			path2);
	report("connectivity_reply", right == count && url, "%d of %d replies "
		"judged right; URLs %s", right, count, url ? "parsed" : "wrong");
}


struct local_server {
	int			fd;
	const char*	reply;
	int32		requests;
};


static status_t
serve(void* data)
{
	local_server* server = (local_server*)data;
	while (true) {
		int client = accept(server->fd, NULL, NULL);
		if (client < 0)
			break;
		char request[1024];
		ssize_t length = recv(client, request, sizeof(request) - 1, 0);
		if (length > 0) {
			atomic_add(&server->requests, 1);
			send(client, server->reply, strlen(server->reply), 0);
		}
		close(client);
	}
	return B_OK;
}


//!	A one-thread HTTP server on 127.0.0.1; returns its port, or 0.
static uint16
start_server(local_server& server, thread_id& thread)
{
	server.fd = socket(AF_INET, SOCK_STREAM, 0);
	server.requests = 0;
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (server.fd < 0
		|| bind(server.fd, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(server.fd, 4) != 0
		|| getsockname(server.fd, (sockaddr*)&address, &length) != 0)
		return 0;
	thread = spawn_thread(serve, "http server", B_NORMAL_PRIORITY, &server);
	resume_thread(thread);
	return ntohs(address.sin_port);
}


static void
stop_server(local_server& server, thread_id thread)
{
	shutdown(server.fd, SHUT_RDWR);
	close(server.fd);
	status_t result;
	wait_for_thread(thread, &result);
}


static void
test_connectivity_local()
{
	static const char* kReplies[] = {
		"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n",
		"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nsuccess\n",
		"HTTP/1.1 302 Found\r\nLocation: http://10.0.0.1/\r\n\r\n",
	};
	static const connectivity_result kWant[] = { CONNECTIVITY_ONLINE,
		CONNECTIVITY_ONLINE, CONNECTIVITY_CAPTIVE };
	int right = 0;
	BString detail;
	for (int i = 0; i < 3; i++) {
		local_server server;
		server.reply = kReplies[i];
		thread_id thread = -1;
		uint16 port = start_server(server, thread);
		BString url;
		url.SetToFormat("http://127.0.0.1:%u/check", port);
		bigtime_t start = system_time();
		connectivity_result result = port != 0
			? check_connectivity(url.String(), 3000000) : CONNECTIVITY_OFFLINE;
		detail << (i > 0 ? ", " : "") << (int)result << " in "
			<< (system_time() - start) / 1000 << " ms";
		if (result == kWant[i] && server.requests == 1)
			right++;
		if (port != 0)
			stop_server(server, thread);
	}

	// nothing listening: offline, quickly
	local_server closed;
	closed.reply = "";
	thread_id thread = -1;
	uint16 port = start_server(closed, thread);
	if (port != 0)
		stop_server(closed, thread);
	BString url;
	url.SetToFormat("http://127.0.0.1:%u/", port);
	bigtime_t start = system_time();
	bool offline = check_connectivity(url.String(), 3000000)
		== CONNECTIVITY_OFFLINE;
	bigtime_t offlineTime = system_time() - start;

	// settings: defaults, then written and read back
	char path[] = "/tmp/wifi_test_connectivityXXXXXX";
	int fd = mkstemp(path);
	if (fd >= 0)
		close(fd);
	unlink(path);
	connectivity_settings settings;
	load_connectivity_settings(settings, path);
	bool defaults = settings.enabled && settings.url == kDefaultConnectivityURL
		&& settings.interval == kDefaultConnectivityInterval;
	settings.enabled = false;
	settings.url = "http://127.0.0.1:1/x";
	settings.interval = 120;
	save_connectivity_settings(settings, path);
	connectivity_settings read;
	load_connectivity_settings(read, path);
	bool roundTrip = !read.enabled && read.url == "http://127.0.0.1:1/x"
		&& read.interval == 120;
	unlink(path);

	report("connectivity_local", right == 3 && offline && defaults
			&& roundTrip, "204, \"success\", redirect: %s (want 0, 0, 1); "
		"nothing listening: %s in %" B_PRIdBIGTIME " ms; settings: defaults "
		"%s, written and read %s", detail.String(),
		offline ? "offline" : "not offline", offlineTime / 1000,
		defaults ? "right" : "wrong", roundTrip ? "right" : "wrong");
}


//!	Runs "NetworkStatus --state" and returns its output.
static BString
network_status_state()
{
	BString output;
	FILE* pipe = popen("/boot/system/apps/NetworkStatus --state 2>&1", "r");
	if (pipe == NULL)
		return output;
	char line[512];
	while (fgets(line, sizeof(line), pipe) != NULL)
		output << line;
	pclose(pipe);
	return output;
}


static void
test_status_state()
{
	if (access("/boot/system/servers/app_server", F_OK) != 0
		|| access("/boot/system/apps/NetworkStatus", X_OK) != 0) {
		printf("CE-INFO wifi: no desktop or no NetworkStatus in this image\n");
		return;
	}

	BPath settingsPath;
	connectivity_settings_path(settingsPath);
	bool existed = access(settingsPath.Path(), F_OK) == 0;
	BString saved;
	if (existed) {
		FILE* file = fopen(settingsPath.Path(), "r");
		char line[512];
		while (file != NULL && fgets(line, sizeof(line), file) != NULL)
			saved << line;
		if (file != NULL)
			fclose(file);
	}

	unlink(settingsPath.Path());
	BString state = network_status_state();
	BString icon;
	icon.SetToFormat("icon %d %d\n", (int)kNetworkIconWiredDim,
		(int)kNetworkIconCross);
	bool noDevice = state.FindFirst(icon) >= 0
		&& state.FindFirst("state disconnected\n") >= 0
		&& state.FindFirst("tooltip No network device\n") >= 0;
	bool menu = state.FindFirst("menu Network preferences" B_UTF8_ELLIPSIS)
		>= 0;
	BString checkOn("check on ");
	checkOn << kDefaultConnectivityURL << "\n";
	bool defaultOn = state.FindFirst(checkOn) >= 0;

	// the Network preferences' switch
	connectivity_settings settings;
	settings.enabled = false;
	save_connectivity_settings(settings);
	BString offState = network_status_state();
	bool switchedOff = offState.FindFirst("check off ") >= 0;

	if (existed) {
		FILE* file = fopen(settingsPath.Path(), "w");
		if (file != NULL) {
			fputs(saved.String(), file);
			fclose(file);
		}
	} else
		unlink(settingsPath.Path());

	BString shown(state);
	shown.ReplaceAll("\n", " | ");
	report("status_state", noDevice && menu && defaultOn && switchedOff,
		"no card: %s; menu %s; check on by default %s, switched off %s: %s",
		noDevice ? "greyed plug, cross" : "wrong", menu ? "right" : "wrong",
		defaultOn ? "yes" : "no", switchedOff ? "yes" : "no",
		shown.String());
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
	test_icon_logic();
	test_icon_kind();
	test_icons_hvif();
	test_connectivity_reply();
	test_connectivity_local();
	test_status_state();
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
