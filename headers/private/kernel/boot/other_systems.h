/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_BOOT_OTHER_SYSTEMS_H
#define KERNEL_BOOT_OTHER_SYSTEMS_H


/*!	Other operating systems in the boot menu (Renga, roadmap F-22).

	The generic part (src/system/boot/loader/other_systems.cpp) puts the
	systems in the main menu, assigns the shortcut keys 1-9, applies the
	"boot_menu_default" setting and runs the background search for removable
	media from the menu's idle hook. The platform finds the systems and
	starts them:

	- platform_find_other_systems() is called once, when the menu is built,
	  for systems on fixed disks (EFI: loaders on EFI system partitions;
	  BIOS: boot records of primary partitions and other disks' MBRs).
	- platform_probe_other_media() is called repeatedly while the main menu
	  waits for a key. Each call may do at most one bounded step (one device
	  read, or checking whether an asynchronous read finished), so the menu
	  answers keys between steps. It returns false once there is nothing
	  left to look at for now.
	- platform_start_other_system() starts a system. It returns only if the
	  system could not be started or (EFI applications) returned.

	Platforms without an implementation get weak defaults that find nothing.
*/


#include <SupportDefs.h>
#include <util/DoublyLinkedList.h>

#include <boot_other_systems.h>


enum other_system_kind {
	OTHER_SYSTEM_INSTALLED = 0,	// on a fixed disk, found when the menu opens
	OTHER_SYSTEM_REMOVABLE,		// "boot other media", found in the background
};


struct OtherSystem : DoublyLinkedListLinkImpl<OtherSystem> {
	// What the boot_menu_default setting is compared with, and the start
	// of the menu label ("Windows Boot Manager").
	char				name[BootOtherSystems::kMaxNameLength];
	// Where it is, for the help text and the log, for example
	// "\EFI\Microsoft\Boot\bootmgfw.efi on EFI volume 1" or
	// "partition 2 of BIOS drive 0x81".
	char				location[160];
	other_system_kind	kind;
	// Platform data, owned by the platform.
	void*				cookie;
};


typedef void (*other_system_found_hook)(OtherSystem* system);


#ifdef __cplusplus
extern "C" {
#endif

// platform
void		platform_find_other_systems(other_system_found_hook found);
bool		platform_probe_other_media(other_system_found_hook found);
status_t	platform_start_other_system(OtherSystem* system);

#ifdef __cplusplus
}
#endif


class Menu;
class MenuItem;

// generic, for user_menu()
void		other_systems_add_to_menu(Menu* menu, MenuItem* before,
				const char* defaultName, MenuItem** _defaultItem);
void		other_systems_cleanup();


#endif	/* KERNEL_BOOT_OTHER_SYSTEMS_H */
