/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_BOOT_LOADER_STATE_H
#define KERNEL_BOOT_LOADER_STATE_H


/*!	The boot loader's side of the boot state (Renga, boot loader batch A;
	the format is headers/private/shared/boot_state.h): boot counting,
	automatic recovery and the one-shot choice for the next start.
*/


#include <SupportDefs.h>


class BootVolume;
class PackageVolumeState;


//!	What the boot state asks of this start (loader_state_load()).
enum loader_state_request {
	LOADER_STATE_NOTHING = 0,
	LOADER_STATE_RECOVERY,
		// two or more starts were not confirmed: menu, last working state
	LOADER_STATE_MENU,
		// "Restart into... Show the boot menu"
	LOADER_STATE_SYSTEM,
		// "Restart into..." another operating system of the menu
	LOADER_STATE_STATE
		// "Restart into..." an older system state
};

static const int32 kRecoveryMenuTimeout = 15;
	// seconds the recovery menu counts down before it starts the last
	// working system state


loader_state_request	loader_state_load(BootVolume& volume);
bool					loader_state_needs_recovery();
uint32					loader_state_failed_starts();
PackageVolumeState*		loader_state_last_good(BootVolume& volume);
bool					loader_state_next_system(char* name, size_t size);
status_t				loader_state_apply_next_state(BootVolume& volume);
status_t				loader_state_select(BootVolume& volume,
							PackageVolumeState* state);
void					loader_state_record_start(BootVolume& volume);


#endif	// KERNEL_BOOT_LOADER_STATE_H
