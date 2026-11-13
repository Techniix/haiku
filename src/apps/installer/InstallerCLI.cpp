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
#include <Path.h>

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


static int
usage()
{
	fprintf(stderr,
		"usage: Installer --list-targets\n"
		"       Installer --unattended --target <mount point|volume name>\n"
		"           [--source <mount point>] [--mode upgrade|replace|erase]\n"
		"           [--confirm-erase <volume name>] [--rename-volume yes|no]\n"
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
	worker->StartInstall(sourceID, targetID);
	status_t result = reporter->Wait();

	if (worker->Lock())
		worker->Quit();
	if (reporter->Lock())
		reporter->Quit();
	sync();
	return result == B_OK ? 0 : 1;
}
