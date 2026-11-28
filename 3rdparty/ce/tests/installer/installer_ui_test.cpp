/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The Installer's window for installing over an existing installation
	(roadmap F-23), on images with a desktop.

	The harness attaches a 64 MiB BFS disk "CETarget" with what looks like
	an installation: a system package file named
	haiku-r1~beta5_hrev57937-1-x86_64.hpkg and a user file. Then:
	- "Installer --list-targets" names it;
	- "Installer --target CETarget --begin" opens the window with that
	  destination chosen and presses Begin: the installation mode window
	  comes up and waits (screenshot installer_modes: the heading names
	  the installation, upgrade is chosen; F-59: the volume is named
	  "Haiku" for this, so "Rename the volume to \"Renga\"" is offered,
	  checked);
	- the Installer is then killed;
	then the wizard (batch 14, F-23, the default window):
	- "Installer --which-window" says "wizard" without a setting and
	  "classic" with classic_window yes in ~/config/settings/Renga/
	  installer (the setting is put back afterwards);
	- "Installer --wizard-summary" (the Summary page's list, as text):
	  upgrading CETarget names its installation and leaves the partition
	  table alone; erasing CETarget's 64 MiB disk is refused as too small;
	  erasing the disk the system runs from is refused as such;
	- "Installer --wizard --page <page> --choice existing --target
	  CETarget --mode upgrade" shows the Welcome, Destination and Summary
	  pages (screenshots installer_wizard_welcome, _destination,
	  _summary); the Summary page prints the same list as
	  --wizard-summary;
	- in the end the volume is unchanged.
	The disk is a snapshot; nothing it gets is kept.
*/


#include <Application.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <FindDirectory.h>
#include <Partition.h>
#include <Path.h>
#include <VolumeRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>
#include <Volume.h>
#include <fs_info.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "installer_ui"

static const char* kInstaller = "/boot/system/apps/Installer";
static const char* kSignature = "application/x-vnd.Haiku-Installer";

static int sFailures = 0;


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


static BString
run(const char* command, int* _status = NULL)
{
	BString output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.Append(buffer, bytes);
	int status = pclose(pipe);
	if (_status != NULL)
		*_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return output;
}






static void
screenshot(const char* name)
{
	printf("CE-HOST screenshot %s\n", name);
	fflush(stdout);
	snooze(1500000);
}


//!	\a text on one line, at most 300 characters.
static BString
one_line(BString text)
{
	text.ReplaceAll("\n", " | ");
	if (text.Length() > 300) {
		text.Truncate(297);
		text << "...";
	}
	return text;
}


/*!	Starts the Installer with \a arguments and its output in a pipe;
	collects the output until a line contains \a wanted (or \a seconds
	pass), takes \a shot then, and kills it. Returns what it printed.
*/
static BString
run_window(const char* const* arguments, const char* wanted,
	const char* shot, int seconds, bool* _seen)
{
	int fds[2];
	BString output;
	*_seen = false;
	if (pipe(fds) != 0)
		return output;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		const char* argv[16];
		int count = 0;
		argv[count++] = kInstaller;
		for (int i = 0; arguments[i] != NULL && count < 15; i++)
			argv[count++] = arguments[i];
		argv[count] = NULL;
		execv(kInstaller, (char* const*)argv);
		_exit(127);
	}
	close(fds[1]);
	bigtime_t end = system_time() + seconds * 1000000LL;
	while (pid > 0 && system_time() < end
		&& output.FindFirst(wanted) < 0) {
		struct pollfd entry = { fds[0], POLLIN, 0 };
		if (poll(&entry, 1, 500) <= 0)
			continue;
		char buffer[1024];
		ssize_t bytes = read(fds[0], buffer, sizeof(buffer));
		if (bytes <= 0)
			break;
		output.Append(buffer, bytes);
	}
	*_seen = output.FindFirst(wanted) >= 0;
	if (*_seen) {
		// the page is laid out and drawn
		snooze(1500000);
		screenshot(shot);
	}
	if (pid > 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
	// what it printed meanwhile
	int flags = fcntl(fds[0], F_GETFL);
	fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
	char buffer[1024];
	ssize_t bytes;
	while ((bytes = read(fds[0], buffer, sizeof(buffer))) > 0)
		output.Append(buffer, bytes);
	close(fds[0]);
	return output;
}


//!	The "summary: " lines of \a output, in order, without the prefix.
static BString
summary_lines(const BString& output)
{
	BString lines;
	int32 start = 0;
	while (start < output.Length()) {
		int32 end = output.FindFirst('\n', start);
		if (end < 0)
			end = output.Length();
		BString line;
		output.CopyInto(line, start, end - start);
		start = end + 1;
		int32 at = line.FindFirst("summary: ");
		if (at >= 0)
			lines << line.String() + at + 9 << "\n";
	}
	return lines;
}


static BString
read_text(const char* path, bool* _exists = NULL)
{
	BString text;
	FILE* file = fopen(path, "r");
	if (_exists != NULL)
		*_exists = file != NULL;
	if (file == NULL)
		return text;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.Append(buffer, bytes);
	fclose(file);
	return text;
}


static void
write_text(const char* path, const BString& text)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	fwrite(text.String(), 1, text.Length(), file);
	fclose(file);
}


