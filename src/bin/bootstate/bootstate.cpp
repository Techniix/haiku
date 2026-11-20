/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	bootstate: the running system's side of the boot state (Renga, roadmap
	F-20/F-22, boot loader batch A; the format and who writes what are in
	headers/private/shared/boot_state.h).

	  bootstate boot            create the file if needed, then confirm the
	                            start once the desktop is up (launch job)
	  bootstate confirm         confirm the current start now
	  bootstate show            the newest copy, as text
	  bootstate list            the restore points (package states), newest
	                            first: "<directory>\t<label>"
	  bootstate next menu       for the next start only: show the boot menu
	  bootstate next system <name>
	                            ... start another operating system the menu
	                            lists ("Windows Boot Manager")
	  bootstate next state <directory>
	                            ... start an older system state
	  bootstate next clear      no special next start
	  bootstate init            create the file if needed

	"Up" means: Deskbar (or the first-start prompt of an installation
	medium, which comes before it) has been running for kSettleSeconds. On a system
	without a Deskbar (the minimum images) the start counts as up once the
	launch job ran. Safe mode starts are not confirmed (the launch job does
	not run in safe mode): a system that only reaches the desktop in safe
	mode keeps its count, so the boot menu keeps offering the last working
	state.

	The file is never truncated, replaced or extended once it exists: the
	boot loader writes into the blocks it has. Only "init" creates it, at
	its full size, written out so that its blocks are allocated.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <OS.h>
#include <syslog.h>

#include <boot_state_file.h>


static const int kSettleSeconds = 5;


static int
read_state(const char* path, BootState::Record& record)
{
	return BootState::read_file(path, record);
}


static status_t
write_state(const char* path, BootState::Record& record, int newestSlot)
{
	return BootState::write_file(path, record, newestSlot);
}


static status_t
init_state(const char* path, bool verbose)
{
	BootState::Record record;
	if (read_state(path, record) != -2) {
		if (verbose)
			printf("bootstate: %s exists\n", path);
		return B_OK;
	}

	// a file of another size cannot be written in place by the loader:
	// keep it for a look, and start a new one
	struct stat st;
	if (lstat(path, &st) == 0) {
		char aside[B_PATH_NAME_LENGTH];
		snprintf(aside, sizeof(aside), "%s.bad", path);
		rename(path, aside);
	}

	char directory[B_PATH_NAME_LENGTH];
	strlcpy(directory, path, sizeof(directory));
	char* slash = strrchr(directory, '/');
	if (slash != NULL) {
		*slash = '\0';
		mkdir(directory, 0755);
	}

	uint8_t* file = (uint8_t*)calloc(1, BootState::kFileSize);
	if (file == NULL)
		return B_NO_MEMORY;
	BootState::init(record);
	BootState::prepare_write(record, file);

	char temporary[B_PATH_NAME_LENGTH];
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		free(file);
		return errno;
	}
	ssize_t written = write(fd, file, BootState::kFileSize);
	free(file);
	status_t status = written == (ssize_t)BootState::kFileSize ? B_OK
		: written < 0 ? errno : B_IO_ERROR;
	if (status == B_OK && fsync(fd) != 0)
		status = errno;
	close(fd);
	if (status == B_OK && rename(temporary, path) != 0)
		status = errno;
	if (status != B_OK)
		unlink(temporary);
	else if (verbose)
		printf("bootstate: created %s\n", path);
	return status;
}


static status_t
confirm(const char* path)
{
	BootState::Record record;
	int slot = read_state(path, record);
	if (slot == -2)
		return B_ENTRY_NOT_FOUND;
	uint32_t failed = record.failedStarts;
	BootState::confirm(record);
	status_t status = write_state(path, record, slot);
	if (status == B_OK) {
		syslog(LOG_INFO, "bootstate: start confirmed (\"%s\", %u unconfirmed "
			"before)", record.lastGood[0] != '\0' ? record.lastGood : "latest",
			(unsigned)failed);
		printf("bootstate: start confirmed (\"%s\", %u unconfirmed before)\n",
			record.lastGood[0] != '\0' ? record.lastGood : "latest",
			(unsigned)failed);
	}
	return status;
}


