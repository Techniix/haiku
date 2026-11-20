/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The boot state (boot loader batch A; headers/private/shared/
	boot_state.h), in two parts:

	- The format, as the boot loader, bootstate and package_daemon use it:
	  the CRC, the choice of the newest valid copy (also after a torn write
	  of either copy, and across a sequence wrap), the slot rotation, boot
	  counting and confirmation, the renaming of "the latest state" when an
	  update turns it into an old one, the threshold for the recovery menu,
	  and the plain-date labels of restore points.
	- This boot: the image ships the file at its full size, the boot loader
	  counted this start in it (the loader is the only writer of the start
	  count) and the bootstate launch job confirmed it, so the start is not
	  counted as failed. bootstate's "next" choice is set and cleared again
	  through the tool.

	The --restore-points harness run (restore_point_test) covers the
	boots: failed starts, the recovery menu and starting an older state.
*/


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>

#include <boot_state_file.h>


#ifdef DESKTOP_PHASE
#	define SUITE "bootstate_desktop"
#else
#	define SUITE "bootstate"
#endif

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


static void
format_checks()
{
	using namespace BootState;

	// the standard CRC-32 check value
	report("crc32", crc32("123456789", 9) == 0xcbf43926, "0x%08x",
		(unsigned)crc32("123456789", 9));

	static uint8_t file[kFileSize];
	memset(file, 0, sizeof(file));
	Record record;
	int slot = newest(file, sizeof(file), record);
	report("empty_file", slot == -1 && record.magic == kMagic
		&& record.failedStarts == 0, "slot %d", slot);

	// two starts, written the way every writer does
	start_attempt(record, NULL);
	prepare_write(record, file + next_slot(slot) * kSlotSize);
	int first = next_slot(slot);
	Record read;
	slot = newest(file, sizeof(file), read);
	report("first_write", slot == first && read.failedStarts == 1
		&& read.sequence == 1, "slot %d sequence %u failed %u", slot,
		(unsigned)read.sequence, (unsigned)read.failedStarts);

	start_attempt(read, "state_2026-11-20_14:05:33");
	prepare_write(read, file + next_slot(slot) * kSlotSize);
	int second = next_slot(slot);
	slot = newest(file, sizeof(file), record);
	report("slots_alternate", second != first && slot == second
		&& record.failedStarts == 2
		&& strcmp(record.booting, "state_2026-11-20_14:05:33") == 0,
		"first %d second %d newest %d", first, second, slot);
	Record once = record;
	once.failedStarts = 1;
	report("recovery_threshold", needs_recovery(record)
		&& !needs_recovery(once), "recovery after %u failed starts, not "
		"after 1", (unsigned)record.failedStarts);

	// a power cut in the middle of the newest write: its copy is torn,
	// the previous one is taken
	uint8_t saved[kSlotSize];
	memcpy(saved, file + second * kSlotSize, kSlotSize);
	file[second * kSlotSize + 40] ^= 0x5a;
	slot = newest(file, sizeof(file), record);
	report("torn_newest", slot == first && record.failedStarts == 1,
		"newest valid slot %d (want %d)", slot, first);
	memcpy(file + second * kSlotSize, saved, kSlotSize);

	// ... and a torn older copy does not matter
	file[first * kSlotSize + 8] ^= 0x01;
	slot = newest(file, sizeof(file), record);
	report("torn_older", slot == second, "slot %d", slot);
	file[first * kSlotSize + 8] ^= 0x01;

	// sequence numbers wrap
	Record a, b;
	init(a);
	init(b);
	a.sequence = 0xffffffff;
	b.sequence = 0;
	seal(a);
	seal(b);
	static uint8_t wrap[kFileSize];
	memset(wrap, 0, sizeof(wrap));
	memcpy(wrap, &a, sizeof(a));
	memcpy(wrap + kSlotSize, &b, sizeof(b));
	slot = newest(wrap, sizeof(wrap), record);
	report("sequence_wrap", slot == 1, "slot %d (want 1)", slot);

	// confirmation, then an update renames "the latest"
	init(record);
	start_attempt(record, NULL);
	confirm(record);
	report("confirm", record.failedStarts == 0 && record.lastGoodValid
		&& record.lastGood[0] == '\0', "failed %u valid %u",
		(unsigned)record.failedStarts, (unsigned)record.lastGoodValid);
	bool changed = latest_became(record, "state_2026-11-20_15:30:00");
	report("latest_became", changed
		&& strcmp(record.lastGood, "state_2026-11-20_15:30:00") == 0
		&& strcmp(record.booting, "state_2026-11-20_15:30:00") == 0,
		"last good \"%s\", booting \"%s\"", record.lastGood, record.booting);
	changed = latest_became(record, "state_2026-11-21_09:00:00");
	report("latest_became_once", !changed
		&& strcmp(record.lastGood, "state_2026-11-20_15:30:00") == 0,
		"last good \"%s\"", record.lastGood);
	init(record);
	report("latest_became_unconfirmed", !latest_became(record, "")
		|| record.lastGood[0] == '\0', "an unconfirmed last good stays "
		"unset");

	// the one-shot choice is cleared by the next start
	set_name(record.nextOnce, sizeof(record.nextOnce), "system:Windows");
	start_attempt(record, NULL);
	report("next_cleared_by_start", record.nextOnce[0] == '\0', "\"%s\"",
		record.nextOnce);

	// long names are cut, never overrun
	char longName[300];
	memset(longName, 'x', sizeof(longName) - 1);
	longName[sizeof(longName) - 1] = '\0';
	set_name(record.booting, sizeof(record.booting), longName);
	seal(record);
	report("long_name", strlen(record.booting) == kNameLength - 1
		&& is_valid(record), "length %d",
		(int)strlen(record.booting));

	// labels
	char label[160];
	bool ok = format_date("state_2026-11-20_14:05:33", label, sizeof(label));
	report("label_date", ok && strcmp(label, "20 November 2026, 14:05") == 0,
		"\"%s\"", label);
	ok = format_date("state_2026-01-02_03:04:05-2", label, sizeof(label));
	report("label_suffix", ok
		&& strcmp(label, "2 January 2026, 03:04 (2)") == 0, "\"%s\"", label);
	report("label_bad", !format_date("state_garbage", label, sizeof(label))
		&& !format_date("state_2026-13-01_00:00:00", label, sizeof(label)),
		"names without a date are left alone");
	ok = format_state_label("state_2026-11-20_14:05:33",
		"replaced 2026-11-20_15:30:12\nchanges 3\n", label, sizeof(label));
	report("label_restore_point", ok && strcmp(label,
		"Before the update of 20 November 2026, 15:30") == 0, "\"%s\"",
		label);
	ok = format_state_label("state_2026-11-20_14:05:33", NULL, label,
		sizeof(label));
	report("label_no_note", ok
		&& strcmp(label, "System of 20 November 2026, 14:05") == 0, "\"%s\"",
		label);
}


