/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The Installer's command line (Renga, roadmap F-23): the same
	installation code as the window (WorkerThread), without a window, for
	scripted installs and the test harness.

	  Installer --list-targets
	      the destinations the window offers, with the installation each
	      has: "<name>\t<device>\t<valid|invalid>\t<installation>"
	  Installer --unattended --target <mount point or volume name>
	      [--source <mount point>] [--mode upgrade|replace|erase]
	      [--confirm-erase <volume name>] [--rename-volume yes|no]
	      [--lean yes|no]
	      installs; a destination that is not empty needs --mode, and
	      "erase" needs the volume's name again in --confirm-erase.
	      Nothing asks: whatever the window would ask about (the running
	      system's volume as the destination, too little space) is refused.
	      Progress goes to standard output; the exit status is 0 on
	      success. The log is also written to the destination,
	      system/var/log/installer.log.

	No BApplication is created, so this works without a desktop.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceTypes.h>
#include <DiskDeviceVisitor.h>
#include <Looper.h>
#include <Messenger.h>
#include <Partition.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>

#include "DestinationPlan.h"

#include "InstallationInfo.h"
#include "InstallerDefs.h"
#include "WorkerThread.h"


extern const char* gPreselectedTarget;
extern bool gBeginAtOnce;


namespace {


class Reporter : public BLooper {
public:
	Reporter()
		:
		BLooper("installer reporter"),
		fDone(create_sem(0, "installer done")),
		fResult(B_ERROR),
		fLastPercent(-10)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_STATUS_MESSAGE:
			{
				float progress;
				const char* status;
				if (message->FindFloat("progress", &progress) == B_OK) {
					if (progress - fLastPercent >= 10 || progress >= 100) {
						fLastPercent = progress;
						printf("Installer: copying, %.0f%%\n", progress);
						fflush(stdout);
					}
				} else if (message->FindString("status", &status) == B_OK) {
					printf("Installer: %s\n", status);
					fflush(stdout);
				}
				break;
			}
			case MSG_INSTALL_FINISHED:
				fResult = B_OK;
				release_sem(fDone);
				break;
			case MSG_RESET:
			{
				// Always a failure: some refusals carry B_OK as their
				// "error" (installing a volume onto itself, for one).
				status_t error = message->GetInt32("error", B_CANCELED);
				fprintf(stderr, "Installer: not completed: %s\n",
					error != B_OK ? strerror(error) : "refused");
				fResult = error != B_OK ? error : B_NOT_ALLOWED;
				release_sem(fDone);
				break;
			}
			default:
				BLooper::MessageReceived(message);
		}
	}

	status_t Wait()
	{
		while (acquire_sem(fDone) == B_INTERRUPTED)
			;
		return fResult;
	}

private:
	sem_id		fDone;
	status_t	fResult;
	float		fLastPercent;
};


class PartitionFinder : public BDiskDeviceVisitor {
public:
	PartitionFinder(const char* name)
		:
		fName(name),
		fID(-1),
		fCount(0)
	{
	}

	virtual bool Visit(BDiskDevice* device)
	{
		return Visit(device, 0);
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		if (partition->ContentName() == fName) {
			fID = partition->ID();
			fCount++;
		}
		return false;
	}

	BString			fName;
	partition_id	fID;
	int32			fCount;
};


class TargetLister : public BDiskDeviceVisitor {
public:
	virtual bool Visit(BDiskDevice* device)
	{
		if (device->IsReadOnlyMedia())
			return false;
		return Visit(device, 0);
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		if (partition->CountChildren() > 0
			|| partition->ContentSize() < 20 * 1024 * 1024)
			return false;
		BPath path;
		partition->GetPath(&path);
		bool valid = !partition->IsReadOnly()
			&& partition->ContentType() != NULL
			&& strcmp(partition->ContentType(), kPartitionTypeBFS) == 0;
		InstallationInfo info;
		if (valid)
			inspect_partition(partition, info);
		printf("%s\t%s\t%s\t%s\n", partition->ContentName().String(),
			path.Path(), valid ? "valid" : "invalid",
			info.found ? info.Description().String() : "-");
		return false;
	}
};


static partition_id
find_partition(BDiskDeviceRoster& roster, const char* spec)
{
	BDiskDevice device;
	BPartition* partition;
	if (spec[0] == '/') {
		if (roster.FindPartitionByMountPoint(spec, &device, &partition)
				== B_OK)
			return partition->ID();
		fprintf(stderr, "Installer: nothing is mounted at %s\n", spec);
		return -1;
	}
	PartitionFinder finder(spec);
	roster.VisitEachPartition(&finder, &device, &partition);
	if (finder.fCount != 1) {
		fprintf(stderr, "Installer: %" B_PRId32 " volumes are named \"%s\"\n",
			finder.fCount, spec);
		return -1;
	}
	return finder.fID;
}


static int usage();