//!	The wizard: its setting, its summary as text, and its pages.
static void
check_wizard(const BString& listing)
{
	// the setting: the wizard by default, the classic window when asked
	BPath settings;
	find_directory(B_USER_SETTINGS_DIRECTORY, &settings);
	BString path(settings.Path());
	path << "/Renga";
	mkdir(path.String(), 0755);
	path << "/installer";
	bool existed;
	BString saved = read_text(path.String(), &existed);
	unlink(path.String());
	BString command;
	command << kInstaller << " --which-window";
	int status;
	BString none = run(command, &status);
	write_text(path.String(), "classic_window yes\n");
	BString classic = run(command, &status);
	if (existed)
		write_text(path.String(), saved);
	else
		unlink(path.String());
	none.Trim();
	classic.Trim();
	report("wizard_default", none == "wizard" && classic == "classic",
		"without a setting: \"%s\"; with classic_window yes: \"%s\"",
		none.String(), classic.String());

	// the Summary page's list as text
	command = "";
	command << kInstaller << " --wizard-summary --choice existing --target "
		"CETarget --mode upgrade 2>&1";
	BString summary = run(command, &status);
	report("wizard_summary_upgrade", status == 0
			&& summary.FindFirst("summary: Upgrade the installation on "
				"\"CETarget\" (Haiku r1~beta5_hrev57937") >= 0
			&& summary.FindFirst("summary: The partition table is not "
				"changed") >= 0
			&& summary.FindFirst("summary: Install ") >= 0
			&& summary.FindFirst("onto \"CETarget\"") >= 0,
		"exit %d: %s", status, one_line(summary).String());

	// CETarget's disk: the second column of its --list-targets line
	BString disk;
	int32 line = listing.FindFirst("CETarget\t");
	if (line >= 0) {
		int32 start = line + 9;
		int32 end = listing.FindFirst('\t', start);
		if (end > start)
			listing.CopyInto(disk, start, end - start);
	}
	command = "";
	command << kInstaller << " --wizard-summary --choice erase --disk '"
		<< disk << "' 2>&1";
	BString small = run(command, &status);
	report("wizard_summary_small", disk.Length() > 0 && status == 1
			&& small.FindFirst("installing needs at least") >= 0
			&& small.FindFirst("summary:") < 0,
		"%s: exit %d: %s", disk.String(), status, one_line(small).String());

	BString bootDisk;
	BVolume bootVolume;
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (BVolumeRoster().GetBootVolume(&bootVolume) == B_OK
		&& roster.FindPartitionByVolume(bootVolume, &device, &partition)
			== B_OK) {
		BPath devicePath;
		device.GetPath(&devicePath);
		bootDisk = devicePath.Path();
	}
	command = "";
	command << kInstaller << " --wizard-summary --choice erase --disk '"
		<< bootDisk << "' 2>&1";
	BString boot = run(command, &status);
	report("wizard_summary_boot_disk", bootDisk.Length() > 0 && status == 1
			&& boot.FindFirst("started from") >= 0
			&& boot.FindFirst("summary:") < 0,
		"%s: exit %d: %s", bootDisk.String(), status, one_line(boot).String());

	// the pages (the licence window is skipped with --page)
	const char* pages[] = { "welcome", "destination", "summary" };
	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		const char* arguments[] = { "--wizard", "--page", pages[i],
			"--choice", "existing", "--target", "CETarget", "--mode",
			"upgrade", NULL };
		BString wanted, shot, check;
		wanted << "wizard page " << pages[i];
		shot << "installer_wizard_" << pages[i];
		check << "wizard_page_" << pages[i];
		bool seen;
		BString output = run_window(arguments, wanted.String(),
			shot.String(), 40, &seen);
		bool pass = seen && output.FindFirst("opening the wizard") >= 0;
		if (strcmp(pages[i], "summary") == 0) {
			// the page lists what --wizard-summary printed
			pass = pass && summary_lines(output).Length() > 0
				&& summary_lines(output) == summary_lines(summary);
		}
		report(check.String(), pass, "%s: %s", seen ? "shown" : "not shown",
			one_line(output).String());
	}
}


