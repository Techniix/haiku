/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Tests the LinuxKPI core (src/libs/compat/linuxkpi, roadmap F-26) in
	QEMU, where there is no MediaTek card: the harness attaches QEMU's "edu"
	educational PCI device (1234:11e8), and harness images carry
	lkpi_pcitest, LinuxKPI_HaikuOS' test driver written against the Linux
	API only. Matched against edu it runs a functional test through
	LinuxKPI: MMIO, an MSI interrupt raised on request and one on the end of
	a computation, DMA from a coherent buffer and from streaming mappings
	above the device's DMA mask (24 bits in the harness, so the DMA API has to bounce them),
	dma_sync_single_for_cpu(), timer_list, delayed work and an hrtimer, each
	firing and each cancelled. LinuxKPI's own load-time self-test (wait
	queues, kthreads, tasklets, work queues, jiffies, page_pool) runs on
	every load as well.

	The driver declines the device after its test on purpose, so Haiku
	unloads it again (uninit_driver, LinuxKPI's SYSUNINITs). The test loads
	it LOAD_CYCLES times and reads the driver's lines back from the syslog.
	A load is triggered the way devfs notices a new driver binary: the
	binary's modification time changes (utimes()), devfs' node listener
	marks the driver updated, and its once-a-second driver daemon reloads
	every updated driver none of whose devices is open
	(legacy_drivers.cpp, handle_driver_events()). Writing a driver name to
	/dev, as src/bin/rescan.c does, is refused by today's devfs.
	Checks:

	- edu_<check>: the driver's "edu: <check> PASS|FAIL" lines of the first
	  load
	- selftest_<n>: "linuxkpi: selftest <n> ...: OK" of the first load
	- load_cycles: every load ran the test to the end and detached again
	- reload_results: the later loads passed the same checks
	- no_area_leak: the kernel team's area count after the last load is not
	  above the count after the second one (LinuxKPI page allocations are
	  areas, so a leak per load shows here). The first comparison point is
	  the second load: between the first and the second the count grew once
	  by about 17 and then stayed (seen in QEMU; other boot activity runs
	  then), which is not a per-load leak
	- no_errors: no "linuxkpi"/"lkpi" line says FAIL, panic or leak
	- mt7921e_packaged, firmware_packaged, data_packaged: the system
	  package carries the MT7921/MT7922 driver with its dev/net link, the
	  six firmware files (the MT7922 ones with the sizes verified on the
	  maintainer's card) and LinuxKPI Manager's data files

	Bouncing is only proven if a buffer above the mask was found; the
	driver says so ("edu: INFO ...") otherwise, and edu_dma_map_bounce
	then reports the identity mapping. The host checks that QEMU never
	clamped a DMA address (ce-test.py, linuxkpi.edu_no_clamp).
*/


#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>


#define SUITE "linuxkpi"

static const char* kSyslog = "/boot/system/var/log/syslog";
static const int LOAD_CYCLES = 4;

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


/*!	Lines of the syslog from \a offset on. syslog_daemon may rotate the
	file (syslog.1); a file smaller than \a offset is read from the start.
*/
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


static int32
kernel_area_count()
{
	int32 count = 0;
	ssize_t cookie = 0;
	area_info info;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &info) == B_OK)
		count++;
	return count;
}


static const char* sDriverPath = NULL;


static bool
rescan()
{
	return sDriverPath != NULL && utimes(sDriverPath, NULL) == 0;
}


/*!	Loads the driver once and waits until its test has ended (the "self-test
	complete" line), at most 30 s. Returns the lines of this load.
*/
static std::vector<std::string>
load_once(bool& ended)
{
	off_t start = syslog_size();
	ended = false;
	if (!rescan())
		return std::vector<std::string>();
	std::vector<std::string> lines;
	for (int i = 0; i < 300; i++) {
		usleep(100000);
		lines = syslog_lines(start);
		if (count_lines(lines, "self-test complete") > 0
			&& count_lines(lines, "detach OK") > 0) {
			ended = true;
			// the driver's uninit (SYSUNINITs) follows right after
			usleep(500000);
			lines = syslog_lines(start);
			break;
		}
	}
	return lines;
}


