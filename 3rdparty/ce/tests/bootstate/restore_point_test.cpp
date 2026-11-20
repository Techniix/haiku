/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Restore points and automatic recovery (boot loader batch A), across
	the boots of a "ce-test.py --restore-points" run on one disk. The
	harness puts a flag file into the image; without it (every other run)
	this test does nothing. The phase is kept in kPhaseFile.

	A  (first boot) An update: a small package is put into
	   /boot/system/packages, package_daemon activates it. Checked: the
	   start was confirmed, the update left a restore point (a new
	   state_<date> directory with package_daemon's restore point note),
	   the boot state's last working state now names that directory, and
	   the package is active.
	   (The harness then starts the disk twice and stops each start right
	   after the boot loader counted it: two failed starts.)
	D  (started by the recovery menu's countdown) Checked: the boot loader
	   started the last working state (the restore point, without the
	   package), and this start was confirmed. Then "bootstate next menu"
	   asks for the boot menu at the next start only.
	E  (the menu was shown; the harness picked the restore point under
	   "System restore points") Checked: the one-shot choice was taken off
	   the disk, the picked state is the one running, the package is not
	   active.
*/


#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <algorithm>

#include <boot_state_file.h>


#define SUITE "restore_points"

static const char* kFlagFile
	= "/boot/system/non-packaged/data/ce-harness/restore-points";
static const char* kPhaseFile = "/boot/home/ce-restore-phase";
static const char* kAdministrative = "/boot/system/packages/administrative";
static const char* kMarker = "/boot/system/data/ce-restore-point/marker";
static const char* kWork = "/boot/home/ce-restore-work";

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


static int
run(const std::string& command, std::string& output)
{
	output.clear();
	FILE* pipe = popen((command + " 2>&1").c_str(), "r");
	if (pipe == NULL)
		return -1;
	char buffer[1024];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, length);
	return pclose(pipe);
}


static std::string
read_text(const char* path)
{
	std::string text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[1024];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, length);
	fclose(file);
	return text;
}


static void
write_text(const char* path, const std::string& text)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	fwrite(text.data(), 1, text.size(), file);
	fflush(file);
	fsync(fileno(file));
	fclose(file);
}


static std::vector<std::string>
states()
{
	std::vector<std::string> names;
	DIR* dir = opendir(kAdministrative);
	if (dir == NULL)
		return names;
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "state_", 6) == 0)
			names.push_back(entry->d_name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());
	return names;
}


//!	Waits until the bootstate launch job confirmed this start.
static bool
wait_confirmed(BootState::Record& record)
{
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	for (int i = 0; i < 180; i++) {
		if (BootState::read_file(path, record) >= 0 && record.lastGoodValid
			&& record.failedStarts == 0)
			return true;
		if (i % 15 == 14) {
			printf("CE-INFO waiting for bootstate to confirm the start\n");
			fflush(stdout);
		}
		sleep(1);
	}
	return false;
}


static bool
make_package(std::string& file, std::string& error)
{
	std::string output;
	std::string content = std::string(kWork) + "/content";
	run(std::string("rm -rf ") + kWork, output);
	if (run("mkdir -p " + content + "/data/ce-restore-point", output) != 0) {
		error = "mkdir: " + output;
		return false;
	}
	write_text((content + "/data/ce-restore-point/marker").c_str(),
		"installed by the restore point test\n");
	write_text((content + "/.PackageInfo").c_str(),
		"name\t\t\tce_restore_marker\n"
		"version\t\t\t1.0-1\n"
		"architecture\tany\n"
		"summary\t\t\t\"Renga harness test package\"\n"
		"description\t\t\"Only used by the restore point test.\"\n"
		"packager\t\t\"Renga harness <harness@renga.invalid>\"\n"
		"vendor\t\t\t\"Techniix\"\n"
		"copyrights\t\t\"2026 Renga contributors\"\n"
		"licenses\t\t\"MIT\"\n"
		"provides {\n\tce_restore_marker = 1.0\n}\n"
		"requires {\n}\n");
	file = std::string(kWork) + "/ce_restore_marker-1.0-1-any.hpkg";
	if (run("package create -q -C " + content + " " + file, output) != 0) {
		error = "package create: " + output;
		return false;
	}
	return true;
}


