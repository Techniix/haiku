/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Boot menu preferences (Renga, roadmap F-22).

	The boot loader shows its menu at every start when the kernel settings
	file (~/config/settings/kernel/drivers/kernel) has "boot_menu_timeout
	<seconds>", counts down, and then starts the entry "boot_menu_default
	<name>" names: another operating system the menu found (by its name,
	"Windows Boot Manager" for example), or this system ("Continue booting")
	when it is absent. This application edits those two lines, and lists the
	other systems it finds on this computer, the same way the boot menu
	finds them (the names come from headers/private/shared/
	boot_other_systems.h, which the boot loader uses too):

	- UEFI: EFI system partitions (FAT partitions on fixed disks) are
	  searched for "/EFI/<directory>/<loader>.efi"; unmounted ones are
	  mounted read-only for the search and unmounted again.
	- BIOS: the boot records of primary MBR partitions, and the MBR of other
	  disks.

	The boot menu finds systems by itself at every start; the list here is
	only what can be chosen as the default. Removable media are not listed:
	the boot menu offers them while it is open.

	  BootMenu                         the window
	  BootMenu --list                  the systems found, one per line
	  BootMenu --show                  the current settings
	  BootMenu --set-timeout <seconds> 0 turns the menu off
	  BootMenu --set-default <name>    "renga" for this system

	The window applies every change at once, as Haiku's preferences do.
	The same messages the controls send (kMsgSetTimeout, kMsgSetDefault)
	are accepted by the application from other programs, which the GUI test
	uses.
*/


#include <Alert.h>
#include <Application.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Directory.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceTypes.h>
#include <DiskDeviceVisitor.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <ObjectList.h>
#include <Partition.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <Spinner.h>
#include <String.h>
#include <StringList.h>
#include <StringView.h>
#include <Window.h>

#include <driver_settings.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_volume.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <boot_other_systems.h>
#include <distro_name.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "BootMenu"


using namespace BootOtherSystems;


static const char* kSignature = "application/x-vnd.Renga-BootMenu";

static const uint32 kMsgShowMenu = 'shwm';
static const uint32 kMsgTimeout = 'tmot';
static const uint32 kMsgDefault = 'dflt';
// From other programs: "seconds" (int32, 0 = off), "name" (string).
static const uint32 kMsgSetTimeout = 'stmo';
static const uint32 kMsgSetDefault = 'sdfl';

static const int32 kDefaultTimeout = 5;
static const int32 kMaxTimeout = 600;
	// the boot loader's limit


// #pragma mark - settings


struct BootMenuSettings {
	int32	timeout;	// 0: no menu unless a key is held
	BString	defaultName;	// empty: this system
};


static status_t
settings_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	if (status == B_OK)
		status = path.Append("kernel/drivers/kernel");
	return status;
}


static BootMenuSettings
read_settings()
{
	BootMenuSettings settings = { 0, "" };
	BPath path;
	if (settings_path(path) != B_OK)
		return settings;

	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return settings;

	const char* timeout = get_driver_parameter(handle, "boot_menu_timeout",
		NULL, NULL);
	if (timeout != NULL)
		settings.timeout = strtol(timeout, NULL, 10);
	if (settings.timeout < 0)
		settings.timeout = 0;
	const char* name = get_driver_parameter(handle, "boot_menu_default",
		NULL, NULL);
	if (name != NULL && strcasecmp(name, "renga") != 0
		&& strcasecmp(name, "haiku") != 0)
		settings.defaultName = name;
	unload_driver_settings(handle);
	return settings;
}


static bool
is_setting_line(const BString& line, const char* key)
{
	BString trimmed(line);
	trimmed.Trim();
	return trimmed.StartsWith(key)
		&& (trimmed.Length() == (int32)strlen(key)
			|| trimmed[strlen(key)] == ' ' || trimmed[strlen(key)] == '\t');
}


