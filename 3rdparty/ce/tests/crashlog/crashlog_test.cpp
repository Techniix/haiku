/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Crash reports and System logs (roadmap F-51), over three boots of the
	same QEMU session. The guest runner (harness/guest/runner.sh) starts it:

	1. At the end of a normal run, with --crash: crash_collector on made-up
	   syslogs (a complete report, a cut-off one, a second run that must not
	   save again), an application crash (debug_server's report), then a
	   kernel panic through upstream's kdl test driver (/dev/misc/kdl). The
	   test prints "CE-HOST kdl_reboot" first: the harness then answers the
	   kernel debugger with "reboot", the guest's own reboot path.
	2. The boot after it (the phase file says 1): the kernel crash report
	   crash_collector saved from the previous session's syslog, with the
	   panic message, a backtrace per CPU, the boot information and the
	   syslog before the crash. On images with System logs and a desktop
	   (the renga-* profiles): the notification at login, the window on the
	   report (as clicking the notification opens it), the live log with its
	   filters, the previous boot, "Save a report" (the archive is read
	   back), and screenshots. Then the kernel debugger is entered once more,
	   the way a user does it (kernel_debugger(), a "USER:" message).
	3. The boot after that: the second report, and the first one not saved
	   twice.
*/


#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Messenger.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <fs_attr.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <zlib.h>

#include <crash_report_format.h>


extern "C" status_t _kern_kernel_debugger(const char* message);


#define SUITE "crashlog"

static const char* kPhaseFile
	= "/boot/system/non-packaged/data/ce-harness/crash-phase";
static const char* kCollector = "/bin/crash_collector";
static const char* kSystemLogs = "/boot/system/apps/SystemLogs";
static const char* kSystemLogsSignature = "application/x-vnd.Renga-SystemLogs";
static const char* kExpectedMarker
	= "CE-INFO crashlog: entering the kernel debugger on purpose";

// System logs' messages (src/apps/systemlogs/SystemLogs.cpp)
static const uint32 kMsgSetFilter = 'slft';
static const uint32 kMsgSelectTab = 'sltb';
static const uint32 kMsgSaveReport = 'slsr';
static const uint32 kMsgGetState = 'slst';

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	// one line: the harness reads a check from a single line
	for (char* c = detail; *c != '\0'; c++) {
		if (*c == '\n' || *c == '\r')
			*c = ' ';
	}
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
info(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("CE-INFO crashlog: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
}


static std::string
read_file(const char* path)
{
	std::string contents;
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return contents;
	char buffer[65536];
	size_t bytesRead;
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0)
		contents.append(buffer, bytesRead);
	fclose(file);
	return contents;
}


static bool
write_file(const char* path, const std::string& contents)
{
	FILE* file = fopen(path, "wb");
	if (file == NULL)
		return false;
	bool ok = fwrite(contents.data(), 1, contents.size(), file)
		== contents.size();
	fflush(file);
	fsync(fileno(file));
	fclose(file);
	return ok;
}


static bool
contains(const std::string& text, const char* what)
{
	return text.find(what) != std::string::npos;
}


static int
run(const char* command, std::string* output = NULL)
{
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return -1;
	char buffer[4096];
	size_t bytesRead;
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
		if (output != NULL)
			output->append(buffer, bytesRead);
	}
	int status = pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


static std::vector<std::string>
files_in(const char* directory, const char* prefix)
{
	std::vector<std::string> names;
	BDirectory dir(directory);
	BEntry entry;
	while (dir.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		if (strncmp(name, prefix, strlen(prefix)) == 0)
			names.push_back(std::string(directory) + "/" + name);
	}
	return names;
}


static int32
read_new_attribute(const char* path)
{
	int32 value = 0;
	BNode node(path);
	if (node.ReadAttr(CRASH_REPORT_NEW_ATTRIBUTE, B_INT32_TYPE, 0, &value,
			sizeof(value)) != sizeof(value)) {
		return -1;
	}
	return value;
}


static void
set_phase(int phase)
{
	char text[16];
	snprintf(text, sizeof(text), "%d\n", phase);
	write_file(kPhaseFile, text);
	sync();
}


/*!	Enters the kernel debugger; the harness answers it with "reboot". */
static void
enter_debugger(const char* how)
{
	info("%s", kExpectedMarker + strlen("CE-INFO crashlog: "));
	printf("CE-HOST kdl_reboot\n");
	fflush(stdout);
	sync();
	// the harness reads the request before the debugger's first line
	snooze(1500000);

	if (strcmp(how, "panic") == 0) {
		// upstream's kdl test driver panics on read
		int fd = open("/dev/misc/kdl", O_RDONLY);
		if (fd < 0) {
			report("kdl_driver", false, "cannot open /dev/misc/kdl: %s",
				strerror(errno));
			return;
		}
		char byte;
		read(fd, &byte, 1);
		close(fd);
	} else {
		status_t status = _kern_kernel_debugger("CE crash test");
		if (status != B_OK) {
			report("user_debugger", false, "kernel_debugger(): %s",
				strerror(status));
			return;
		}
	}
	// "reboot" comes from the harness within a few seconds
	snooze(30000000);
	report("rebooted", false, "still running 30 s after entering the kernel "
		"debugger (%s)", how);
}


// #pragma mark - phase 0: before the first crash


static void
check_collector()
{
	const char* directory = "/tmp/ce-crashlog";
	run("rm -rf /tmp/ce-crashlog");
	mkdir(directory, 0755);

	std::string syslog;
	for (int i = 0; i < 2000; i++) {
		char line[80];
		snprintf(line, sizeof(line), "KERN: filler line %d before the crash\n",
			i);
		syslog += line;
	}
	syslog += "KERN: the last line before the crash\n";
	syslog += "\n" CRASH_REPORT_BEGIN "\nformat: 1\nreason: panic\n"
		"message: PANIC: made-up panic\nrevision: hrev1\ntime: 1700000000\n"
		"uptime_us: 1234\ncpu_count: 2\n"
		CRASH_REPORT_CPU " 0: thread 1 \"idle thread 1\"\nstack trace\n"
		CRASH_REPORT_IMAGES "\nloaded kernel images:\n"
		CRASH_REPORT_END "\nKERN: after the report\n";
	// a second report, cut off (no end line)
	syslog += CRASH_REPORT_BEGIN "\nformat: 1\nreason: keyboard\n"
		"message: Keyboard Requested Halt.\ntime: 1700000100\n"
		"uptime_us: 99\n";
	write_file("/tmp/ce-crashlog-syslog", syslog);

	std::string output;
	int status = run("/bin/crash_collector --previous-syslog "
		"/tmp/ce-crashlog-syslog --directory /tmp/ce-crashlog 2>&1", &output);
	std::vector<std::string> files = files_in(directory, "kernel-");
	std::string first;
	std::string second;
	for (size_t i = 0; i < files.size(); i++) {
		std::string text = read_file(files[i].c_str());
		// (the second report's syslog tail has the first report in it)
		if (contains(text, "reason: keyboard\n"))
			second = text;
		else if (contains(text, "made-up panic"))
			first = text;
	}
	report("collector_saves", status == 0 && files.size() == 2
		&& !first.empty() && !second.empty(),
		"exit %d, %d files (want 2)", status, (int)files.size());
	report("collector_contents",
		contains(first, "reason: panic") && contains(first, CRASH_REPORT_END)
			&& contains(first, "the last line before the crash")
			&& !contains(first, "filler line 0 ")
			&& !contains(first, "after the report"),
		"report %s, syslog tail %s, older lines %s",
		contains(first, CRASH_REPORT_END) ? "complete" : "incomplete",
		contains(first, "the last line before the crash") ? "kept" : "missing",
		contains(first, "filler line 0 ") ? "kept (want only 64 KiB)"
			: "left out");
	report("collector_incomplete", contains(second, "incomplete"),
		"cut-off report %s", contains(second, "incomplete")
			? "marked incomplete" : "not marked");
	bool marked = files.size() == 2
		&& read_new_attribute(files[0].c_str()) == 1
		&& read_new_attribute(files[1].c_str()) == 1;
	report("collector_marks_new", marked, "attribute %s",
		CRASH_REPORT_NEW_ATTRIBUTE);

	status = run("/bin/crash_collector --previous-syslog "
		"/tmp/ce-crashlog-syslog --directory /tmp/ce-crashlog 2>&1");
	report("collector_once", status == 0
		&& files_in(directory, "kernel-").size() == 2,
		"second run: %d files (want still 2)",
		(int)files_in(directory, "kernel-").size());

	status = run("/bin/crash_collector --previous-syslog /nonexistent "
		"--directory /tmp/ce-crashlog 2>&1");
	report("collector_nothing", status == 0, "no previous syslog: exit %d",
		status);
	run("rm -rf /tmp/ce-crashlog /tmp/ce-crashlog-syslog");
}


static void
check_application_crash(const char* self)
{
	std::vector<std::string> before = files_in(CRASH_REPORT_DIRECTORY,
		"app-crashlog_test-");
	pid_t child = fork();
	if (child == 0) {
		execl(self, self, "--crash-app", (char*)NULL);
		_exit(127);
	}
	int status = 0;
	waitpid(child, &status, 0);

	std::string text;
	std::vector<std::string> after;
	for (int i = 0; i < 40; i++) {
		after = files_in(CRASH_REPORT_DIRECTORY, "app-crashlog_test-");
		if (after.size() > before.size())
			break;
		snooze(250000);
	}
	std::string path;
	for (size_t i = 0; i < after.size(); i++) {
		bool old = false;
		for (size_t j = 0; j < before.size(); j++)
			old |= before[j] == after[i];
		if (!old)
			path = after[i];
	}
	text = read_file(path.c_str());
	report("app_crash_report", !path.empty()
		&& contains(text, "Renga application crash report")
		&& contains(text, "program: /boot/system/non-packaged/bin/")
		&& contains(text, "reason: ") && contains(text, "stack trace"),
		"%s", path.empty() ? "no new app-crashlog_test-*.txt" : path.c_str());
	report("app_crash_symbol", contains(text, "crash_here"),
		"the stack trace %s the crashing function",
		contains(text, "crash_here") ? "names" : "does not name");
}


extern "C" void __attribute__((noinline))
crash_here()
{
	*(volatile int*)16 = 1;
}


// #pragma mark - phase 1: after the panic


static std::string
newest_kernel_report(const char* mustContain, std::string* _path)
{
	for (int i = 0; i < 120; i++) {
		std::vector<std::string> files = files_in(CRASH_REPORT_DIRECTORY,
			"kernel-");
		for (size_t j = 0; j < files.size(); j++) {
			std::string text = read_file(files[j].c_str());
			if (contains(text, mustContain)) {
				if (_path != NULL)
					*_path = files[j];
				return text;
			}
		}
		snooze(250000);
	}
	return "";
}


static void
check_kernel_report(const std::string& text, const char* reason,
	const char* message)
{
	system_info systemInfo;
	get_system_info(&systemInfo);
	char cpuCount[32];
	snprintf(cpuCount, sizeof(cpuCount), "cpu_count: %" B_PRIu32 "\n",
		systemInfo.cpu_count);

	std::string wantReason = std::string("reason: ") + reason + "\n";
	std::string wantMessage = std::string("message: ") + message;
	report("report_reason", contains(text, wantReason.c_str())
		&& contains(text, wantMessage.c_str()),
		"want reason \"%s\" and the entry message", reason);
	report("report_boot_info", contains(text, "revision: hrev")
		&& contains(text, cpuCount) && contains(text, "memory_bytes: ")
		&& contains(text, "uptime_us: ") && contains(text, "cpu_model: "),
		"revision, %s, memory, uptime, CPU model", cpuCount);

	int threads = 0;
	for (uint32 i = 0; i < systemInfo.cpu_count; i++) {
		char want[32];
		snprintf(want, sizeof(want), CRASH_REPORT_CPU " %" B_PRIu32 ": ", i);
		if (contains(text, want))
			threads++;
	}
	report("report_cpus", threads == (int)systemInfo.cpu_count
		&& contains(text, "stack trace for thread"),
		"%d of %" B_PRIu32 " CPUs with their thread, backtraces %s", threads,
		systemInfo.cpu_count, contains(text, "stack trace for thread")
			? "present" : "missing");
	report("report_images", contains(text, CRASH_REPORT_IMAGES)
		&& contains(text, "kernel_x86_64"), "loaded kernel images listed");
	size_t images = text.find(CRASH_REPORT_IMAGES);
	size_t tail = text.find("==== syslog before the crash");
	bool complete = contains(text, CRASH_REPORT_END)
		&& !contains(text.substr(0, tail), "incomplete");
	report("report_complete", complete, "report %s its end line",
		complete ? "has" : "lacks");
	if (!complete && images != std::string::npos) {
		// what the report ends with, for the log
		std::string end = text.substr(images, std::min((size_t)3000,
			(tail != std::string::npos ? tail : text.size()) - images));
		size_t last = end.size() > 400 ? end.size() - 400 : 0;
		end = end.substr(last);
		for (size_t i = 0; i < end.size(); i++) {
			if (end[i] == '\n')
				end[i] = '|';
		}
		info("report ends with: %s", end.c_str());
	}
}


static BMessage
app_state(BMessenger& messenger, uint32 what = kMsgGetState,
	BMessage* request = NULL)
{
	BMessage message(what);
	if (request != NULL)
		message = *request;
	BMessage reply;
	if (messenger.SendMessage(&message, &reply, 5000000, 5000000) != B_OK)
		reply.MakeEmpty();
	return reply;
}


static bool
notification_shown()
{
	BMessenger server("application/x-vnd.Haiku-notification_server");
	BMessage message(B_GET_PROPERTY);
	message.AddSpecifier("Hidden");
	message.AddSpecifier("Window", (int32)0);
	BMessage reply;
	if (server.SendMessage(&message, &reply, 2000000, 2000000) != B_OK)
		return false;
	bool hidden = true;
	return reply.FindBool("result", &hidden) == B_OK && !hidden;
}


static bool
zip_has(const std::string& zip, const char* name)
{
	// a central directory entry: 0x02014b50 ... name
	size_t position = 0;
	while ((position = zip.find("PK\x01\x02", position)) != std::string::npos) {
		if (position + 46 > zip.size())
			break;
		const uint8* entry = (const uint8*)zip.data() + position;
		uint16 nameLength = entry[28] | (entry[29] << 8);
		std::string entryName(zip, position + 46, nameLength);
		if (entryName.compare(0, strlen(name), name) == 0)
			return true;
		position += 46;
	}
	return false;
}


/*!	Inflates the first entry of a ZIP file (the README, the archive writer
	adds it last; any entry will do) and checks its CRC.
*/
static bool
zip_first_entry_valid(const std::string& zip, std::string* _name)
{
	if (zip.size() < 30 || zip.compare(0, 4, "PK\x03\x04") != 0)
		return false;
	const uint8* header = (const uint8*)zip.data();
	uint16 method = header[8] | (header[9] << 8);
	uint32 crc = header[14] | (header[15] << 8) | (header[16] << 16)
		| ((uint32)header[17] << 24);
	uint32 compressedSize = header[18] | (header[19] << 8)
		| (header[20] << 16) | ((uint32)header[21] << 24);
	uint32 size = header[22] | (header[23] << 8) | (header[24] << 16)
		| ((uint32)header[25] << 24);
	uint16 nameLength = header[26] | (header[27] << 8);
	uint16 extraLength = header[28] | (header[29] << 8);
	*_name = std::string(zip, 30, nameLength);
	size_t dataStart = 30 + nameLength + extraLength;
	if (dataStart + compressedSize > zip.size())
		return false;

	std::string data;
	if (method == 0)
		data = zip.substr(dataStart, compressedSize);
	else if (method == 8) {
		data.resize(size);
		z_stream stream;
		memset(&stream, 0, sizeof(stream));
		if (inflateInit2(&stream, -15) != Z_OK)
			return false;
		stream.next_in = (Bytef*)zip.data() + dataStart;
		stream.avail_in = compressedSize;
		stream.next_out = (Bytef*)&data[0];
		stream.avail_out = size;
		int result = inflate(&stream, Z_FINISH);
		inflateEnd(&stream);
		if (result != Z_STREAM_END)
			return false;
	} else
		return false;
	return crc32(0, (const Bytef*)data.data(), data.size()) == crc;
}


static void
check_gui(const std::string& reportPath)
{
	bool haveApp = access(kSystemLogs, X_OK) == 0;
	bool desktop = be_roster->IsRunning("application/x-vnd.Be-TSKB");
	for (int i = 0; i < 120 && haveApp && !desktop; i++) {
		snooze(500000);
		desktop = be_roster->IsRunning("application/x-vnd.Be-TSKB");
	}
	if (!haveApp || !desktop) {
		info("no System logs or no desktop (%s, %s): GUI checks skipped",
			haveApp ? "application installed" : "no application",
			desktop ? "Deskbar running" : "no Deskbar");
		report("report_marked_new", read_new_attribute(reportPath.c_str())
			== 1, "attribute %s set (no desktop to show it)",
			CRASH_REPORT_NEW_ATTRIBUTE);
		return;
	}

	// Reachable from the Deskbar (Applications menu)
	const char* deskbarEntry
		= "/boot/system/data/deskbar/menu/Applications/SystemLogs";
	BEntry deskbar(deskbarEntry);
	report("deskbar_entry", deskbar.Exists(), "%s", deskbarEntry);

	// The notification at login removes the attribute once it was shown.
	bool notified = false;
	bool shown = false;
	for (int i = 0; i < 240; i++) {
		if (!notified && read_new_attribute(reportPath.c_str()) <= 0)
			notified = true;
		if (notified && notification_shown()) {
			shown = true;
			break;
		}
		snooze(250000);
	}
	report("notification", notified && shown, "notification %s, window %s",
		notified ? "sent" : "not sent (attribute still set)",
		shown ? "on screen" : "not on screen");
	printf("CE-HOST screenshot crash_notification\n");
	fflush(stdout);
	snooze(1000000);

	// Clicking the notification launches System logs with the report.
	BEntry entry(reportPath.c_str());
	entry_ref ref;
	entry.GetRef(&ref);
	team_id team = -1;
	BMessage refs(B_REFS_RECEIVED);
	refs.AddRef("refs", &ref);
	be_roster->Launch(kSystemLogsSignature, &refs, &team);
	BMessenger messenger(kSystemLogsSignature);
	BMessage state;
	for (int i = 0; i < 60; i++) {
		messenger = BMessenger(kSystemLogsSignature);
		if (messenger.IsValid()) {
			state = app_state(messenger);
			if (state.GetInt32("tab", -1) == 2
				&& reportPath == state.GetString("selected_crash", "")
				&& state.GetInt32("crash_text_length", 0) > 0)
				break;
		}
		snooze(250000);
	}
	report("viewer_opens_report", state.GetInt32("tab", -1) == 2
		&& reportPath == state.GetString("selected_crash", "")
		&& state.GetInt32("crash_text_length", 0) > 1000,
		"tab %" B_PRId32 ", selected \"%s\", %" B_PRId32 " characters shown",
		state.GetInt32("tab", -1), state.GetString("selected_crash", ""),
		state.GetInt32("crash_text_length", 0));
	report("viewer_lists_crashes", state.GetInt32("crash_reports", 0) >= 2,
		"%" B_PRId32 " crash reports listed (kernel and application)",
		state.GetInt32("crash_reports", 0));
	printf("CE-HOST screenshot systemlogs_crash\n");
	fflush(stdout);
	snooze(1500000);

	// the live log and its filters
	BMessage tab(kMsgSelectTab);
	tab.AddInt32("tab", 0);
	state = app_state(messenger, 0, &tab);
	int32 lines = state.GetInt32("live_lines", 0);
	int32 components = state.GetInt32("live_components", 0);
	report("live_log", lines > 50 && components > 3,
		"%" B_PRId32 " lines of this boot, %" B_PRId32 " components", lines,
		components);

	{
		// a new line appears while the window is open
		run("/bin/logger 'CE crashlog live line'");
		bool appeared = false;
		BMessage filter(kMsgSetFilter);
		filter.AddInt32("tab", 0);
		filter.AddString("search", "CE crashlog live line");
		for (int i = 0; i < 20 && !appeared; i++) {
			snooze(500000);
			state = app_state(messenger, 0, &filter);
			appeared = state.GetInt32("live_shown", 0) >= 1;
		}
		report("live_updates", appeared, "a line logged now %s",
			appeared ? "appeared" : "did not appear within 10 s");
	}

	BMessage filter(kMsgSetFilter);
	filter.AddInt32("tab", 0);
	filter.AddString("search", "");
	filter.AddString("component", "kernel");
	filter.AddInt32("level", 0);
	state = app_state(messenger, 0, &filter);
	int32 kernelLines = state.GetInt32("live_shown", 0);
	filter.MakeEmpty();
	filter.what = kMsgSetFilter;
	filter.AddInt32("tab", 0);
	filter.AddString("component", "");
	filter.AddInt32("level", 2);
	state = app_state(messenger, 0, &filter);
	int32 errorLines = state.GetInt32("live_shown", 0);
	report("live_filters", kernelLines > 0 && kernelLines < lines
		&& errorLines < lines, "component \"kernel\": %" B_PRId32 ", errors: %"
		B_PRId32 " of %" B_PRId32 " lines", kernelLines, errorLines, lines);
	filter.MakeEmpty();
	filter.what = kMsgSetFilter;
	filter.AddInt32("tab", 0);
	filter.AddString("search", "Welcome to syslog");
	filter.AddInt32("level", 0);
	state = app_state(messenger, 0, &filter);
	report("live_search", state.GetInt32("live_shown", 0) == 1,
		"\"Welcome to syslog\": %" B_PRId32 " line(s) (want 1: this boot)",
		state.GetInt32("live_shown", 0));
	filter.ReplaceString("search", "");
	app_state(messenger, 0, &filter);
	printf("CE-HOST screenshot systemlogs_live\n");
	fflush(stdout);
	snooze(1500000);

	tab.ReplaceInt32("tab", 1);
	state = app_state(messenger, 0, &tab);
	report("previous_boot", state.GetInt32("previous_lines", 0) > 50,
		"%" B_PRId32 " lines (%s)", state.GetInt32("previous_lines", 0),
		state.GetBool("previous_found", false) ? "from the system log"
			: "from previous_syslog");
	printf("CE-HOST screenshot systemlogs_previous\n");
	fflush(stdout);
	snooze(1500000);

	// "Save a report"
	const char* archive = "/tmp/ce-system-report.zip";
	unlink(archive);
	BMessage save(kMsgSaveReport);
	save.AddString("path", archive);
	state = app_state(messenger, 0, &save);
	std::string zip = read_file(archive);
	std::string crashName = std::string("crash/")
		+ BPath(reportPath.c_str()).Leaf();
	bool members = zip_has(zip, "logs/syslog") && zip_has(zip, crashName.c_str())
		&& zip_has(zip, "crash/app-") && zip_has(zip, "hardware/listdev.txt")
		&& zip_has(zip, "hardware/listusb.txt")
		&& zip_has(zip, "hardware/sysinfo.txt");
	std::string firstName;
	bool valid = zip_first_entry_valid(zip, &firstName);
	report("save_report", state.GetInt32("status", -1) == 0 && members,
		"status %" B_PRId32 ", %d bytes, logs, the crash reports and hardware "
		"%s", state.GetInt32("status", -1), (int)zip.size(),
		members ? "in it" : "missing");
	report("save_report_valid", valid, "first entry %s %s",
		firstName.c_str(), valid ? "inflates with the right CRC" : "is broken");
	unlink(archive);

	tab.ReplaceInt32("tab", 0);
	app_state(messenger, 0, &tab);
	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team >= 0)
		kill(team, SIGKILL);

	// the command line
	std::string output;
	int status = run("/boot/system/apps/SystemLogs --save-report "
		"/tmp/ce-system-report2.zip 2>&1", &output);
	zip = read_file("/tmp/ce-system-report2.zip");
	report("save_report_cli", status == 0 && zip_has(zip, "logs/syslog"),
		"exit %d, %d bytes", status, (int)zip.size());
	unlink("/tmp/ce-system-report2.zip");
}


