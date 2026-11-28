/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WIZARD_PLAN_H
#define WIZARD_PLAN_H


/*!	What the Installer's wizard (roadmap F-23, batch 14) is to do, and its
	Summary page in words and pictures. The window (WizardWindow.cpp) and
	the command line (Installer --wizard-summary, InstallerCLI.cpp) both
	make their summary here, so the text the harness checks is the text
	the page shows. The disk layouts are DestinationPlan's, the steps the
	shared disk engine's (RengaDisk::Plan).
*/


#include <DiskEngine.h>
#include <String.h>
#include <StringList.h>

#include <vector>

#include "DestinationPlan.h"
#include "InstallationInfo.h"


enum wizard_destination {
	WIZARD_ERASE_DISK,		// a whole disk: DESTINATION_ERASE_DISK
	WIZARD_ALONGSIDE,		// the largest free space: DESTINATION_FREE_SPACE
	WIZARD_EXISTING,		// an existing BFS volume (upgrade, replace,
							// erase, or an empty one)
	WIZARD_MANUAL			// DriveSetup first, then a volume as for
							// WIZARD_EXISTING
};


struct WizardChoice {
								WizardChoice();

			wizard_destination	destination;
			BString				diskPath;		// erase, alongside
			BString				volumeName;		// erase, alongside: the new
												// volume's name
			partition_id		target;			// existing, manual
			install_mode		mode;			// existing, manual: for a
												// volume that is not empty
			bool				lean;
			firmware_kind		firmware;		// running_firmware()
};


struct WizardSummary {
								WizardSummary();

			RengaDisk::DiskInfo	disk;			// the destination's disk
			RengaDisk::Plan		plan;			// erase, alongside
			bool				changesTable;	// the plan has steps
			bool				erasesDisk;
			BString				targetName;		// the volume the system
												// goes onto
			BStringList			changes;		// the Summary page's list
			BStringList			warnings;		// detected-system warnings
			BString				startNote;		// how the new system is
												// started, if not by
												// itself (Finish page)
			std::vector<DiskSegment> before;
			std::vector<DiskSegment> after;
};


/*!	The summary of \a choice: the planned changes, the disk before and
	after, and, for erase and alongside, the engine's plan (checked with
	the engine's safety rules; real disks allowed, since this is the
	installation the user asked for). Nothing is written. On failure \a why
	says what is wrong, in words for the Destination page.
*/
status_t make_wizard_summary(const WizardChoice& choice,
	WizardSummary& summary, BString& why);

//!	"erase", "alongside", "existing", "manual"; false for anything else.
bool wizard_destination_for_name(const char* name,
	wizard_destination& destination);

//!	The name of the volume the running system started from ("Renga").
BString boot_volume_name();


#endif	// WIZARD_PLAN_H
