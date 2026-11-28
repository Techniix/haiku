/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WizardPlan.h"

#include <Catalog.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Partition.h>
#include <Path.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <distro_name.h>

#include <string.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallerWizard"


WizardChoice::WizardChoice()
	:
	destination(WIZARD_ERASE_DISK),
	target(-1),
	mode(INSTALL_MODE_ASK),
	lean(false),
	firmware(FIRMWARE_UNKNOWN)
{
}


WizardSummary::WizardSummary()
	:
	plan(""),
	changesTable(false),
	erasesDisk(false)
{
}


bool
wizard_destination_for_name(const char* name,
	wizard_destination& destination)
{
	static const struct {
		const char*			name;
		wizard_destination	destination;
	} kNames[] = {
		{ "erase", WIZARD_ERASE_DISK },
		{ "alongside", WIZARD_ALONGSIDE },
		{ "existing", WIZARD_EXISTING },
		{ "manual", WIZARD_MANUAL },
	};
	for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
		if (strcmp(name, kNames[i].name) == 0) {
			destination = kNames[i].destination;
			return true;
		}
	}
	return false;
}


BString
boot_volume_name()
{
	BVolume volume;
	char name[B_FILE_NAME_LENGTH] = "";
	if (BVolumeRoster().GetBootVolume(&volume) == B_OK)
		volume.GetName(name);
	return name;
}


//!	The disk engine's step text with a capital letter, for the list.
static BString
capitalized(const BString& text)
{
	BString result(text);
	if (result.Length() > 0 && result[0] >= 'a' && result[0] <= 'z')
		result.SetByteAt(0, result[0] - 'a' + 'A');
	return result;
}


static void
add_warnings(const RengaDisk::DiskInfo& disk, WizardSummary& summary)
{
	for (const RengaDisk::PartitionInfo& partition : disk.partitions) {
		if (partition.warning.Length() == 0)
			continue;
		BString line;
		line.SetToFormat(B_TRANSLATE("Warning: \"%s\": %s"),
			partition.contentName.Length() > 0
				? partition.contentName.String() : partition.name.String(),
			partition.warning.String());
		summary.warnings.Add(line);
	}
}


static void
add_install_line(WizardSummary& summary, const WizardChoice& choice,
	const char* onto)
{
	BString line;
	line.SetToFormat(B_TRANSLATE("Install %s from \"%s\" onto %s"),
		DISTRO_NAME, boot_volume_name().String(), onto);
	summary.changes.Add(line);
	if (choice.lean) {
		summary.changes.Add(B_TRANSLATE("Leave out the development files "
			"(headers, compiler): a lean installation"));
	}
}