// #pragma mark - main


int
main(int argc, char** argv)
{
	if (argc >= 2 && strcmp(argv[1], "--crash-app") == 0) {
		crash_here();
		return 0;
	}

	BApplication app("application/x-vnd.CE-crashlog-test");

	std::string phaseText = read_file(kPhaseFile);
	int phase = phaseText.empty() ? 0 : atoi(phaseText.c_str());
	bool crash = argc >= 2 && strcmp(argv[1], "--crash") == 0;

	printf("CE-SUITE %s START phase %d\n", SUITE, phase);
	fflush(stdout);

	if (phase == 0) {
		if (!crash) {
			info("not started by the runner's crash phase; nothing to do");
			printf("CE-SUITE %s DONE failures=0\n", SUITE);
			return 0;
		}
		report("collector_installed", access(kCollector, X_OK) == 0, "%s",
			kCollector);
		check_collector();
		check_application_crash(argv[0]);
		set_phase(1);
		enter_debugger("panic");
		// only reached if the debugger did not reboot
		unlink(kPhaseFile);
	} else if (phase == 1) {
		std::string path;
		std::string text = newest_kernel_report("requested from kdl driver",
			&path);
		report("report_saved", !text.empty(), "%s", text.empty()
			? "no kernel-*.txt with the panic in " CRASH_REPORT_DIRECTORY
			: path.c_str());
		if (!text.empty()) {
			check_kernel_report(text, CRASH_REPORT_REASON_PANIC,
				"PANIC: requested from kdl driver.");
			size_t tail = text.find("==== syslog before the crash");
			report("report_syslog_tail", tail != std::string::npos
				&& text.find("kdl: read", tail) != std::string::npos,
				"syslog before the crash %s the kdl driver's last line",
				tail != std::string::npos
					&& text.find("kdl: read", tail) != std::string::npos
					? "ends with" : "lacks");
			report("previous_syslog", access(CRASH_REPORT_PREVIOUS_SYSLOG,
				F_OK) == 0, "%s", CRASH_REPORT_PREVIOUS_SYSLOG);
			check_gui(path);
		}
		set_phase(2);
		enter_debugger("user");
		unlink(kPhaseFile);
	} else {
		std::string text = newest_kernel_report("USER: CE crash test", NULL);
		report("user_report_saved", !text.empty(), "%s", text.empty()
			? "no report of the user's debugger entry" : "saved");
		if (!text.empty()) {
			report("user_report_reason",
				contains(text, "reason: " CRASH_REPORT_REASON_USER "\n")
				&& contains(text, "message: USER: CE crash test")
				&& contains(text.substr(0, text.find("==== syslog before")),
					CRASH_REPORT_END),
				"reason \"user request\", message \"USER: CE crash test\"");
		}
		std::vector<std::string> files = files_in(CRASH_REPORT_DIRECTORY,
			"kernel-");
		int panics = 0;
		for (size_t i = 0; i < files.size(); i++) {
			if (contains(read_file(files[i].c_str()),
					"requested from kdl driver"))
				panics++;
		}
		report("saved_once", panics == 1 && files.size() == 2,
			"%d kernel reports, %d of the panic (want 2 and 1)",
			(int)files.size(), panics);
		unlink(kPhaseFile);
	}

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures != 0;
}