/*!	The desktop is up: Deskbar runs, or the first-start prompt of an
	installation medium ("Install" / "Try it out"), which comes before it.
*/
static bool
desktop_running()
{
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		// the program is the first word of the arguments; the others may
		// be paths too
		char program[sizeof(info.args)];
		strlcpy(program, info.args, sizeof(program));
		program[strcspn(program, " ")] = '\0';
		const char* name = strrchr(program, '/');
		name = name != NULL ? name + 1 : program;
		if (strcmp(name, "Deskbar") == 0
			|| strcmp(name, "FirstBootPrompt") == 0)
			return true;
	}
	return false;
}


static status_t
boot(const char* path)
{
	status_t status = init_state(path, false);
	if (status != B_OK) {
		fprintf(stderr, "bootstate: cannot create %s: %s\n", path,
			strerror(status));
		return status;
	}

	if (access("/boot/system/Deskbar", X_OK) == 0) {
		// No time limit: a start that reaches the desktop late (a slow
		// disk check, a long first boot) still worked. Until then this
		// sleeps; a start that never gets there stays unconfirmed.
		syslog(LOG_INFO, "bootstate: waiting for the desktop");
		int settled = 0;
		while (true) {
			if (desktop_running()) {
				if (++settled >= kSettleSeconds)
					return confirm(path);
			} else
				settled = 0;
			sleep(1);
		}
	}

	// no desktop on this system: the launch job ran, so the boot is through
	return confirm(path);
}


static void
show(const char* path)
{
	BootState::Record record;
	int slot = read_state(path, record);
	if (slot == -2) {
		printf("no boot state (%s)\n", path);
		return;
	}
	printf("file: %s\n", path);
	printf("slot: %d\n", slot);
	printf("sequence: %u\n", (unsigned)record.sequence);
	printf("failed starts: %u\n", (unsigned)record.failedStarts);
	printf("starts: %u\n", (unsigned)record.totalStarts);
	printf("booting: %s\n", record.booting[0] != '\0' ? record.booting
		: "latest");
	printf("last working: %s\n", !record.lastGoodValid ? "none yet"
		: record.lastGood[0] != '\0' ? record.lastGood : "latest");
	printf("next start: %s\n", record.nextOnce[0] != '\0' ? record.nextOnce
		: "normal");
}


static void
list()
{
	printf("latest\tLatest state\n");
	static char names[512][BootState::kNameLength];
	int count = BootState::list_restore_points(names, 512);
	for (int i = 0; i < count; i++) {
		char label[160];
		BootState::restore_point_label(names[i], label, sizeof(label));
		printf("%s\t%s\n", names[i], label);
	}
}


static status_t
next(const char* path, const char* what, const char* argument)
{
	char value[BootState::kNextLength];
	if (strcmp(what, "menu") == 0)
		strlcpy(value, "menu", sizeof(value));
	else if (strcmp(what, "clear") == 0)
		value[0] = '\0';
	else if ((strcmp(what, "system") == 0 || strcmp(what, "state") == 0)
		&& argument != NULL && argument[0] != '\0') {
		snprintf(value, sizeof(value), "%s:%s", what, argument);
	} else
		return B_BAD_VALUE;

	status_t status = init_state(path, false);
	if (status != B_OK)
		return status;
	BootState::Record record;
	int slot = read_state(path, record);
	BootState::set_name(record.nextOnce, sizeof(record.nextOnce), value);
	return write_state(path, record, slot);
}


static void
usage()
{
	fprintf(stderr, "usage: bootstate boot | confirm | show | list | init\n"
		"       bootstate next menu | clear | system <name> "
		"| state <directory>\n");
	exit(1);
}


int
main(int argc, char** argv)
{
	if (argc < 2)
		usage();

	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	const char* command = argv[1];
	status_t status = B_OK;

	if (strcmp(command, "boot") == 0)
		status = boot(path);
	else if (strcmp(command, "confirm") == 0)
		status = confirm(path);
	else if (strcmp(command, "init") == 0)
		status = init_state(path, true);
	else if (strcmp(command, "show") == 0)
		show(path);
	else if (strcmp(command, "list") == 0)
		list();
	else if (strcmp(command, "next") == 0 && argc >= 3) {
		status = next(path, argv[2], argc >= 4 ? argv[3] : NULL);
		if (status == B_BAD_VALUE)
			usage();
	} else
		usage();

	if (status != B_OK) {
		fprintf(stderr, "bootstate: %s: %s\n", command, strerror(status));
		return 1;
	}
	return 0;
}
