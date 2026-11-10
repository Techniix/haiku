/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_AUDIO_TEST_SHIM_DPRINTF_H
#define _USB_AUDIO_TEST_SHIM_DPRINTF_H

/*!	The kernel's dprintf(), for usb_audio_uac2_test. POSIX has a dprintf()
	of its own (with a file descriptor first), so the driver's calls are
	renamed. Included by kernel.h and lock.h, one of which every driver
	source includes.
*/

#include <SupportDefs.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_audio_test_dprintf(const char* format, ...) _PRINTFLIKE(1, 2);

#ifdef __cplusplus
}
#endif

#define dprintf usb_audio_test_dprintf

#endif	// _USB_AUDIO_TEST_SHIM_DPRINTF_H
