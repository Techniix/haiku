/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: DriveSetup's pending changes (batch 13, roadmap F-24 GUI,
	first part), on a disk image only. Through DriveSetup's harness hook
	(MSG_TEST_QUEUE 'Tdsq', harness images only), which queues and applies
	exactly as its menus do.

	  drivesetup_queue.queued_not_written  a new GPT table and two
	                     partitions are queued (three pending changes with
	                     the engine's preview), and the image is unchanged
	                     (screenshot drivesetup_pending)
	  drivesetup_queue.applied   one Apply writes them: GPT with two
	                     partitions, the engine's log, nothing pending
	  drivesetup_queue.format    a queued format of partition 0 as BFS named
	                     "Data", applied
	  drivesetup_queue.refused   changes queued for the boot disk are
	                     refused by the engine's safety rules at Apply and
	                     nothing is written
	  drivesetup_queue.switch    "Queue changes, apply together" off hides
	                     the pending changes (DriveSetup writes at once, as
	                     before), on again shows them

	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <FindDirectory.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <DiskEngine.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "drivesetup_queue"

static const char* kDriveSetupSignature = "application/x-vnd.Haiku-DriveSetup";
static const uint32 kTestQueue = 'Tdsq';
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


static BString
one_line(BString text)
{
	text.ReplaceAll("\n", " / ");
	if (text.Length() > 300)
		text.Truncate(300);
	return text;
}


static status_t
ask(BMessage& request, BMessage& reply)
{
	status_t status = BMessenger(kDriveSetupSignature).SendMessage(&request,
		&reply, 5000000, 60000000);
	if (status == B_OK && reply.what != B_REPLY)
		status = B_ERROR;
	return status;
}


static BMessage
queue(const char* disk, RengaDisk::OperationKind kind, int32 index,
	const char* system, off_t size, const char* name)
{
	BMessage request(kTestQueue);
	request.AddString("disk", disk);
	request.AddString("action", "queue");
	request.AddInt32("kind", kind);
	request.AddInt32("index", index);
	request.AddString("system", system);
	request.AddInt64("size", size);
	request.AddString("name", name);
	BMessage reply;
	ask(request, reply);
	return reply;
}


static BMessage
action(const char* disk, const char* what)
{
	BMessage request(kTestQueue);
	if (disk != NULL)
		request.AddString("disk", disk);
	request.AddString("action", what);
	BMessage reply;
	ask(request, reply);
	return reply;
}


static BString
describe(const char* path)
{
	RengaDisk::DiskInfo disk;
	BString text;
	if (RengaDisk::GetDisk(path, disk) != B_OK)
		return "(not found)";
	text << (disk.partitioning.Length() > 0 ? disk.partitioning.String()
		: "no table") << ":";
	for (size_t i = 0; i < disk.partitions.size(); i++) {
		const RengaDisk::PartitionInfo& partition = disk.partitions[i];
		text << " [" << partition.index << " " << partition.name << " "
			<< RengaDisk::SizeString(partition.size) << " "
			<< partition.content << " \"" << partition.contentName << "\"]";
	}
	return text;
}


int
main()
{
	BApplication app("application/x-vnd.Renga-drivesetup-queue-test");
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	BPath home;
	find_directory(B_USER_DIRECTORY, &home);
	BString image = home.Path();
	image << "/drivesetup-queue-test.img";
	unlink(image.String());
	int fd = open(image.String(), O_CREAT | O_RDWR | O_TRUNC, 0644);
	bool created = fd >= 0 && ftruncate(fd, 64 * 1024 * 1024) == 0;
	if (fd >= 0)
		close(fd);
	BString disk;
	status_t attached = created
		? RengaDisk::AttachImage(image.String(), disk) : B_ERROR;

	bool wasRunning = be_roster->IsRunning(kDriveSetupSignature);
	be_roster->Launch(kDriveSetupSignature);
	for (int32 i = 0; i < 80 && !be_roster->IsRunning(kDriveSetupSignature);
			i++) {
		snooze(250000);
	}
	snooze(3000000);

	if (attached != B_OK) {
		report("queued_not_written", false, "image %d, attach: %s",
			(int)created, strerror(attached));
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	BString before = describe(disk.String());
	BMessage r1 = queue(disk.String(), RengaDisk::kInitializePartitioning, -1,
		"gpt", 0, "");
	BMessage r2 = queue(disk.String(), RengaDisk::kCreatePartition, -1, "",
		16 * 1024 * 1024, "one");
	BMessage r3 = queue(disk.String(), RengaDisk::kCreatePartition, -1, "", 0,
		"two");
	BString queued = describe(disk.String());
	printf("CE-HOST screenshot drivesetup_pending\n");
	fflush(stdout);
	snooze(800000);
	BString preview = r3.GetString("preview", "");
	report("queued_not_written", r1.GetBool("found", false)
			&& r3.GetInt32("pending", 0) == 3 && preview.Length() > 0
			&& queued == before && r3.GetBool("pending_shown", false),
		"disk found %d, pending %d, shown %d, preview [%s]; disk before %s, "
		"after queueing %s", (int)r1.GetBool("found", false),
		(int)r3.GetInt32("pending", 0), (int)r3.GetBool("pending_shown",
			false), one_line(preview).String(), before.String(),
		queued.String());

	BMessage applied = action(disk.String(), "apply");
	snooze(1000000);
	BString after = describe(disk.String());
	report("applied", applied.GetInt32("status", -1) == B_OK
			&& applied.GetInt32("pending", -1) == 0
			&& after.FindFirst("GUID") >= 0 && after.FindFirst("one") >= 0
			&& after.FindFirst("two") >= 0,
		"status %d, pending %d, log [%s]; disk %s",
		(int)applied.GetInt32("status", -1),
		(int)applied.GetInt32("pending", -1),
		one_line(applied.GetString("log", "")).String(), after.String());

	queue(disk.String(), RengaDisk::kFormat, 0, "bfs", 0, "Data");
	BMessage formatted = action(disk.String(), "apply");
	snooze(1000000);
	BString afterFormat = describe(disk.String());
	report("format", formatted.GetInt32("status", -1) == B_OK
			&& afterFormat.FindFirst("\"Data\"") >= 0,
		"status %d, log [%s]; disk %s",
		(int)formatted.GetInt32("status", -1),
		one_line(formatted.GetString("log", "")).String(),
		afterFormat.String());

	// the boot disk: queued, refused at Apply
	std::vector<RengaDisk::DiskInfo> disks;
	RengaDisk::ListDisks(disks);
	BString bootDisk;
	for (size_t i = 0; i < disks.size(); i++) {
		if (disks[i].isBootDisk)
			bootDisk = disks[i].path;
	}
	BString bootBefore = describe(bootDisk.String());
	queue(bootDisk.String(), RengaDisk::kSetName, 0, "", 0, "renamed");
	BMessage refused = action(bootDisk.String(), "apply");
	BString bootAfter = describe(bootDisk.String());
	BString why = refused.GetString("refused", "");
	action(NULL, "discard");
	report("refused", bootDisk.Length() > 0 && why.Length() > 0
			&& bootBefore == bootAfter,
		"boot disk %s: refused [%s], unchanged %d", bootDisk.String(),
		why.String(), (int)(bootBefore == bootAfter));

	action(NULL, "toggle");
	snooze(500000);
	BMessage off = action(NULL, "state");
	action(NULL, "toggle");
	snooze(500000);
	BMessage on = action(NULL, "state");
	report("switch", !off.GetBool("queue_mode", true)
			&& !off.GetBool("pending_shown", true)
			&& on.GetBool("queue_mode", false)
			&& on.GetBool("pending_shown", false),
		"off: queue %d shown %d; on: queue %d shown %d",
		(int)off.GetBool("queue_mode", true),
		(int)off.GetBool("pending_shown", true),
		(int)on.GetBool("queue_mode", false),
		(int)on.GetBool("pending_shown", false));

	if (!wasRunning)
		BMessenger(kDriveSetupSignature).SendMessage(B_QUIT_REQUESTED);
	snooze(1000000);
	RengaDisk::DetachImage(image.String());
	unlink(image.String());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