/*!	Writes \a settings into the kernel settings file. Every other line of
	the file is kept as it is (it holds the other boot loader and kernel
	options); the two lines go to the end. A timeout of 0 removes both,
	which is the boot loader's default behaviour. The file is replaced
	through a temporary file, so it is never left half written.
*/
static status_t
write_settings(const BootMenuSettings& settings)
{
	BPath path;
	status_t status = settings_path(path);
	if (status != B_OK)
		return status;
	BPath directory;
	path.GetParent(&directory);
	create_directory(directory.Path(), 0755);

	BString content;
	BFile file(path.Path(), B_READ_ONLY);
	off_t size = 0;
	if (file.InitCheck() == B_OK && file.GetSize(&size) == B_OK && size > 0
		&& size < 1024 * 1024) {
		char* buffer = content.LockBuffer(size + 1);
		ssize_t bytesRead = file.Read(buffer, size);
		content.UnlockBuffer(bytesRead > 0 ? bytesRead : 0);
	}
	file.Unset();

	BStringList lines;
	content.Split("\n", false, lines);
	BString output;
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BString line = lines.StringAt(i);
		if (is_setting_line(line, "boot_menu_timeout")
			|| is_setting_line(line, "boot_menu_default"))
			continue;
		if (i == lines.CountStrings() - 1 && line.IsEmpty())
			break;
		output << line << "\n";
	}

	if (settings.timeout > 0) {
		if (!output.IsEmpty() && !output.EndsWith("\n\n"))
			output << "\n";
		output << "boot_menu_timeout " << settings.timeout << "\n";
		if (!settings.defaultName.IsEmpty()) {
			BString name(settings.defaultName);
			name.CharacterEscape("\"\\", '\\');
			output << "boot_menu_default \"" << name << "\"\n";
		}
	}

	BString temporary(path.Path());
	temporary << ".new";
	BFile out(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	if ((status = out.InitCheck()) != B_OK)
		return status;
	if (out.Write(output.String(), output.Length()) != output.Length())
		return B_IO_ERROR;
	out.Sync();
	out.Unset();

	BEntry entry(temporary.String());
	return entry.Rename(path.Path(), true);
}


// #pragma mark - finding systems


struct FoundSystem {
	BString	name;
	BString	where;
};


class SystemFinder : public BDiskDeviceVisitor {
public:
	SystemFinder(BObjectList<FoundSystem, true>& systems)
		:
		fSystems(systems),
		fBootDevice(-1)
	{
		BDiskDeviceRoster roster;
		BDiskDevice device;
		BPartition* partition;
		if (roster.FindPartitionByMountPoint("/boot", &device, &partition)
				== B_OK)
			fBootDevice = device.ID();
	}

	virtual bool Visit(BDiskDevice* device)
	{
		if (!device->IsRemovableMedia())
			_SearchBootRecords(device);
		return Visit(device, 0);
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		BDiskDevice* device = partition->Device();
		if (device == NULL || device->IsRemovableMedia()
			|| partition->ContentType() == NULL)
			return false;
		const char* type = partition->ContentType();
		if (strcmp(type, kPartitionTypeFAT12) != 0
			&& strcmp(type, kPartitionTypeFAT16) != 0
			&& strcmp(type, kPartitionTypeFAT32) != 0)
			return false;
		_SearchEFIVolume(partition);
		return false;
	}

private:
	void _Add(const char* name, const BString& where)
	{
		for (int32 i = 0; i < fSystems.CountItems(); i++) {
			if (compare_ignoring_case(fSystems.ItemAt(i)->name, name) == 0)
				return;
		}
		FoundSystem* system = new FoundSystem;
		system->name = name;
		system->where = where;
		fSystems.AddItem(system);
	}