/*!	Renga (batch 13, F-23): "--plan-disk <raw device> --layout erase|free
	[--name <volume name>] [--apply]": the wizard's Destination step on the
	command line. Prints the shared disk engine's preview of the layout;
	with --apply it checks and applies it (the engine's rules: never the
	boot disk, nothing mounted, disk images only) and prints the new
	volume's partition as "volume: <name>", ready for --unattended
	--target <name>. Without --name the volume is "Renga", or "Renga 2"
	and so on when a volume of that name exists (the installation medium's
	own); a --name another volume has is refused.
*/
static int
plan_disk(int argc, char** argv)
{
	const char* disk = NULL;
	const char* layoutName = NULL;
	const char* name = NULL;
	bool apply = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--plan-disk") == 0 && i + 1 < argc)
			disk = argv[++i];
		else if (strcmp(argv[i], "--layout") == 0 && i + 1 < argc)
			layoutName = argv[++i];
		else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
			name = argv[++i];
		else if (strcmp(argv[i], "--apply") == 0)
			apply = true;
		else
			return usage();
	}
	destination_layout layout;
	if (disk == NULL || layoutName == NULL
		|| !destination_layout_for_name(layoutName, layout)) {
		return usage();
	}
	RengaDisk::DiskInfo info;
	if (RengaDisk::GetDisk(disk, info) != B_OK) {
		fprintf(stderr, "Installer: no disk %s\n", disk);
		return 1;
	}
	// the new volume's name must be unique, or --target cannot find it
	BString volumeName;
	if (name == NULL)
		volumeName = unique_volume_name("Renga");
	else if (volume_name_exists(name)) {
		fprintf(stderr, "Installer: a volume named \"%s\" exists already; "
			"choose another --name\n", name);
		return 1;
	} else
		volumeName = name;
	RengaDisk::Plan plan(info.path.String());
	BString why;
	if (make_destination_plan(info, layout, volumeName.String(), plan, why)
			!= B_OK) {
		fprintf(stderr, "Installer: %s\n", why.String());
		return 1;
	}
	printf("%s\n", plan.Preview().String());
	if (!apply)
		return 0;
	if (plan.Check(why) != B_OK) {
		fprintf(stderr, "Installer: refused: %s\n", why.String());
		return 1;
	}
	BPath backups;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &backups,
		true);
	if (status == B_OK)
		status = backups.Append("Renga/disk-backups");
	if (status == B_OK)
		status = create_directory(backups.Path(), 0700);
	if (status != B_OK) {
		fprintf(stderr, "Installer: no backup folder: %s\n",
			strerror(status));
		return 1;
	}
	BString log;
	status = plan.Apply(backups.Path(), log);
	printf("%s\n", log.String());
	if (status != B_OK) {
		fprintf(stderr, "Installer: applying failed: %s\n",
			strerror(status));
		return 1;
	}
	printf("volume: %s\n", volumeName.String());
	return 0;
}


static int
usage()
{
	fprintf(stderr,
		"usage: Installer --list-targets\n"
		"       Installer --unattended --target <mount point|volume name>\n"
		"           [--source <mount point>] [--mode upgrade|replace|erase]\n"
		"           [--confirm-erase <volume name>] [--rename-volume yes|no]\n"
		"           [--lean yes|no]\n"
		"       Installer --plan-disk <raw device> --layout erase|free\n"
		"           [--name <volume name>] [--apply]\n"
		"       Installer [--target <volume name> [--begin]]   (the window)\n");
	return 2;
}


}	// namespace


/*!	Runs the command line if \a argv asks for it; returns its exit status,
	or -1 if the window is to be shown.
*/
int
installer_command_line(int argc, char** argv)
{
	if (argc < 2)
		return -1;

	if (strcmp(argv[1], "--list-targets") == 0) {
		BDiskDeviceRoster roster;
		TargetLister lister;
		BDiskDevice device;
		BPartition* partition;
		roster.VisitEachPartition(&lister, &device, &partition);
		return 0;
	}

	if (strcmp(argv[1], "--plan-disk") == 0)
		return plan_disk(argc, argv);

	if (strcmp(argv[1], "--unattended") != 0) {
		if (strcmp(argv[1], "--target") == 0 && (argc == 3
				|| (argc == 4 && strcmp(argv[3], "--begin") == 0))) {
			gPreselectedTarget = argv[2];
			gBeginAtOnce = argc == 4;
			return -1;
		}
		return usage();
	}

	const char* source = "/boot";
	const char* target = NULL;
	const char* modeName = NULL;
	const char* confirmation = NULL;
	int32 rename = -1;
	bool lean = false;
	for (int i = 2; i < argc; i++) {
		if (i + 1 >= argc)
			return usage();
		if (strcmp(argv[i], "--source") == 0)
			source = argv[++i];
		else if (strcmp(argv[i], "--target") == 0)
			target = argv[++i];
		else if (strcmp(argv[i], "--mode") == 0)
			modeName = argv[++i];
		else if (strcmp(argv[i], "--confirm-erase") == 0)
			confirmation = argv[++i];
		else if (strcmp(argv[i], "--rename-volume") == 0)
			rename = strcmp(argv[++i], "no") == 0 ? 0 : 1;
		else if (strcmp(argv[i], "--lean") == 0)
			lean = strcmp(argv[++i], "no") != 0;
		else
			return usage();
	}
	install_mode mode = modeName != NULL
		? install_mode_for_name(modeName) : INSTALL_MODE_ASK;
	if (target == NULL || (modeName != NULL && mode == INSTALL_MODE_ASK))
		return usage();

	BDiskDeviceRoster roster;
	partition_id sourceID = find_partition(roster, source);
	partition_id targetID = find_partition(roster, target);
	if (sourceID < 0 || targetID < 0)
		return 1;

	printf("Installer: unattended %s from %s onto %s\n",
		install_mode_name(mode), source, target);
	fflush(stdout);

	Reporter* reporter = new Reporter();
	reporter->Run();
	WorkerThread* worker = new WorkerThread(BMessenger(reporter));
	worker->SetUnattended(true);
	worker->SetInstallMode(mode, confirmation);
	worker->SetRenameVolume(rename);
	worker->SetLeanInstall(lean);
	worker->StartInstall(sourceID, targetID);
	status_t result = reporter->Wait();

	if (worker->Lock())
		worker->Quit();
	if (reporter->Lock())
		reporter->Quit();
	sync();
	return result == B_OK ? 0 : 1;
}
