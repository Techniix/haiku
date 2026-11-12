/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Attaching, detaching and reloading a Wi-Fi driver built on
	freebsd_network and freebsd_wlan, the layers under every FreeBSD and
	LinuxKPI (mt7921e) Wi-Fi driver on Renga. QEMU has no Wi-Fi device, so
	the harness attaches QEMU's PCI test device ("pci-testdev", 1b36:0005)
	and harness images carry fbsd_wlantest, a net80211 driver without a
	radio that matches it (src/add-ons/kernel/drivers/network/wlan/
	fbsd_wlantest). Its attach creates and deletes one extra vap; then
	freebsd_wlan's start_wlan() creates the vap published as
	/dev/net/fbsd_wlantest/0. A reload (the kernel daemon's, when the
	driver's binary changes and none of its devices is open) runs
	stop_wlan() (the vap deleted: ieee80211_vap_detach(), ether_ifdetach(),
	if_free()) and device_detach() before the driver is loaded again.

	Until Renga's fix in freebsd_network's if_detach(), every one of those
	vap deletions destroyed the ifnet's address lock twice, and a KDEBUG
	kernel panicked ("mutex_destroy(): the lock ... is held by -32768"), the
	panic of mt7921e's reload on the maintainer's machine. Here the first
	load at boot would already have panicked.

	The driver attaches only once this test creates
	/boot/system/non-packaged/data/ce-harness/fbsd_wlantest and touches the
	binary, so that it holds no memory during the other tests.

	Checks:
	- attached: the first load attached, made and deleted the extra vap
	- published: /dev/net/fbsd_wlantest/0 exists
	- reloads: RELOADS reloads, each with "vap deleted" and "detached" of
	  the old image and "attached", "extra vap done" of the new one
	- republished: the device is there again after the last one
	- no_errors: the driver printed no FAIL line

	net_server opens Wi-Fi devices it finds, and devfs does not reload a
	driver with an open device, so the test removes the interface from the
	network stack ("ifconfig --delete <device>") before each reload.
*/


#include <OS.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <vector>


#define SUITE "wlan_reload"

static const char* kSyslog = "/boot/system/var/log/syslog";
static const char* kDevice = "/dev/net/fbsd_wlantest/0";
static const int RELOADS = 3;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[512];
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


static off_t
syslog_size()
{
	struct stat st;
	if (stat(kSyslog, &st) != 0)
		return 0;
	return st.st_size;
}


static std::vector<std::string>
syslog_lines(off_t offset)
{
	std::vector<std::string> lines;
	FILE* file = fopen(kSyslog, "r");
	if (file == NULL)
		return lines;
	fseeko(file, 0, SEEK_END);
	if (ftello(file) < offset)
		offset = 0;
	fseeko(file, offset, SEEK_SET);
	char buffer[2048];
	while (fgets(buffer, sizeof(buffer), file) != NULL) {
		size_t length = strlen(buffer);
		while (length > 0 && (buffer[length - 1] == '\n'
				|| buffer[length - 1] == '\r'))
			buffer[--length] = '\0';
		if (strstr(buffer, "fbsd_wlantest") != NULL)
			lines.push_back(buffer);
	}
	fclose(file);
	return lines;
}


static int
count_lines(const std::vector<std::string>& lines, const char* text)
{
	int count = 0;
	for (size_t i = 0; i < lines.size(); i++) {
		if (lines[i].find(text) != std::string::npos)
			count++;
	}
	return count;
}


static bool
exists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0;
}


