/*
 * Copyright 2008-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Artur Wyszynski <harakash@gmail.com>
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <KernelExport.h>

#define __BOOTSPLASH_KERNEL__
#include <boot/images.h>
#include <boot/platform/generic/video_blitter.h>
#include <boot/platform/generic/video_splash.h>

#include <boot_item.h>
#include <lock.h>
#include <debug.h>
#include <frame_buffer_console.h>

#include <boot_splash.h>

#ifndef HAIKU_DISTRO_COMPATIBILITY_OFFICIAL
#	include <thread.h>
#	include "boot_splash_render.h"
#	define SPLASH_PLAYER 1
#endif


//#define TRACE_BOOT_SPLASH 1
#ifdef TRACE_BOOT_SPLASH
#	define TRACE(x...) dprintf(x);
#else
#	define TRACE(x...) ;
#endif


static struct frame_buffer_boot_info *sInfo;
static uint8 *sUncompressedIcons;


#ifdef SPLASH_PLAYER
/*!	The Renga splash player: a low-priority kernel thread that animates the
	logo band (gust, leaves, dappled light) at ~25 frames per second, drawn
	by boot_splash_render.cpp.

	- It starts with boot_splash_init(). Its first frame is identical to the
	  logo the boot loader drew, so the handoff does not show.
	- It runs below normal priority, so boot work always wins the CPU; on a
	  busy single CPU the animation just gets fewer frames.
	- After boot_splash_uninit() (the boot script starts), it only finishes
	  the gust sequence, draws the settled logo and exits; the screen then
	  keeps the settled logo until the desktop appears.
	- boot_splash_stop_animation() ends it at once; frame_buffer_update()
	  calls it before a graphics driver takes over the frame buffer.
	- The band only covers the logo's rows, so it never overlaps the stage
	  icons, which boot_splash_set_stage() keeps drawing as before.
*/
static frame_buffer_boot_info sPlayerInfo;
static thread_id sPlayerThread = -1;
static int32 sPlayerStop = 0;
static int32 sPlayerFinish = 0;
// Pausing: boot_splash_pause_animation() raises sPlayerPaused and then takes
// sPlayerFrameLock once, which waits for a frame being drawn to finish; the
// player draws each frame under that lock and skips it while paused.
static int32 sPlayerPaused = 0;
static mutex sPlayerFrameLock = MUTEX_INITIALIZER("boot splash frame");

static const bigtime_t kFrameInterval = 40000;	// 25 fps


static void
player_blit(const uint8* band, int32 bandX, int32 bandY, int32 sourceLeft,
	int32 width)
{
	BlitParameters params;
	params.from = band;
	params.fromWidth = SPLASH_BAND_WIDTH;
	params.fromLeft = sourceLeft;
	params.fromTop = 0;
	params.fromRight = sourceLeft + width;
	params.fromBottom = SPLASH_BAND_HEIGHT;
	params.to = (uint8*)sPlayerInfo.frame_buffer;
	params.toBytesPerRow = sPlayerInfo.bytes_per_row;
	params.toLeft = bandX + sourceLeft;
	params.toTop = bandY;
	blit(params, sPlayerInfo.depth);
}


static status_t
player_thread(void*)
{
	int logoWidth, logoHeight, logoX, logoY;
	compute_splash_logo_placement(sPlayerInfo.width, sPlayerInfo.height,
		logoWidth, logoHeight, logoX, logoY);

	// The band is the logo extended sideways; clip it to the screen.
	int32 bandX = logoX - SPLASH_LOGO_LEFT;
	int32 bandY = logoY;
	int32 sourceLeft = bandX < 0 ? -bandX : 0;
	int32 width = min_c((int32)SPLASH_BAND_WIDTH - sourceLeft,
		(int32)sPlayerInfo.width - (bandX + sourceLeft));

	uint8* band = (uint8*)malloc(SPLASH_BAND_WIDTH * SPLASH_BAND_HEIGHT * 3);
	if (band == NULL || width <= 0)
		return B_NO_MEMORY;

	const bigtime_t start = system_time();
	bigtime_t next = start;
	int32 frames = 0;
	while (atomic_get(&sPlayerStop) == 0) {
		int32 timeMs = (int32)((system_time() - start) / 1000);
		bool settle = atomic_get(&sPlayerFinish) != 0
			&& timeMs >= SPLASH_SEQUENCE_MS;

		splash_render(band, SPLASH_BAND_WIDTH * 3, timeMs, settle);
		if (atomic_get(&sPlayerStop) != 0)
			break;

		mutex_lock(&sPlayerFrameLock);
		bool paused = atomic_get(&sPlayerPaused) > 0;
		if (!paused) {
			player_blit(band, bandX, bandY, sourceLeft, width);
			frames++;
		}
		mutex_unlock(&sPlayerFrameLock);
		if (settle && !paused)
			break;

		next += kFrameInterval;
		bigtime_t now = system_time();
		if (next < now)
			next = now;		// we fell behind: skip, don't catch up
		snooze_until(next, B_SYSTEM_TIMEBASE);
	}

	free(band);
	// One line for the syslog: shows how smoothly it ran on this machine.
	dprintf("boot splash: %" B_PRId32 " frames in %" B_PRId64 " ms%s\n",
		frames, (system_time() - start) / 1000,
		atomic_get(&sPlayerStop) != 0 ? " (stopped)" : "");
	return B_OK;
}


