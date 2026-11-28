/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Renga's shared disk-management engine (F-24, first
	part) through its command-line front end, rengadisk, on a disk image
	file only (a 64 MiB file in the home folder, attached as a file disk).
	Nothing else is written: the refusal checks stop before any write.

	  disk_engine.attach            the image becomes a disk; "list" shows
	                                it as an image without a table
	  disk_engine.preview_only      a plan without --apply shows its steps
	                                and writes nothing
	  disk_engine.apply             table:gpt and two partitions: the disk
	                                has a GUID Partition Map with "one" and
	                                "two" of 16 MiB
	  disk_engine.journal           the table was saved before the first
	                                write; the journal lists each step and
	                                "complete"
	  disk_engine.refuses_boot_disk a plan for the boot disk is refused
	  disk_engine.refuses_real_disk a plan for a disk that is not an image
	                                is refused without --allow-real-disks
	                                (only checked when the machine has one
	                                besides the boot disk)
	  disk_engine.refuses_bad_step  a plan naming a partition that does not
	                                exist is refused before anything is
	                                written
	  disk_engine.refuses_other_device
	                                a plan made for another device ID
	                                (another stick in the same port,
	                                RengaDisk::Plan::ExpectDevice) is
	                                refused by Check() and Apply() before
	                                the table backup; the disk's own ID
	                                passes Check() (the engine directly)
	  disk_engine.refuses_larger_table
	                                an image whose GPT was made for a
	                                larger disk (128 MiB, cut to 64 MiB):
	                                a new table is refused ("describes a
	                                larger disk"; changing it crashed
	                                libbe), "rengadisk wipe" clears it
	                                (saved first), then a new table works
	  disk_engine.whole_disk_fat_not_refused
	                                a FAT volume on a whole disk (no
	                                partition table) whose boot code
	                                bytes would read as an MBR entry
	                                past the disk's end: a new table is
	                                not refused (only an Intel map's
	                                entries are checked)
	  disk_engine.restore           a saved table written back brings a
	                                deleted partition back
	  disk_engine.format            partition 0 formatted as BFS named Data
	  disk_engine.detach            the image is detached again

	Output format: see 3rdparty/ce/README.md.
*/


#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceTypes.h>
#include <DiskEngine.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "disk_engine"

static const char* kTool = "/boot/system/non-packaged/bin/rengadisk";
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[700];
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