/*!	"edu: <check> PASS|FAIL <detail>" lines -> check -> (pass, detail). */
static std::map<std::string, std::pair<bool, std::string> >
edu_results(const std::vector<std::string>& lines)
{
	std::map<std::string, std::pair<bool, std::string> > results;
	for (size_t i = 0; i < lines.size(); i++) {
		size_t at = lines[i].find("edu: ");
		if (at == std::string::npos)
			continue;
		std::string rest = lines[i].substr(at + 5);
		size_t space = rest.find(' ');
		if (space == std::string::npos)
			continue;
		std::string name = rest.substr(0, space);
		std::string verdict = rest.substr(space + 1);
		bool pass;
		if (verdict.compare(0, 4, "PASS") == 0)
			pass = true;
		else if (verdict.compare(0, 4, "FAIL") == 0)
			pass = false;
		else
			continue;
		std::string detail = verdict.size() > 5 ? verdict.substr(5) : "";
		results[name] = std::make_pair(pass, detail);
	}
	return results;
}


static const char* kEduChecks[] = {
	"mmio_id", "mmio_liveness", "msi_raise", "msi_factorial",
	"dma_coherent_alloc", "dma_coherent_roundtrip", "dma_map_bounce",
	"dma_streaming_roundtrip", "dma_sync_for_cpu", "timer_list",
	"timer_cancel", "delayed_work", "delayed_work_cancel", "hrtimer", "done"
};