static int
run(const char* command, char* output, size_t size)
{
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return -1;
	size_t length = fread(output, 1, size - 1, pipe);
	output[length] = '\0';
	return pclose(pipe);
}


static bool
deskbar_running()
{
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (strncmp(info.args, "/boot/system/Deskbar", 20) == 0
			|| strstr(info.args, "FirstBootPrompt") != NULL)
			return true;
	}
	return false;
}


static void
live_checks()
{
	char path[B_PATH_NAME_LENGTH];
	BootState::file_path(path, sizeof(path));
	struct stat st;
	bool exists = stat(path, &st) == 0;
	report("file", exists && st.st_size == (off_t)BootState::kFileSize,
		"%s: %s, %lld bytes", path, exists ? "present" : "missing",
		exists ? (long long)st.st_size : 0LL);

	// bootstate (launch job) confirms once the boot is through: at once
	// without a desktop, a few seconds after Deskbar started with one. In
	// the renga-* harness images the desktop only comes up for the second
	// phase (ce-tests-media, with media_server): there
	// bootstate_desktop_test, the same program, checks the confirmation.
	BootState::Record record;
	int slot = BootState::read_file(path, record);
#ifndef DESKTOP_PHASE
	if (access("/boot/system/servers/media_server", X_OK) == 0
		&& !deskbar_running()) {
		printf("CE-INFO a desktop system: the confirmation is checked by "
			"bootstate_desktop_test\n");
		report("counted_by_loader", slot >= 0 && record.totalStarts >= 1,
			"slot %d, %u start(s) recorded by the boot loader", slot,
			(unsigned)record.totalStarts);
		return;
	}
#endif
	for (int i = 0; i < 300; i++) {
		slot = BootState::read_file(path, record);
		if (slot >= 0 && record.lastGoodValid && record.failedStarts == 0)
			break;
		if (i % 15 == 14) {
			printf("CE-INFO waiting for bootstate to confirm the start\n");
			fflush(stdout);
		}
		sleep(1);
	}
	if (!(slot >= 0 && record.lastGoodValid && record.failedStarts == 0)) {
		char teams[4096];
		run("ps 2>&1 | grep -E 'Deskbar|bootstate|app_server|Tracker'; "
			"grep bootstate /var/log/syslog 2>&1 | tail -5", teams,
			sizeof(teams));
		printf("CE-INFO not confirmed; teams and log:\n%s\n", teams);
	}
	report("counted_by_loader", slot >= 0 && record.totalStarts >= 1,
		"slot %d, %u start(s) recorded by the boot loader", slot,
		(unsigned)record.totalStarts);
	report("confirmed", slot >= 0 && record.lastGoodValid
		&& record.failedStarts == 0
		&& strcmp(record.lastGood, record.booting) == 0,
		"failed %u, last working \"%s\" (valid %u), booting \"%s\"",
		(unsigned)record.failedStarts, record.lastGood,
		(unsigned)record.lastGoodValid, record.booting);

	// the one-shot choice through the tool, then cleared again
	char output[2048];
	int status = run("/bin/bootstate next system 'Windows Boot Manager' 2>&1",
		output, sizeof(output));
	BootState::read_file(path, record);
	bool set = status == 0
		&& strcmp(record.nextOnce, "system:Windows Boot Manager") == 0;
	status = run("/bin/bootstate next clear 2>&1", output, sizeof(output));
	BootState::Record cleared;
	BootState::read_file(path, cleared);
	report("next_choice", set && status == 0 && cleared.nextOnce[0] == '\0'
		&& cleared.failedStarts == record.failedStarts,
		"set \"%s\", then \"%s\"", record.nextOnce, cleared.nextOnce);

	status = run("/bin/bootstate show 2>&1", output, sizeof(output));
	// (the running state is the latest one, or in a --restore-points run
	// the restore point)
	const char* lastWorking = strstr(output, "last working: ");
	report("show", status == 0 && strstr(output, "failed starts: 0") != NULL
		&& lastWorking != NULL && strncmp(lastWorking + 14, "none", 4) != 0,
		"%.*s", lastWorking != NULL ? (int)strcspn(lastWorking, "\n") : 0,
		lastWorking != NULL ? lastWorking : "");

	status = run("/bin/bootstate list 2>&1", output, sizeof(output));
	report("list", status == 0 && strncmp(output, "latest\t", 7) == 0,
		"%.*s", (int)strcspn(output, "\n"), output);
}


int
main()
{
#ifndef DESKTOP_PHASE
	format_checks();
#endif
	live_checks();
	return sFailures == 0 ? 0 : 1;
}