static status_t
summarize_new_volume(const WizardChoice& choice, WizardSummary& summary,
	BString& why)
{
	status_t status = RengaDisk::GetDisk(choice.diskPath.String(),
		summary.disk);
	if (status != B_OK) {
		why.SetToFormat(B_TRANSLATE("There is no disk %s."),
			choice.diskPath.String());
		return status;
	}
	const RengaDisk::DiskInfo& disk = summary.disk;
	destination_layout layout = choice.destination == WIZARD_ERASE_DISK
		? DESTINATION_ERASE_DISK : DESTINATION_FREE_SPACE;
	if (!valid_volume_name(choice.volumeName.String(), why))
		return B_BAD_VALUE;
	if (layout == DESTINATION_ERASE_DISK && choice.firmware != FIRMWARE_UEFI) {
		// The erase layout is GPT with an EFI system partition, which a
		// BIOS does not start: wiping the disk would leave a computer
		// that cannot start anything (review of PR #42).
		why.SetToFormat(B_TRANSLATE("%s Erasing a disk makes a GUID "
			"partition table with an EFI system partition, which a BIOS "
			"cannot start. Choose \"Set up partitions by hand\" instead "
			"(DriveSetup: an Intel partition map with an active Be File "
			"System partition), then that volume."),
			choice.firmware == FIRMWARE_BIOS
				? B_TRANSLATE("This computer was started by a BIOS.")
				: B_TRANSLATE("How this computer was started could not be "
					"told."));
		return B_NOT_SUPPORTED;
	}
	summary.plan = RengaDisk::Plan(disk.path.String());
	summary.plan.AllowRealDisks(true);
	// only ever applied to this very disk: another one plugged into the
	// same port has another device ID, and the engine refuses it
	summary.plan.ExpectDevice(disk.id);
	status = make_destination_plan(disk, layout,
		choice.volumeName.String(), summary.plan, why);
	if (status == B_OK)
		status = summary.plan.Check(why);
	if (status != B_OK) {
		why = capitalized(why) << ".";
		return status;
	}
	summary.changesTable = true;
	summary.erasesDisk = layout == DESTINATION_ERASE_DISK;
	summary.targetName = choice.volumeName;
	disk_segments_now(disk, summary.before);
	disk_segments_planned(disk, layout, choice.volumeName.String(),
		summary.after);

	BString line;
	if (summary.erasesDisk) {
		line.SetToFormat(B_TRANSLATE("Erase the whole disk %s (%s): "
			"everything on it is lost"), disk.path.String(),
			describe_disk(disk).String());
		summary.changes.Add(line);
		// every volume, logical partitions in an extended one too
		BStringList lost;
		disk_volume_descriptions(disk, lost);
		for (int32 i = 0; i < lost.CountStrings(); i++) {
			line.SetToFormat(B_TRANSLATE("  Lost: %s"),
				lost.StringAt(i).String());
			summary.changes.Add(line);
		}
	} else {
		line.SetToFormat(B_TRANSLATE("Keep every partition on the disk %s "
			"(%s); use its largest free space"), disk.path.String(),
			describe_disk(disk).String());
		summary.changes.Add(line);
	}
	for (int32 i = 0; i < summary.plan.CountOperations(); i++) {
		line.SetToFormat(B_TRANSLATE("Step %d: %s"), (int)i + 1,
			capitalized(summary.plan.Describe(i)).String());
		summary.changes.Add(line);
	}
	BString onto;
	onto.SetToFormat(B_TRANSLATE("the new volume \"%s\""),
		choice.volumeName.String());
	add_install_line(summary, choice, onto.String());
	if (summary.erasesDisk) {
		line.SetToFormat(B_TRANSLATE("Put the boot loader into the new EFI "
			"system partition (EFI/BOOT) and write the boot sector of "
			"\"%s\""), choice.volumeName.String());
	} else {
		line.SetToFormat(B_TRANSLATE("Write the boot sector of \"%s\"; the "
			"disk's EFI system partitions are not changed"),
			choice.volumeName.String());
		if (choice.firmware == FIRMWARE_UEFI) {
			if (!disk_has_haiku_efi_loader(disk)) {
				summary.startNote.SetToFormat(B_TRANSLATE("The new system "
					"will not start until %s's EFI loader is installed: no "
					"EFI system partition on this disk has it. Install it "
					"with Tools > Install EFI loader in the classic "
					"window."), DISTRO_NAME);
			}
		} else {
			summary.startNote = B_TRANSLATE("This computer was started by a "
				"BIOS: start the new system from the boot menu, which Boot "
				"Manager sets up (Tools > Set up boot menu in the classic "
				"window).");
		}
	}
	summary.changes.Add(line);
	if (summary.startNote.Length() > 0)
		summary.changes.Add(summary.startNote);
	summary.changes.Add(B_TRANSLATE("The partition table is saved first, on "
		"the installation medium (~/config/settings/Renga/disk-backups, "
		"rengadisk restore), and copied onto the new volume afterwards; "
		"overwritten partition contents cannot be restored"));
	add_warnings(disk, summary);
	return B_OK;
}