static void
start_player()
{
	if (sInfo == NULL || sInfo->depth == 8)
		return;	// 8-bit modes keep the static logo

	int logoWidth, logoHeight, logoX, logoY;
	compute_splash_logo_placement(sInfo->width, sInfo->height, logoWidth,
		logoHeight, logoX, logoY);
	if (logoWidth < kSplashLogoWidth || logoHeight < kSplashLogoHeight)
		return;	// clipped logo: keep it static

	sPlayerInfo = *sInfo;
	sPlayerThread = spawn_kernel_thread(&player_thread, "boot splash",
		B_NORMAL_PRIORITY - 2, NULL);
	if (sPlayerThread >= 0)
		resume_thread(sPlayerThread);
}
#endif	// SPLASH_PLAYER


//	#pragma mark - exported functions


void
boot_splash_init(uint8 *bootSplash)
{
	TRACE("boot_splash_init: enter\n");

	if (debug_screen_output_enabled())
		return;

	sInfo = (frame_buffer_boot_info *)get_boot_item(FRAME_BUFFER_BOOT_INFO,
		NULL);

	sUncompressedIcons = bootSplash;

#ifdef SPLASH_PLAYER
	start_player();
#endif
}


void
boot_splash_uninit(void)
{
	sInfo = NULL;

#ifdef SPLASH_PLAYER
	// Let the player finish the gust and settle; don't wait for it.
	atomic_set(&sPlayerFinish, 1);
#endif
}


void
boot_splash_stop_animation(void)
{
#ifdef SPLASH_PLAYER
	thread_id thread = sPlayerThread;
	if (thread < 0)
		return;
	atomic_set(&sPlayerStop, 1);
	if (thread != thread_get_current_thread_id()) {
		status_t result;
		wait_for_thread(thread, &result);
	}
	sPlayerThread = -1;
#endif
}


void
boot_splash_pause_animation(bool pause)
{
#ifdef SPLASH_PLAYER
	if (!pause) {
		atomic_add(&sPlayerPaused, -1);
		return;
	}

	atomic_add(&sPlayerPaused, 1);
	// No waiting inside the kernel debugger (the PCI "pcirefresh" command
	// gets here): the player is not running then anyway.
	if (sPlayerThread >= 0 && !debug_debugger_running()
		&& sPlayerThread != thread_get_current_thread_id()) {
		// Wait for a frame that is being drawn right now.
		mutex_lock(&sPlayerFrameLock);
		mutex_unlock(&sPlayerFrameLock);
	}
#endif
}


void
boot_splash_set_stage(int stage)
{
	TRACE("boot_splash_set_stage: stage=%d\n", stage);

	if (sInfo == NULL || stage < 0 || stage >= BOOT_SPLASH_STAGE_MAX)
		return;

	int width, height, x, y;
	compute_splash_icons_placement(sInfo->width, sInfo->height,
		width, height, x, y);

	int stageLeftEdge = width * stage / BOOT_SPLASH_STAGE_MAX;
	int stageRightEdge = width * (stage + 1) / BOOT_SPLASH_STAGE_MAX;

	BlitParameters params;
	params.from = sUncompressedIcons;
	params.fromWidth = kSplashIconsWidth;
	params.fromLeft = stageLeftEdge;
	params.fromTop = 0;
	params.fromRight = stageRightEdge;
	params.fromBottom = height;
	params.to = (uint8*)sInfo->frame_buffer;
	params.toBytesPerRow = sInfo->bytes_per_row;
	params.toLeft = stageLeftEdge + x;
	params.toTop = y;

	blit(params, sInfo->depth);
}
