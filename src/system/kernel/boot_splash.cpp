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
#include <debug.h>
#include <frame_buffer_console.h>

#include <boot_splash.h>


//#define TRACE_BOOT_SPLASH 1
#ifdef TRACE_BOOT_SPLASH
#	define TRACE(x...) dprintf(x);
#else
#	define TRACE(x...) ;
#endif


static struct frame_buffer_boot_info *sInfo;
static uint8 *sUncompressedIcons;

#ifdef SPLASH_ANIMATION
// Logo animation (see boot/splash_animation-*.h): frame n + 1 is drawn when
// stage n starts. The boot loader decompressed the frames right after the
// icons, in the 24-bit case only.
static int32 sFramesShown = 0;


static void
show_animation_frames(int stage)
{
	if (sInfo->depth == 8)
		return;

	int logoWidth, logoHeight, logoX, logoY;
	compute_splash_logo_placement(sInfo->width, sInfo->height, logoWidth,
		logoHeight, logoX, logoY);
	if (logoWidth < kSplashLogoWidth || logoHeight < kSplashLogoHeight)
		return;	// clipped logo: keep the static one

	// Frames are deltas: draw every frame up to this stage's, in order.
	int32 target = min_c(stage + 1, kSplashAnimationFrameCount);
	while (sFramesShown < target) {
		uint8* data = sUncompressedIcons
			+ (size_t)kSplashIconsWidth * kSplashIconsHeight * 3;
		for (int32 i = 0; i < sFramesShown; i++) {
			data += (size_t)kSplashAnimationFrames[i].width
				* kSplashAnimationFrames[i].height * 3;
		}

		const splash_animation_frame& frame
			= kSplashAnimationFrames[sFramesShown];
		if (frame.width > 0 && frame.height > 0) {
			BlitParameters params;
			params.from = data;
			params.fromWidth = frame.width;
			params.fromLeft = 0;
			params.fromTop = 0;
			params.fromRight = frame.width;
			params.fromBottom = frame.height;
			params.to = (uint8*)sInfo->frame_buffer;
			params.toBytesPerRow = sInfo->bytes_per_row;
			params.toLeft = logoX + frame.x;
			params.toTop = logoY + frame.y;
			blit(params, sInfo->depth);
		}
		sFramesShown++;
	}
}
#endif


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
}


void
boot_splash_uninit(void)
{
	sInfo = NULL;
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

#ifdef SPLASH_ANIMATION
	show_animation_frames(stage);
#endif
}