	void _SearchEFIVolume(BPartition* partition)
	{
		BPath mountPoint;
		bool mountedHere = false;
		if (!partition->IsMounted()) {
			if (partition->Mount(NULL, B_MOUNT_READ_ONLY) != B_OK)
				return;
			mountedHere = true;
		}
		if (partition->GetMountPoint(&mountPoint) != B_OK) {
			if (mountedHere)
				partition->Unmount();
			return;
		}

		BPath efiPath(mountPoint.Path(), "EFI");
		BDirectory efi(efiPath.Path());
		int32 count = 0;
		BEntry entry;
		while (efi.InitCheck() == B_OK && efi.GetNextEntry(&entry) == B_OK) {
			char directoryName[B_FILE_NAME_LENGTH];
			if (!entry.IsDirectory() || entry.GetName(directoryName) != B_OK
				|| is_ignored_efi_directory(directoryName))
				continue;

			BPath directoryPath(efiPath.Path(), directoryName);
			bool windows = is_windows_directory(directoryName);
			if (windows)
				directoryPath.Append("Boot");
			BDirectory directory(directoryPath.Path());
			BString best;
			int bestRank = -1;
			BEntry file;
			while (directory.GetNextEntry(&file) == B_OK) {
				char fileName[B_FILE_NAME_LENGTH];
				if (file.IsDirectory() || file.GetName(fileName) != B_OK)
					continue;
				int rank = efi_loader_rank(fileName);
				if (rank < 0 || (windows && compare_ignoring_case(fileName,
						"bootmgfw.efi") != 0))
					continue;
				if (bestRank < 0 || rank < bestRank) {
					bestRank = rank;
					best = fileName;
				}
			}
			if (bestRank < 0)
				continue;

			char name[kMaxNameLength];
			efi_system_name(directoryName, name, sizeof(name));
			BString where;
			where.SetToFormat("UEFI, \\EFI\\%s%s\\%s", directoryName,
				windows ? "\\Boot" : "", best.String());
			_Add(name, where);
			count++;
		}

		BPath fallback(mountPoint.Path(), "EFI/BOOT/BOOTX64.EFI");
		BEntry fallbackEntry(fallback.Path());
		if (count == 0 && fallbackEntry.Exists()
			&& !_IsHaikuLoader(fallback.Path())) {
			_Add("EFI fallback loader", "UEFI, \\EFI\\BOOT\\BOOTX64.EFI");
		}

		if (mountedHere)
			partition->Unmount();
	}

	// Renga's and Haiku's own loader on the fallback path (an EFI system
	// partition only Haiku uses) is not another system.
	bool _IsHaikuLoader(const char* path)
	{
		BFile file(path, B_READ_ONLY);
		off_t size;
		if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
			|| size > 16 * 1024 * 1024)
			return false;
		char* buffer = (char*)malloc(size);
		if (buffer == NULL)
			return false;
		bool haiku = file.Read(buffer, size) == size
			&& (memmem(buffer, size, "boot loader!", 12) != NULL)
			&& (memmem(buffer, size, "Haiku revision", 14) != NULL);
		free(buffer);
		return haiku;
	}

	bool _Read(int fd, off_t sector, uint8* buffer)
	{
		return pread(fd, buffer, 512, sector * 512) == 512;
	}

	void _SearchBootRecords(BDiskDevice* device)
	{
		BPath path;
		if (device->GetPath(&path) != B_OK)
			return;
		int fd = open(path.Path(), O_RDONLY);
		if (fd < 0)
			return;

		uint8 mbr[512];
		uint8 record[512];
		if (!_Read(fd, 0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa) {
			close(fd);
			return;
		}
		bool gpt = false;
		for (int p = 0; p < 4; p++) {
			if (mbr[446 + 16 * p + 4] == kMBRTypeGPTProtective)
				gpt = true;
		}
		if (gpt) {
			close(fd);
			return;
		}

		for (int p = 0; p < 4; p++) {
			const uint8* entry = mbr + 446 + 16 * p;
			uint8 type = entry[4];
			uint32 start = entry[8] | (entry[9] << 8) | (entry[10] << 16)
				| ((uint32)entry[11] << 24);
			if (!is_chainloadable_partition_type(type) || start == 0
				|| !_Read(fd, start, record))
				continue;
			char name[kMaxNameLength];
			if (boot_record_name(record, type, (entry[0] & 0x80) != 0, name,
					sizeof(name))) {
				BString where;
				where.SetToFormat("BIOS, partition %d of %s", p + 1,
					path.Path());
				_Add(name, where);
			}
		}
		close(fd);
	}

	BObjectList<FoundSystem, true>&	fSystems;
	partition_id					fBootDevice;
};


static void
find_systems(BObjectList<FoundSystem, true>& systems)
{
	BDiskDeviceRoster roster;
	SystemFinder finder(systems);
	BDiskDevice device;
	BPartition* partition;
	roster.VisitEachPartition(&finder, &device, &partition);
}


