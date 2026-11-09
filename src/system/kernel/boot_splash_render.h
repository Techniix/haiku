/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BOOT_SPLASH_RENDER_H
#define _BOOT_SPLASH_RENDER_H


#include <stdint.h>

#include <boot/splash_player_assets-renga.h>


// Total length of the gust sequence; after it only the light moves.
#define SPLASH_SEQUENCE_MS	3200


/*!	Renders the animation band at "timeMs" into a 24-bit BGR buffer of
	SPLASH_BAND_WIDTH x SPLASH_BAND_HEIGHT pixels. With "settled", renders
	the final logo (no wind, no leaves, wordmark centred, still light).
	Integer arithmetic only: this runs in the kernel, and the same code is
	built on the host to make the boot loader's logo and previews.
*/
void splash_render(uint8_t* bgr, int32_t bytesPerRow, int32_t timeMs,
	bool settled);


#endif	// _BOOT_SPLASH_RENDER_H
