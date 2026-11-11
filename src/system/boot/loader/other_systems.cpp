/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Other operating systems in the boot menu (Renga, roadmap F-22, first
	part). See headers/private/kernel/boot/other_systems.h for the split
	between this file and the platforms.

	Menu layout (main menu):

		Select boot volume/state
		...
		-------------------------------
		Windows Boot Manager          1   systems on fixed disks
		Ubuntu                        2
		Boot other media: USB drive   3   removable media, appear when found
		(Looking for other media...)      while the search is not done
		-------------------------------
		Reboot
		Continue booting

	Every entry is logged on the serial console ("boot menu: other system
	<n>: ..."), so that a test can tell what was found and press the right
	shortcut key.
*/


#include <boot/other_systems.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boot/menu.h>
#include <boot/platform.h>
#include <boot/stdio.h>
#include <KernelExport.h>
#include <util/DoublyLinkedList.h>


using BootOtherSystems::compare_ignoring_case;


typedef DoublyLinkedList<OtherSystem> OtherSystemList;


static OtherSystemList sSystems;
static Menu* sMenu = NULL;
static MenuItem* sInsertBefore = NULL;
	// the separator in front of "Reboot"
static MenuItem* sSearchingItem = NULL;
static MenuItem* sDefaultItem = NULL;
static const char* sDefaultName = NULL;
static int32 sShortcutCount = 0;
static bool sMenuChanged = false;
static bool sSearched = false;
	// platform_find_other_systems() ran (once per boot)


// #pragma mark - weak platform defaults


extern "C" void __attribute__((weak))
platform_find_other_systems(other_system_found_hook found)
{
}


extern "C" bool __attribute__((weak))
platform_probe_other_media(other_system_found_hook found)
{
	return false;
}


extern "C" status_t __attribute__((weak))
platform_start_other_system(OtherSystem* system)
{
	return B_UNSUPPORTED;
}


// #pragma mark -


/*!	Returns the number of \a system among the systems of the same name, in
	the order they were found (1 for the first).
*/
static int
number_among_same_name(OtherSystem* system)
{
	int count = 0;
	for (OtherSystemList::Iterator it = sSystems.GetIterator();
			OtherSystem* other = it.Next();) {
		if (compare_ignoring_case(other->name, system->name) == 0)
			count++;
		if (other == system)
			break;
	}
	return count;
}


static bool
start_other_system(Menu* menu, MenuItem* item)
{
	OtherSystem* system = (OtherSystem*)item->Data();
	if (system == NULL)
		return true;

	dprintf("boot menu: starting \"%s\" (%s)\n", system->name,
		system->location);
	status_t status = platform_start_other_system(system);

	// Only here if it could not be started or (an EFI application) it
	// returned. The menu stays; the help text says what happened.
	dprintf("boot menu: \"%s\" returned: %s\n", system->name,
		strerror(status));

	char* text = (char*)malloc(256);
	if (text != NULL) {
		if (status == B_OK) {
			snprintf(text, 256, "%s returned to the boot menu.",
				system->name);
		} else {
			snprintf(text, 256, "%s could not be started: %s.",
				system->name, strerror(status));
		}
		// The previous help text is leaked on purpose: items do not own
		// it, and this happens at most a few times per boot.
		item->SetHelpText(text);
	}
	return true;
}


static void
add_system_item(OtherSystem* system)
{
	if (sMenu == NULL)
		return;

	char label[BootOtherSystems::kMaxNameLength + 64];
	if (system->kind == OTHER_SYSTEM_REMOVABLE)
		snprintf(label, sizeof(label), "Boot other media: %s", system->name);
	else
		strlcpy(label, system->name, sizeof(label));

	// Two systems of the same name (two Windows installations on two
	// disks) are told apart by number; the setting picks the first.
	int sameName = number_among_same_name(system);
	if (sameName > 1) {
		size_t length = strlen(label);
		snprintf(label + length, sizeof(label) - length, " (%d)", sameName);
	}

	MenuItem* item = new(std::nothrow) MenuItem(label);
	if (item == NULL)
		return;

	char shortcut = 0;
	if (sShortcutCount < 9)
		shortcut = '1' + sShortcutCount++;

	char* help = (char*)malloc(256);
	if (help != NULL) {
		if (shortcut != 0) {
			snprintf(help, 256, "Starts %s from %s. Shortcut: %c.",
				system->name, system->location, shortcut);
		} else
			snprintf(help, 256, "Starts %s from %s.", system->name,
				system->location);
		item->SetHelpText(help);
	}

	item->SetData(system);
	item->SetTarget(start_other_system);
	item->SetKeepsMenuOpen(true);
	if (shortcut != 0)
		item->SetShortcut(shortcut);

	sMenu->InsertItem(item, sSearchingItem != NULL
		? sSearchingItem : sInsertBefore);
	sMenuChanged = true;

	dprintf("boot menu: %s %c: \"%s\" (%s)\n",
		system->kind == OTHER_SYSTEM_REMOVABLE
			? "found other media" : "other system",
		shortcut != 0 ? shortcut : '-', label, system->location);

	if (sDefaultItem == NULL && sDefaultName != NULL
		&& system->kind == OTHER_SYSTEM_INSTALLED
		&& compare_ignoring_case(system->name, sDefaultName) == 0) {
		sDefaultItem = item;
	}
}


static void
system_found(OtherSystem* system)
{
	sSystems.Add(system);
	add_system_item(system);
}


static bool
idle_hook(Menu* menu)
{
	sMenuChanged = false;

	bigtime_t started = system_time();
	bool searching = platform_probe_other_media(system_found);
	bigtime_t took = system_time() - started;
	if (took >= 20000) {
		// Keys wait while a step runs; the harness checks that none is
		// long (other_media.responsive).
		dprintf("boot menu: probe step took %" B_PRId64 " ms\n",
			took / 1000);
	}
	if (!searching && sSearchingItem != NULL) {
		// Every device was looked at once; empty drives are looked at
		// again in the background, but the note goes.
		dprintf("boot menu: search for other media done\n");
		menu->RemoveItem(sSearchingItem);
		delete sSearchingItem;
		sSearchingItem = NULL;
		sMenuChanged = true;
	}

	return sMenuChanged;
}


/*!	Adds the other operating systems to the main menu \a menu, in front of
	\a before (the separator above "Reboot"), and starts the background
	search for removable media. If \a defaultName names one of the systems
	found on a fixed disk, \a _defaultItem is set to its item.
*/
void
other_systems_add_to_menu(Menu* menu, MenuItem* before,
	const char* defaultName, MenuItem** _defaultItem)
{
	sMenu = menu;
	sInsertBefore = before;
	sDefaultName = defaultName;
	sDefaultItem = NULL;

	// A separator between the boot loader's own entries and the systems;
	// it stays even if nothing is found, which keeps the layout stable.
	MenuItem* separator = new(std::nothrow) MenuItem();
	if (separator != NULL) {
		separator->SetType(MENU_ITEM_SEPARATOR);
		menu->InsertItem(separator, before);
	}

	// The menu can be shown a second time (when loading the kernel
	// failed); the systems are found once.
	sShortcutCount = 0;
	if (!sSearched) {
		sSearched = true;
		platform_find_other_systems(system_found);
	} else {
		for (OtherSystemList::Iterator it = sSystems.GetIterator();
				OtherSystem* system = it.Next();) {
			add_system_item(system);
		}
	}

	sSearchingItem = new(std::nothrow) MenuItem(
		"(Looking for other media...)");
	if (sSearchingItem != NULL) {
		sSearchingItem->SetType(MENU_ITEM_NO_CHOICE);
		sSearchingItem->SetEnabled(false);
		menu->InsertItem(sSearchingItem, before);
	}
	menu->SetIdleHook(idle_hook);

	if (defaultName != NULL && sDefaultItem == NULL) {
		dprintf("boot menu: default \"%s\" not found, using \"Continue "
			"booting\"\n", defaultName);
	}
	if (_defaultItem != NULL)
		*_defaultItem = sDefaultItem;
}


void
other_systems_cleanup()
{
	sMenu = NULL;
	sInsertBefore = NULL;
	sSearchingItem = NULL;
	sDefaultItem = NULL;
	sDefaultName = NULL;
}