//!	Runs rengadisk with \a arguments; its output and exit status.
static BString
tool(const char* arguments, int* _status = NULL)
{
	BString command;
	command << kTool << " " << arguments << " 2>&1";
	BString output;
	FILE* pipe = popen(command.String(), "r");
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


static BString
one_line(BString text)
{
	text.ReplaceAll('\n', ' ');
	text.ReplaceAll('\t', ' ');
	if (text.Length() > 300)
		text.Truncate(300);
	return text;
}


//!	"list"'s lines for the disk \a path: its "disk" line and partitions.
static BString
disk_lines(const BString& path)
{
	BString listing = tool("list");
	BString result;
	bool inDisk = false;
	int32 start = 0;
	while (start < listing.Length()) {
		int32 end = listing.FindFirst('\n', start);
		if (end < 0)
			end = listing.Length();
		BString line;
		listing.CopyInto(line, start, end - start);
		start = end + 1;
		if (line.StartsWith("disk\t"))
			inDisk = line.StartsWith(BString("disk\t") << path << "\t");
		if (inDisk)
			result << line << "\n";
	}
	return result;
}


//!	The first disk of "list" with \a mark in its line and not \a without.
static BString
find_disk(const char* mark, const char* without)
{
	BString listing = tool("list");
	int32 start = 0;
	while (start < listing.Length()) {
		int32 end = listing.FindFirst('\n', start);
		if (end < 0)
			end = listing.Length();
		BString line;
		listing.CopyInto(line, start, end - start);
		start = end + 1;
		if (!line.StartsWith("disk\t")
			|| (mark != NULL && line.FindFirst(mark) < 0)
			|| (without != NULL && line.FindFirst(without) >= 0))
			continue;
		BString path;
		int32 tab = line.FindFirst('\t', 5);
		line.CopyInto(path, 5, tab - 5);
		return path;
	}
	return "";
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	if (access(kTool, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kTool);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	BPath home;
	find_directory(B_USER_DIRECTORY, &home);
	BString image = home.Path();
	image << "/disk-engine-test.img";
	BString backups = home.Path();
	backups << "/disk-engine-test-backups";
	BString command;
	command.SetToFormat("rm -rf '%s' '%s'", image.String(), backups.String());
	system(command.String());
	int fd = open(image.String(), O_CREAT | O_RDWR | O_TRUNC, 0644);
	bool created = fd >= 0 && ftruncate(fd, 64 * 1024 * 1024) == 0;
	if (fd >= 0)
		close(fd);

	int status = -1;
	BString attachArgs;
	attachArgs << "attach '" << image << "'";
	BString disk = tool(attachArgs.String(), &status);
	disk.Trim();
	BString lines = disk_lines(disk);
	report("attach", created && status == 0 && disk.StartsWith("/dev/disk")
			&& lines.FindFirst("(no partition table)") >= 0
			&& lines.FindFirst("image ") >= 0,
		"image created %d, attach exit %d, disk \"%s\": %s", created, status,
		disk.String(), one_line(lines).String());
	if (status != 0 || !disk.StartsWith("/dev/disk")) {
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	BString steps;
	steps << "plan " << disk << " table:gpt create:16M:one create:16M:two";
	BString preview = tool(steps.String(), &status);
	lines = disk_lines(disk);
	report("preview_only", status == 0
			&& preview.FindFirst("new partition table: GUID Partition Map")
				>= 0
			&& preview.FindFirst("nothing written") >= 0
			&& lines.FindFirst("(no partition table)") >= 0,
		"exit %d: %s; disk afterwards: %s", status,
		one_line(preview).String(), one_line(lines).String());

	BString applyArgs(steps);
	applyArgs << " --apply --backup-dir '" << backups << "'";
	BString applied = tool(applyArgs.String(), &status);
	lines = disk_lines(disk);
	report("apply", status == 0 && lines.FindFirst("GUID Partition Map") >= 0
			&& lines.FindFirst("\"one\"") >= 0
			&& lines.FindFirst("\"two\"") >= 0
			&& lines.FindFirst("16.0 MiB") >= 0,
		"exit %d: %s; disk: %s", status, one_line(applied).String(),
		one_line(lines).String());

	// the backup and the journal
	BString journal;
	int tables = 0;
	if (DIR* dir = opendir(backups.String())) {
		while (dirent* entry = readdir(dir)) {
			BString name(entry->d_name);
			if (name.EndsWith(".table"))
				tables++;
			if (name.EndsWith(".journal")) {
				BString path(backups);
				path << "/" << name;
				if (FILE* file = fopen(path.String(), "r")) {
					char buffer[2048];
					size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
					buffer[length] = '\0';
					journal = buffer;
					fclose(file);
				}
			}
		}
		closedir(dir);
	}
	report("journal", tables == 1 && journal.FindFirst("step 1:") >= 0
			&& journal.FindFirst("step 3:") >= 0
			&& journal.FindFirst("complete") >= 0,
		"%d table backup(s); journal: %s", tables,
		one_line(journal).String());

	BString bootDisk = find_disk("boot disk", NULL);
	BString bootArgs;
	bootArgs << "plan " << bootDisk << " table:gpt --apply";
	BString refused = tool(bootArgs.String(), &status);
	report("refuses_boot_disk", bootDisk.Length() > 0 && status == 1
			&& refused.FindFirst("refused: this is the disk the running "
				"system started from") >= 0,
		"boot disk \"%s\", exit %d: %s", bootDisk.String(), status,
		one_line(refused).String());

	BString realDisk;
	{
		// any disk that is neither an image nor the boot disk
		BString listing = tool("list");
		int32 start = 0;
		while (realDisk.Length() == 0 && start < listing.Length()) {
			int32 end = listing.FindFirst('\n', start);
			if (end < 0)
				end = listing.Length();
			BString line;
			listing.CopyInto(line, start, end - start);
			start = end + 1;
			// (not a CD drive without a disc: read-only or empty)
			if (line.StartsWith("disk\t") && line.FindFirst("image ") < 0
				&& line.FindFirst("boot disk") < 0
				&& line.FindFirst("read-only") < 0
				&& line.FindFirst("\t0 B\t") < 0) {
				int32 tab = line.FindFirst('\t', 5);
				line.CopyInto(realDisk, 5, tab - 5);
			}
		}
	}
	if (realDisk.Length() > 0) {
		BString realArgs;
		realArgs << "plan " << realDisk << " table:gpt";
		refused = tool(realArgs.String(), &status);
		report("refuses_real_disk", status == 1
				&& refused.FindFirst("not a disk image") >= 0,
			"disk \"%s\", exit %d: %s", realDisk.String(), status,
			one_line(refused).String());
	} else
		printf("CE-INFO disk_engine: no disk besides the boot disk and the "
			"image; refuses_real_disk not checked\n");

	BString badArgs;
	badArgs << "plan " << disk << " delete:7 --apply --backup-dir '"
		<< backups << "'";
	refused = tool(badArgs.String(), &status);
	lines = disk_lines(disk);
	report("refuses_bad_step", status == 1
			&& refused.FindFirst("no such partition") >= 0
			&& lines.FindFirst("\"two\"") >= 0,
		"exit %d: %s", status, one_line(refused).String());

	// a plan for the device the disk was when it was made: refused once
	// another device is at that path (review of PR #42)
	{
		RengaDisk::DiskInfo info;
		RengaDisk::GetDisk(disk.String(), info);
		BString otherBackups(backups);
		otherBackups << "/other-device";
		mkdir(otherBackups.String(), 0755);
		RengaDisk::Operation operation;
		operation.kind = RengaDisk::kSetName;
		operation.index = 0;
		operation.size = 0;
		operation.name = "renamed";
		RengaDisk::Plan other(disk.String());
		other.ExpectDevice(info.id + 1000);
		other.Add(operation);
		BString why, log;
		status_t checked = other.Check(why);
		status_t applied = other.Apply(otherBackups.String(), log);
		DIR* dir = opendir(otherBackups.String());
		int written = 0;
		while (dir != NULL && readdir(dir) != NULL)
			written++;
		if (dir != NULL)
			closedir(dir);
		RengaDisk::Plan same(disk.String());
		same.ExpectDevice(info.id);
		same.Add(operation);
		BString sameWhy;
		status_t sameChecked = same.Check(sameWhy);
		lines = disk_lines(disk);
		report("refuses_other_device", info.id >= 0
				&& checked == B_DEV_ID_ERROR && applied == B_DEV_ID_ERROR
				&& why.FindFirst("replaced") >= 0
				&& log.FindFirst("replaced") >= 0
				&& written == 2 /* "." and ".." only */
				&& sameChecked == B_OK && lines.FindFirst("renamed") < 0,
			"device %" B_PRId32 ": check \"%s\" (%s), apply \"%s\" (%s), "
			"%d entries in the backup folder; own ID: %s", info.id,
			why.String(), strerror(checked), one_line(log).String(),
			strerror(applied), written, sameWhy.Length() > 0
				? sameWhy.String() : strerror(sameChecked));
	}

	BString saved(backups);
	saved << "/before-delete.table";
	BString backupArgs;
	backupArgs << "backup " << disk << " '" << saved << "'";
	tool(backupArgs.String(), &status);
	int backupStatus = status;
	BString deleteArgs;
	deleteArgs << "plan " << disk << " delete:1 --apply --backup-dir '"
		<< backups << "'";
	tool(deleteArgs.String(), &status);
	BString afterDelete = disk_lines(disk);
	BString restoreArgs;
	restoreArgs << "restore " << disk << " '" << saved << "'";
	BString restored = tool(restoreArgs.String(), &status);
	restored.Trim();
	// the image is attached again under a new path
	if (status == 0 && restored.StartsWith("/dev/disk"))
		disk = restored;
	snooze(500000);
	lines = disk_lines(disk);
	report("restore", backupStatus == 0
			&& afterDelete.FindFirst("\"two\"") < 0
			&& status == 0 && lines.FindFirst("\"two\"") >= 0,
		"backup exit %d; after delete: %s; restore exit %d %s; after "
		"restore: %s", backupStatus, one_line(afterDelete).String(), status,
		one_line(restored).String(), one_line(lines).String());

	BString formatArgs;
	formatArgs << "plan " << disk << " format:0:bfs:Data --apply "
		"--backup-dir '" << backups << "'";
	BString formatted = tool(formatArgs.String(), &status);
	lines = disk_lines(disk);
	report("format", status == 0 && lines.FindFirst("Be File System") >= 0
			&& lines.FindFirst("\"Data\"") >= 0,
		"exit %d: %s; disk: %s", status, one_line(formatted).String(),
		one_line(lines).String());

	// a table for a larger disk (review of PR #42): refused, then wiped
	{
		BString larger = home.Path();
		larger << "/disk-engine-larger.img";
		int largerFD = open(larger.String(), O_CREAT | O_RDWR | O_TRUNC,
			0644);
		bool made = largerFD >= 0
			&& ftruncate(largerFD, 128 * 1024 * 1024) == 0;
		if (largerFD >= 0)
			close(largerFD);
		BString arguments;
		arguments << "attach '" << larger << "'";
		BString big = tool(arguments.String(), &status);
		big.Trim();
		arguments = "";
		arguments << "plan " << big << " table:gpt create:100M:big --apply "
			"--backup-dir '" << backups << "'";
		BString made100 = tool(arguments.String(), &status);
		made = made && status == 0;
		arguments = "";
		arguments << "detach '" << larger << "'";
		tool(arguments.String(), &status);
		made = made && status == 0 && truncate(larger.String(),
			64 * 1024 * 1024) == 0;
		arguments = "";
		arguments << "attach '" << larger << "'";
		BString cut = tool(arguments.String(), &status);
		cut.Trim();
		made = made && status == 0 && cut.StartsWith("/dev/disk");

		arguments = "";
		arguments << "plan " << cut << " table:gpt --apply --backup-dir '"
			<< backups << "'";
		int refusedStatus = -1;
		BString refusedOutput = tool(arguments.String(), &refusedStatus);

		arguments = "";
		arguments << "wipe " << cut << " --backup-dir '" << backups << "'";
		int wipeStatus = -1;
		BString wiped = tool(arguments.String(), &wipeStatus);
		// its last line is the disk's path (an image is attached again)
		BString wipedPath = cut;
		int32 at = wiped.FindLast("/dev/disk");
		if (at >= 0) {
			wiped.CopyInto(wipedPath, at, wiped.Length() - at);
			wipedPath.Trim();
		}

		arguments = "";
		arguments << "plan " << wipedPath << " table:gpt --apply "
			"--backup-dir '" << backups << "'";
		int afterStatus = -1;
		BString after = tool(arguments.String(), &afterStatus);
		BString afterLines = disk_lines(wipedPath);

		report("refuses_larger_table", made && refusedStatus == 1
				&& refusedOutput.FindFirst("describes a larger disk") >= 0
				&& refusedOutput.FindFirst("rengadisk wipe") >= 0
				&& wipeStatus == 0 && wiped.FindFirst("table saved") >= 0
				&& afterStatus == 0
				&& afterLines.FindFirst("GUID Partition Map") >= 0,
			"prepared %d (%s); new table on the cut image: exit %d: %s; "
			"wipe: exit %d: %s; new table after: exit %d: %s",
			made, one_line(made100).String(), refusedStatus,
			one_line(refusedOutput).String(), wipeStatus,
			one_line(wiped).String(), afterStatus,
			one_line(after).String());

		arguments = "";
		arguments << "detach '" << larger << "'";
		tool(arguments.String(), &status);
		unlink(larger.String());
	}

	// a whole-disk FAT volume: 0x55aa, and boot code where MBR entries
	// would be (review of PR #42)
	{
		BString fatImage = home.Path();
		fatImage << "/disk-engine-fat.img";
		int fatFD = open(fatImage.String(), O_CREAT | O_RDWR | O_TRUNC, 0644);
		bool made = fatFD >= 0 && ftruncate(fatFD, 64 * 1024 * 1024) == 0;
		if (fatFD >= 0)
			close(fatFD);
		BString arguments;
		arguments << "attach '" << fatImage << "'";
		BString fatDisk = tool(arguments.String(), &status);
		fatDisk.Trim();
		BDiskDeviceRoster roster;
		BDiskDevice device;
		status_t initialized = roster.GetDeviceForPath(fatDisk.String(),
			&device);
		if (initialized == B_OK)
			initialized = device.PrepareModifications();
		if (initialized == B_OK) {
			initialized = device.Initialize(kPartitionTypeFAT32, "FATSTICK",
				NULL);
			if (initialized == B_OK)
				initialized = device.CommitModifications();
			else
				device.CancelModifications();
		}
		// boot code that reads as a FAT32 entry from block 2048 with far
		// more blocks than the disk has
		uint8 entry[16] = {};
		entry[4] = 0x0c;
		uint32 start = 2048, count = 0x7fffffff;
		memcpy(entry + 8, &start, 4);
		memcpy(entry + 12, &count, 4);
		int raw = open(fatDisk.String(), O_RDWR);
		uint8 signature[2] = {};
		bool crafted = raw >= 0
			&& pwrite(raw, entry, sizeof(entry), 446) == (ssize_t)sizeof(entry)
			&& pread(raw, signature, 2, 510) == 2
			&& signature[0] == 0x55 && signature[1] == 0xaa;
		if (raw >= 0)
			close(raw);
		BString fatLines = disk_lines(fatDisk);

		arguments = "";
		arguments << "plan " << fatDisk << " table:gpt";
		int planStatus = -1;
		BString planned = tool(arguments.String(), &planStatus);
		report("whole_disk_fat_not_refused", made && initialized == B_OK
				&& crafted && fatLines.FindFirst("(no partition table)") >= 0
				&& planStatus == 0
				&& planned.FindFirst("describes a larger disk") < 0,
			"initialized %s, boot code crafted %d; disk: %s; plan exit %d: %s",
			strerror(initialized), crafted, one_line(fatLines).String(),
			planStatus, one_line(planned).String());

		arguments = "";
		arguments << "detach '" << fatImage << "'";
		tool(arguments.String(), &status);
		unlink(fatImage.String());
	}

	BString detachArgs;
	detachArgs << "detach '" << image << "'";
	tool(detachArgs.String(), &status);
	report("detach", status == 0 && disk_lines(disk).Length() == 0,
		"exit %d", status);

	system(command.String());
	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