static const char* kSelftests[] = {
	"1 timeout", "2 wake", "3 tasklet", "4 workqueue", "5 jiffies",
	"6 page_pool"
};


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	static const char* kPaths[] = {
		"/boot/system/non-packaged/add-ons/kernel/drivers/bin/lkpi_pcitest",
		"/boot/system/add-ons/kernel/drivers/bin/lkpi_pcitest"
	};
	struct stat st;
	for (size_t i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); i++) {
		if (stat(kPaths[i], &st) == 0) {
			sDriverPath = kPaths[i];
			break;
		}
	}
	if (sDriverPath == NULL) {
		report("driver_installed", false, "lkpi_pcitest not in the image");
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	std::vector<std::vector<std::string> > loads;
	std::vector<int32> areas;
	int ended = 0;
	for (int cycle = 0; cycle < LOAD_CYCLES; cycle++) {
		bool done;
		loads.push_back(load_once(done));
		if (done)
			ended++;
		// let the unload and LinuxKPI's deferred frees (30 s grace for
		// thread records is not waited for; areas go at once) settle
		usleep(300000);
		areas.push_back(kernel_area_count());
		printf("CE-INFO linuxkpi: load %d: %zu lines, ended %d, %" B_PRId32
			" kernel areas\n", cycle + 1, loads.back().size(), done,
			areas.back());
		fflush(stdout);
	}

	const std::vector<std::string>& first = loads[0];
	report("probe", count_lines(first, "stage a: probe 1234:11e8") == 1,
		"%d probe lines for the edu device",
		count_lines(first, "stage a: probe 1234:11e8"));
	report("msi_vector", count_lines(first, "installed (msi=1)") == 1,
		"MSI vector installed: %d", count_lines(first, "installed (msi=1)"));

	std::map<std::string, std::pair<bool, std::string> > results
		= edu_results(first);
	for (size_t i = 0; i < sizeof(kEduChecks) / sizeof(kEduChecks[0]); i++) {
		std::string name = std::string("edu_") + kEduChecks[i];
		std::map<std::string, std::pair<bool, std::string> >::iterator found
			= results.find(kEduChecks[i]);
		if (found == results.end())
			report(name.c_str(), false, "no result from the driver");
		else {
			report(name.c_str(), found->second.first, "%s",
				found->second.second.c_str());
		}
	}
	for (size_t i = 0; i < first.size(); i++) {
		if (first[i].find("edu: INFO") != std::string::npos) {
			printf("CE-INFO %s\n", first[i].c_str());
			fflush(stdout);
		}
	}

	for (size_t i = 0; i < sizeof(kSelftests) / sizeof(kSelftests[0]); i++) {
		std::string pattern = std::string("linuxkpi: selftest ")
			+ kSelftests[i];
		std::string line;
		for (size_t j = 0; j < first.size(); j++) {
			if (first[j].find(pattern) != std::string::npos) {
				line = first[j].substr(first[j].find(pattern)
					+ strlen("linuxkpi: selftest "));
				break;
			}
		}
		std::string name = std::string("selftest_")
			+ (strchr(kSelftests[i], ' ') + 1);
		report(name.c_str(), line.find(": OK") != std::string::npos, "%s",
			line.empty() ? "no result" : line.c_str());
	}

	report("load_cycles", ended == LOAD_CYCLES,
		"%d of %d loads ran the test and detached", ended, LOAD_CYCLES);

	int laterFailures = 0, laterResults = 0;
	for (size_t cycle = 1; cycle < loads.size(); cycle++) {
		std::map<std::string, std::pair<bool, std::string> > later
			= edu_results(loads[cycle]);
		for (std::map<std::string, std::pair<bool, std::string> >::iterator
				it = later.begin(); it != later.end(); it++) {
			laterResults++;
			if (!it->second.first)
				laterFailures++;
		}
	}
	report("reload_results", laterFailures == 0
			&& laterResults == (int)(LOAD_CYCLES - 1)
				* (int)(sizeof(kEduChecks) / sizeof(kEduChecks[0])),
		"%d results in the later loads, %d failed", laterResults,
		laterFailures);

	std::string counts;
	for (size_t i = 0; i < areas.size(); i++) {
		char number[16];
		snprintf(number, sizeof(number), "%s%" B_PRId32, i > 0 ? " " : "",
			areas[i]);
		counts += number;
	}
	report("no_area_leak", areas.size() == (size_t)LOAD_CYCLES
			&& areas.back() <= areas[1] + 1,
		"kernel areas after each load: %s", counts.c_str());

	int errors = 0;
	std::string firstError;
	for (size_t cycle = 0; cycle < loads.size(); cycle++) {
		for (size_t i = 0; i < loads[cycle].size(); i++) {
			const std::string& line = loads[cycle][i];
			if (line.find("lkpi") == std::string::npos
				&& line.find("linuxkpi") == std::string::npos
				&& line.find("edu:") == std::string::npos)
				continue;
			if (line.find("FAIL") != std::string::npos
				|| line.find("panic") != std::string::npos
				|| line.find("leak") != std::string::npos) {
				if (errors++ == 0)
					firstError = line;
			}
		}
	}
	report("no_errors", errors == 0, "%d error lines%s%s", errors,
		errors > 0 ? ", first: " : "", firstError.c_str());

	// what the system package brings for the maintainer's hardware
	struct stat link;
	bool driver = stat("/boot/system/add-ons/kernel/drivers/bin/mt7921e",
			&st) == 0 && S_ISREG(st.st_mode)
		&& lstat("/boot/system/add-ons/kernel/drivers/dev/net/mt7921e",
			&link) == 0 && S_ISLNK(link.st_mode);
	report("mt7921e_packaged", driver, "drivers/bin/mt7921e and the "
		"dev/net link %s", driver ? "present" : "missing");
	static const struct {
		const char*	name;
		off_t		size;	// 0: any
	} kFirmware[] = {
		{ "WIFI_MT7922_patch_mcu_1_1_hdr.bin", 137632 },
		{ "WIFI_RAM_CODE_MT7922_1.bin", 1233748 },
		{ "WIFI_MT7961_patch_mcu_1_2_hdr.bin", 0 },
		{ "WIFI_RAM_CODE_MT7961_1.bin", 0 },
		{ "WIFI_MT7961_patch_mcu_1a_2_hdr.bin", 0 },
		{ "WIFI_RAM_CODE_MT7961_1a.bin", 0 },
		{ "LICENCE.mediatek", 0 },
	};
	int firmwareOK = 0;
	std::string missing;
	for (size_t i = 0; i < sizeof(kFirmware) / sizeof(kFirmware[0]); i++) {
		std::string path = std::string("/boot/system/data/firmware/mt7921e/")
			+ kFirmware[i].name;
		if (stat(path.c_str(), &st) == 0 && st.st_size > 0
			&& (kFirmware[i].size == 0 || st.st_size == kFirmware[i].size))
			firmwareOK++;
		else
			missing += std::string(" ") + kFirmware[i].name;
	}
	report("firmware_packaged",
		firmwareOK == (int)(sizeof(kFirmware) / sizeof(kFirmware[0])),
		"%d of %d files%s%s", firmwareOK,
		(int)(sizeof(kFirmware) / sizeof(kFirmware[0])),
		missing.empty() ? "" : ", wrong or missing:", missing.c_str());
	bool data = stat("/boot/system/data/linuxkpi/drivers/mt7921e", &st) == 0
		&& stat("/boot/system/data/linuxkpi/coverage", &st) == 0
		&& stat("/boot/system/data/linuxkpi/diagnostics", &st) == 0;
	report("data_packaged", data, "data/linuxkpi manifest, coverage and "
		"diagnostics %s", data ? "present" : "missing");

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
