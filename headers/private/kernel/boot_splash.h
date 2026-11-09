/*
 * Copyright 2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Artur Wyszynski <harakash@gmail.com>
 */
#ifndef KERNEL_BOOT_SPLASH_H
#define KERNEL_BOOT_SPLASH_H


#include <sys/types.h>

enum {
	BOOT_SPLASH_STAGE_1_INIT_MODULES = 0,
	BOOT_SPLASH_STAGE_2_BOOTSTRAP_FS,
	BOOT_SPLASH_STAGE_3_INIT_DEVICES,
	BOOT_SPLASH_STAGE_4_MOUNT_BOOT_FS,
	BOOT_SPLASH_STAGE_5_INIT_CPU_MODULES,
	BOOT_SPLASH_STAGE_6_INIT_VM_MODULES,
	BOOT_SPLASH_STAGE_7_RUN_BOOT_SCRIPT,

	BOOT_SPLASH_STAGE_MAX // keep this at the end
};


#ifdef __cplusplus
extern "C" {
#endif

void boot_splash_init(uint8 * boot_splash);
void boot_splash_uninit(void);
void boot_splash_set_stage(int stage);

/* Stops the splash animation for good and waits until it no longer touches
   the frame buffer. Called before anyone else takes the frame buffer. */
void boot_splash_stop_animation(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_BOOT_SPLASH_H */