// #pragma mark - window


class BootMenuWindow : public BWindow {
public:
	BootMenuWindow()
		:
		BWindow(BRect(100, 100, 560, 400), B_TRANSLATE_SYSTEM_NAME("Boot menu"),
			B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
				| B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE)
	{
		fShowMenu = new BCheckBox("show", B_TRANSLATE(
			"Show the boot menu every time the computer starts"),
			new BMessage(kMsgShowMenu));
		fTimeout = new BSpinner("timeout",
			B_TRANSLATE("Start the default entry after (seconds):"),
			new BMessage(kMsgTimeout));
		fTimeout->SetRange(1, kMaxTimeout);
		fDefaultMenu = new BPopUpMenu("default");
		fDefaultField = new BMenuField("default",
			B_TRANSLATE("Default entry:"), fDefaultMenu);

		BStringView* note = new BStringView("note", B_TRANSLATE(
			"Without this, hold Shift or Space while the computer starts."));
		BStringView* note2 = new BStringView("note2", B_TRANSLATE(
			"The menu finds other systems and removable media by itself."));
		fFound = new BStringView("found", "");

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fShowMenu)
			.AddGrid()
				.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
				.Add(fTimeout->CreateLabelLayoutItem(), 0, 0)
				.Add(fTimeout->CreateTextViewLayoutItem(), 1, 0)
				.Add(fDefaultField->CreateLabelLayoutItem(), 0, 1)
				.Add(fDefaultField->CreateMenuBarLayoutItem(), 1, 1)
			.End()
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fFound)
			.Add(note)
			.Add(note2)
		.End();

		fSettings = read_settings();
		_FillDefaultMenu();
		_UpdateControls();
		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgShowMenu:
				fSettings.timeout = fShowMenu->Value() == B_CONTROL_ON
					? (fTimeout->Value() > 0 ? fTimeout->Value()
						: kDefaultTimeout)
					: 0;
				_Apply();
				break;
			case kMsgTimeout:
				if (fShowMenu->Value() == B_CONTROL_ON) {
					fSettings.timeout = fTimeout->Value();
					_Apply();
				}
				break;
			case kMsgDefault:
				fSettings.defaultName = message->GetString("name", "");
				_Apply();
				break;
			case kMsgSetTimeout:
			{
				int32 seconds = message->GetInt32("seconds", 0);
				fSettings.timeout = seconds < 0 ? 0
					: seconds > kMaxTimeout ? kMaxTimeout : seconds;
				_Apply();
				break;
			}
			case kMsgSetDefault:
				fSettings.defaultName = message->GetString("name", "");
				if (fSettings.defaultName.ICompare("renga") == 0)
					fSettings.defaultName = "";
				_FillDefaultMenu();
				_Apply();
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void _FillDefaultMenu()
	{
		while (BMenuItem* item = fDefaultMenu->RemoveItem((int32)0))
			delete item;

		BMessage* message = new BMessage(kMsgDefault);
		message->AddString("name", "");
		fDefaultMenu->AddItem(new BMenuItem(B_TRANSLATE(
			DISTRO_NAME " (this system)"), message));

		if (fSystems.IsEmpty())
			find_systems(fSystems);
		bool listed = fSettings.defaultName.IsEmpty();
		for (int32 i = 0; i < fSystems.CountItems(); i++) {
			FoundSystem* system = fSystems.ItemAt(i);
			message = new BMessage(kMsgDefault);
			message->AddString("name", system->name);
			BString label;
			label.SetToFormat("%s (%s)", system->name.String(),
				system->where.String());
			fDefaultMenu->AddItem(new BMenuItem(label, message));
			if (system->name.ICompare(fSettings.defaultName) == 0)
				listed = true;
		}
		if (!listed) {
			// A default for a system not found now (another disk not
			// connected, say) is kept and shown.
			message = new BMessage(kMsgDefault);
			message->AddString("name", fSettings.defaultName);
			BString label;
			label.SetToFormat(B_TRANSLATE("%s (not found now)"),
				fSettings.defaultName.String());
			fDefaultMenu->AddItem(new BMenuItem(label, message));
		}

		BString found;
		if (fSystems.IsEmpty())
			found = B_TRANSLATE("No other operating systems found.");
		else {
			found.SetToFormat(B_TRANSLATE("Other operating systems found: %d"),
				(int)fSystems.CountItems());
		}
		fFound->SetText(found);
	}

	void _UpdateControls()
	{
		bool show = fSettings.timeout > 0;
		fShowMenu->SetValue(show ? B_CONTROL_ON : B_CONTROL_OFF);
		fTimeout->SetValue(show ? fSettings.timeout : kDefaultTimeout);
		fTimeout->SetEnabled(show);
		fDefaultField->SetEnabled(show);

		for (int32 i = 0; i < fDefaultMenu->CountItems(); i++) {
			BMenuItem* item = fDefaultMenu->ItemAt(i);
			BString name = item->Message()->GetString("name", "");
			if (name.ICompare(fSettings.defaultName) == 0) {
				item->SetMarked(true);
				break;
			}
		}
	}

	void _Apply()
	{
		status_t status = write_settings(fSettings);
		_UpdateControls();
		if (status != B_OK) {
			BString text;
			text.SetToFormat(B_TRANSLATE("The boot menu settings could not "
				"be saved: %s"), strerror(status));
			BAlert* alert = new BAlert(B_TRANSLATE("Boot menu"), text,
				B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
				B_STOP_ALERT);
			alert->Go(NULL);
		} else {
			printf("BootMenu: saved timeout %" B_PRId32 ", default \"%s\"\n",
				fSettings.timeout, fSettings.defaultName.String());
		}
	}

	BCheckBox*			fShowMenu;
	BSpinner*			fTimeout;
	BPopUpMenu*			fDefaultMenu;
	BMenuField*			fDefaultField;
	BStringView*		fFound;
	BootMenuSettings	fSettings;
	BObjectList<FoundSystem, true> fSystems;
};