static bool
wait_for(off_t start, const char* text, int count, bigtime_t timeout,
	std::vector<std::string>& lines)
{
	bigtime_t deadline = system_time() + timeout;
	do {
		lines = syslog_lines(start);
		if (count_lines(lines, text) >= count)
			return true;
		snooze(200000);
	} while (system_time() < deadline);
	return false;
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	static const char* kPaths[] = {
		"/boot/system/non-packaged/add-ons/kernel/drivers/bin/fbsd_wlantest",
		"/boot/system/add-ons/kernel/drivers/bin/fbsd_wlantest"
	};
	const char* driverPath = NULL;
	for (size_t i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); i++) {
		if (exists(kPaths[i])) {
			driverPath = kPaths[i];
			break;
		}
	}
	if (driverPath == NULL) {
		report("driver_installed", false, "fbsd_wlantest not in the image");
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	// The driver attaches only once this file exists (see its probe), so
	// that it does not hold low memory during the boot's other tests; the
	// first load is a reload here.
	static const char* kEnable
		= "/boot/system/non-packaged/data/ce-harness/fbsd_wlantest";
	FILE* enable = fopen(kEnable, "w");
	if (enable != NULL)
		fclose(enable);
	utimes(driverPath, NULL);

	bool published = false;
	for (int i = 0; i < 100 && !published; i++) {
		published = exists(kDevice);
		if (!published)
			snooze(200000);
	}
	std::vector<std::string> lines;
	bool attached = wait_for(0, "fbsd_wlantest: extra vap done", 1,
		20000000, lines);
	report("attached", attached && count_lines(lines, "vap deleted") >= 1,
		"%d attach, %d extra vap, %d vap deletions in the syslog",
		count_lines(lines, "fbsd_wlantest: attached"),
		count_lines(lines, "extra vap done"),
		count_lines(lines, "vap deleted"));
	report("published", published, "%s %s", kDevice,
		published ? "exists" : "missing");

	int reloaded = 0;
	std::string detail;
	for (int cycle = 0; cycle < RELOADS; cycle++) {
		off_t start = syslog_size();
		// Let the network stack close the device, then change the binary's
		// modification time: the kernel daemon reloads it within a second.
		// net_server may bring the interface up again in the meantime, so
		// try a few times.
		std::vector<std::string> cycleLines;
		bool done = false;
		for (int attempt = 0; attempt < 4 && !done; attempt++) {
			int result = system("ifconfig --delete /dev/net/fbsd_wlantest/0 "
				">/dev/null 2>&1");
			if (attempt == 0 && cycle == 0) {
				printf("CE-INFO wlan_reload: ifconfig --delete: exit %d\n",
					result);
				fflush(stdout);
			}
			snooze(500000);
			if (utimes(driverPath, NULL) != 0) {
				detail += "utimes failed: ";
				detail += strerror(errno);
				break;
			}
			done = wait_for(start, "fbsd_wlantest: extra vap done", 1,
				6000000, cycleLines);
		}
		int deleted = count_lines(cycleLines, "vap deleted");
		int detached = count_lines(cycleLines, "fbsd_wlantest: detached");
		int attachedAgain = count_lines(cycleLines, "fbsd_wlantest: attached");
		char text[128];
		snprintf(text, sizeof(text), "%s#%d: %d deleted, %d detached, %d "
			"attached", cycle > 0 ? "; " : "", cycle + 1, deleted, detached,
			attachedAgain);
		detail += text;
		// two vap deletions: the published one (stop_wlan) and the new
		// image's extra one
		if (done && deleted >= 2 && detached >= 1 && attachedAgain >= 1)
			reloaded++;
		else
			break;
	}
	report("reloads", reloaded == RELOADS, "%d of %d: %s", reloaded, RELOADS,
		detail.c_str());

	bool republished = false;
	for (int i = 0; i < 50 && !republished; i++) {
		republished = exists(kDevice);
		if (!republished)
			snooze(200000);
	}
	report("republished", republished, "%s %s after the reloads", kDevice,
		republished ? "exists" : "missing");

	lines = syslog_lines(0);
	int failures = count_lines(lines, "FAIL");
	report("no_errors", failures == 0, "%d FAIL lines from the driver",
		failures);
	unlink(kEnable);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