static status_t
summarize_existing(const WizardChoice& choice, WizardSummary& summary,
	BString& why)
{
	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition;
	if (choice.target < 0 || roster.GetPartitionWithID(choice.target,
			&device, &partition) != B_OK) {
		why = B_TRANSLATE("Choose the volume to install onto.");
		return B_BAD_VALUE;
	}
	BPath devicePath;
	device.GetPath(&devicePath);
	RengaDisk::GetDisk(devicePath.Path(), summary.disk);
	if (summary.disk.isBootDisk && partition->IsMounted()) {
		BPath mountPoint;
		partition->GetMountPoint(&mountPoint);
		if (strcmp(mountPoint.Path(), "/boot") == 0) {
			why = B_TRANSLATE("This is the volume the running system started "
				"from.");
			return B_NOT_ALLOWED;
		}
	}
	if (partition->IsReadOnly() || partition->ContentType() == NULL
		|| strcmp(partition->ContentType(), "Be File System") != 0) {
		why = B_TRANSLATE("The system can only be installed onto a writable "
			"Be File System volume.");
		return B_BAD_VALUE;
	}
	summary.targetName = partition->ContentName();
	InstallationInfo existing;
	inspect_partition(partition, existing);
	if (choice.mode == INSTALL_MODE_UPGRADE && !existing.found) {
		why = B_TRANSLATE("There is no installation to upgrade on this "
			"volume.");
		return B_BAD_VALUE;
	}
	disk_segments_now(summary.disk, summary.before, choice.target);
	summary.after = summary.before;

	BString line;
	BString name = summary.targetName;
	switch (choice.mode) {
		case INSTALL_MODE_UPGRADE:
			line.SetToFormat(B_TRANSLATE("Upgrade the installation on \"%s\" "
				"(%s): files, settings and installed packages are kept; the "
				"old system stays bootable as a package state"), name.String(),
				existing.Description().String());
			break;
		case INSTALL_MODE_REPLACE:
			line.SetToFormat(B_TRANSLATE("Replace the system on \"%s\": the "
				"system and the installed packages are removed, files and "
				"settings are kept"), name.String());
			break;
		case INSTALL_MODE_ERASE:
			line.SetToFormat(B_TRANSLATE("Erase the volume \"%s\": everything "
				"on it is lost (asked again, naming the volume)"),
				name.String());
			break;
		default:
			line.SetToFormat(B_TRANSLATE("Install onto \"%s\"; if it is not "
				"empty, you are asked how"), name.String());
			break;
	}
	summary.changes.Add(line);
	summary.changes.Add(B_TRANSLATE("The partition table is not changed"));
	BString onto;
	onto.SetToFormat("\"%s\"", name.String());
	add_install_line(summary, choice, onto.String());
	if (choice.mode == INSTALL_MODE_ASK) {
		line.SetToFormat(B_TRANSLATE("Write the boot sector of \"%s\""),
			name.String());
	} else {
		line.SetToFormat(B_TRANSLATE("Write the boot sector of \"%s\"; "
			"Haiku's or %s's EFI loaders on the disk's EFI system partitions "
			"are replaced (the old ones are kept as *_old.EFI)"),
			name.String(), DISTRO_NAME);
	}
	summary.changes.Add(line);
	add_warnings(summary.disk, summary);
	return B_OK;
}


status_t
make_wizard_summary(const WizardChoice& choice, WizardSummary& summary,
	BString& why)
{
	summary = WizardSummary();
	why = "";
	switch (choice.destination) {
		case WIZARD_ERASE_DISK:
		case WIZARD_ALONGSIDE:
			if (choice.diskPath.Length() == 0) {
				why = B_TRANSLATE("Choose a disk.");
				return B_BAD_VALUE;
			}
			return summarize_new_volume(choice, summary, why);
		case WIZARD_EXISTING:
		case WIZARD_MANUAL:
			return summarize_existing(choice, summary, why);
	}
	return B_BAD_VALUE;
}