class BootMenuApp : public BApplication {
public:
	BootMenuApp()
		:
		BApplication(kSignature),
		fWindow(NULL)
	{
	}

	virtual void ReadyToRun()
	{
		fWindow = new BootMenuWindow();
		fWindow->Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgSetTimeout:
			case kMsgSetDefault:
				if (fWindow != NULL)
					fWindow->PostMessage(message);
				break;
			default:
				BApplication::MessageReceived(message);
		}
	}

private:
	BWindow*	fWindow;
};


static int
usage()
{
	fprintf(stderr, "usage: BootMenu [--list | --show | --set-timeout "
		"<seconds> | --set-default <name>]\n");
	return 1;
}


int
main(int argc, char** argv)
{
	if (argc > 1) {
		BootMenuSettings settings = read_settings();
		if (strcmp(argv[1], "--list") == 0 && argc == 2) {
			BObjectList<FoundSystem, true> systems;
			find_systems(systems);
			for (int32 i = 0; i < systems.CountItems(); i++) {
				printf("%s\t%s\n", systems.ItemAt(i)->name.String(),
					systems.ItemAt(i)->where.String());
			}
			return 0;
		}
		if (strcmp(argv[1], "--show") == 0 && argc == 2) {
			printf("timeout %" B_PRId32 "\ndefault %s\n", settings.timeout,
				settings.defaultName.IsEmpty() ? "renga"
					: settings.defaultName.String());
			return 0;
		}
		if (strcmp(argv[1], "--set-timeout") == 0 && argc == 3) {
			int32 seconds = strtol(argv[2], NULL, 10);
			settings.timeout = seconds < 0 ? 0
				: seconds > kMaxTimeout ? kMaxTimeout : seconds;
		} else if (strcmp(argv[1], "--set-default") == 0 && argc == 3) {
			settings.defaultName = strcasecmp(argv[2], "renga") == 0
				? "" : argv[2];
		} else
			return usage();
		status_t status = write_settings(settings);
		if (status != B_OK) {
			fprintf(stderr, "BootMenu: %s\n", strerror(status));
			return 1;
		}
		return 0;
	}

	BootMenuApp app;
	app.Run();
	return 0;
}