int
main()
{
	BApplication app("application/x-vnd.Renga-installer-ui-test");
	printf("CE-SUITE %s START\n", SUITE);

	// The harness attaches "CETarget", a small BFS disk with what looks like
	// an installation (a system package file and a user file).
	int status;
	BString command;
	BString output;
	run("mountvolume CETarget 2>&1", &status);
	struct stat st;
	bool ready = stat("/CETarget/home/Documents/letter.txt", &st) == 0
		&& stat("/CETarget/system/packages/"
			"haiku-r1~beta5_hrev57937-1-x86_64.hpkg", &st) == 0;
	report("target_ready", ready, "CETarget %s",
		ready ? "mounted, with an installation" : "missing (the harness "
			"attaches it on renga-* runs)");
	if (!ready) {
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	command = "";
	command << kInstaller << " --list-targets";
	output = run(command, &status);
	report("list_targets", status == 0 && output.FindFirst("CETarget\t") >= 0
		&& output.FindFirst("Haiku r1~beta5_hrev57937") >= 0,
		"CETarget listed with its installation: %s",
		output.FindFirst("Haiku r1~beta5_hrev57937") >= 0 ? "yes" : "no");

	// The window: --target chooses CETarget, --begin skips the licence
	// window and presses Begin after the disk scan, so the installation
	// mode window comes up over it. (Talking to the Installer through
	// messengers and scripting hung this test in three attempts; the cause
	// was not found, so it does without.)
	// F-59: the volume is named "Haiku" for this, so the window also
	// offers to rename it (an upgraded volume keeps its name otherwise);
	// the mount point follows the name.
	BVolume volume;
	volume.SetTo(dev_for_path("/CETarget"));
	status_t renamed = volume.SetName("Haiku");
	printf("CE-INFO CETarget named \"Haiku\": %s\n", strerror(renamed));
	printf("CE-INFO starting %s --target Haiku --begin\n", kInstaller);
	fflush(stdout);
	team_id team = fork();
	if (team == 0) {
		execl(kInstaller, kInstaller, "--target", "Haiku", "--begin",
			(char*)NULL);
		_exit(127);
	}
	snooze(10000000);
	bool running = team > 0 && kill(team, 0) == 0;
	report("mode_window_waits", running,
		"the Installer %s after Begin (it waits for the mode window)",
		running ? "is still running" : "has exited");
	screenshot("installer_modes");

	if (team > 0) {
		kill(team, SIGKILL);
		waitpid(team, NULL, 0);
	}
	volume.SetName("CETarget");

	check_wizard(output);

	bool kept = stat("/CETarget/home/Documents/letter.txt", &st) == 0
		&& stat("/CETarget/system/packages/"
			"haiku-r1~beta5_hrev57937-1-x86_64.hpkg", &st) == 0;
	report("nothing_changed", kept, "without an answer the volume is %s",
		kept ? "unchanged" : "changed");

	run("unmount /CETarget 2>&1");

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures != 0;
}
