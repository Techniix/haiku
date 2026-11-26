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
	  disk_engine.restore           a saved table written back brings a
	                                deleted partition back
	  disk_engine.format            partition 0 formatted as BFS named Data
	  disk_engine.detach            the image is detached again

	Output format: see 3rdparty/ce/README.md.
*/


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