static void
phase_update()
{
	BootState::Record record;
	bool confirmed = wait_confirmed(record);
	report("first_start_confirmed", confirmed
		&& record.booting[0] == '\0' && record.totalStarts >= 1,
		"failed %u, starts %u, booting \"%s\"",
		(unsigned)record.failedStarts, (unsigned)record.totalStarts,
		record.booting);

	std::vector<std::string> before = states();
	std::string file, error, output;
	bool made = make_package(file, error);
	bool copied = made && run("cp " + file + " /boot/system/packages/",
		output) == 0;
	bool active = false;
	for (int i = 0; copied && i < 120 && !active; i++) {
		struct stat st;
		active = stat(kMarker, &st) == 0;
		if (!active) {
			if (i % 15 == 14) {
				printf("CE-INFO waiting for the package to be activated\n");
				fflush(stdout);
			}
			sleep(1);
		}
	}
	report("update_active", active, "%s", !made ? error.c_str()
		: !copied ? output.c_str() : active ? "the package is active"
			: "the package did not become active in 120 s");

	std::vector<std::string> after = states();
	std::string created;
	for (size_t i = 0; i < after.size(); i++) {
		if (std::find(before.begin(), before.end(), after[i]) == before.end())
			created = after[i];
	}
	report("update_saved", !created.empty(), "restore point %s (%d before, "
		"%d after)", created.empty() ? "none" : created.c_str(),
		(int)before.size(), (int)after.size());

	std::string note = read_text((std::string(kAdministrative) + "/"
		+ created + "/" + BootState::kRestorePointFile).c_str());
	char label[160] = "";
	BootState::format_state_label(created.c_str(), note.c_str(), label,
		sizeof(label));
	report("note", note.find("replaced ") != std::string::npos
		&& note.find("changes 1") != std::string::npos
		&& strncmp(label, "Before the update of ", 21) == 0,
		"label \"%s\"", label);

	// package_daemon renamed "the latest" in the boot state
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	BootState::read_file(path, record);
	report("last_good_renamed", !created.empty() && record.lastGoodValid
		&& created == record.lastGood && created == record.booting,
		"last working \"%s\", booting \"%s\"", record.lastGood,
		record.booting);

	run("/bin/bootstate list", output);
	report("listed", !created.empty() && output.find(created + "\t"
		+ label) != std::string::npos, "%s", output.c_str());

	write_text(kPhaseFile, "updated " + created + "\n");
	sync();
}


static void
phase_recovered(const std::string& restorePoint)
{
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	BootState::Record record;
	BootState::read_file(path, record);
	std::string booting = record.booting;
	struct stat st;
	bool marker = stat(kMarker, &st) == 0;
	report("recovery_started_last_good", booting == restorePoint && !marker,
		"booting \"%s\" (want \"%s\"), the update's package %s",
		booting.c_str(), restorePoint.c_str(),
		marker ? "is active" : "is not active");
	report("recovery_counted", record.totalStarts >= 4,
		"%u starts recorded (the first, two stopped, this one)",
		(unsigned)record.totalStarts);

	bool confirmed = wait_confirmed(record);
	report("recovery_confirmed", confirmed && restorePoint == record.lastGood,
		"failed %u, last working \"%s\"", (unsigned)record.failedStarts,
		record.lastGood);

	std::string output;
	int status = run("/bin/bootstate next menu", output);
	BootState::read_file(path, record);
	report("next_menu_set", status == 0
		&& strcmp(record.nextOnce, "menu") == 0, "\"%s\" %s",
		record.nextOnce, output.c_str());

	write_text(kPhaseFile, "recovered " + restorePoint + "\n");
	sync();
}


static void
phase_picked(const std::string& restorePoint)
{
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	BootState::Record record;
	BootState::read_file(path, record);
	report("one_shot_taken", record.nextOnce[0] == '\0', "next start \"%s\"",
		record.nextOnce);
	struct stat st;
	bool marker = stat(kMarker, &st) == 0;
	report("picked_older_state", restorePoint == record.booting && !marker,
		"booting \"%s\" (want \"%s\"), the update's package %s",
		record.booting, restorePoint.c_str(),
		marker ? "is active" : "is not active");
	bool confirmed = wait_confirmed(record);
	report("picked_confirmed", confirmed, "failed %u",
		(unsigned)record.failedStarts);
	write_text(kPhaseFile, "done\n");
	sync();
}


int
main()
{
	struct stat st;
	if (stat(kFlagFile, &st) != 0) {
		printf("CE-INFO restore_point_test: not a --restore-points run\n");
		return 0;
	}

	std::string phase = read_text(kPhaseFile);
	std::string argument;
	size_t space = phase.find(' ');
	if (space != std::string::npos) {
		argument = phase.substr(space + 1);
		while (!argument.empty() && (argument.back() == '\n'
				|| argument.back() == '\r'))
			argument.pop_back();
		phase = phase.substr(0, space);
	}
	printf("CE-INFO restore_point_test: phase \"%s\"\n",
		phase.empty() ? "update" : phase.c_str());
	fflush(stdout);

	if (phase.empty())
		phase_update();
	else if (phase == "updated")
		phase_recovered(argument);
	else if (phase == "recovered")
		phase_picked(argument);
	else
		report("phase", false, "unexpected phase \"%s\"", phase.c_str());
	return sFailures == 0 ? 0 : 1;
}
