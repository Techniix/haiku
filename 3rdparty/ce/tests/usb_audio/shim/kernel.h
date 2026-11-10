/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_AUDIO_TEST_SHIM_KERNEL_H
#define _USB_AUDIO_TEST_SHIM_KERNEL_H

/*!	Stands in for the kernel's <kernel.h> when usb_audio_uac2_test builds the
	usb_audio driver's sources as part of a userland program: the test is the
	"user" (every address is a user address) and the areas the driver makes
	for itself are ordinary areas of the test team.
*/

#include <OS.h>
#include <KernelExport.h>

#define IS_USER_ADDRESS(x)	true
#define IS_KERNEL_ADDRESS(x)	true

#undef B_KERNEL_READ_AREA
#undef B_KERNEL_WRITE_AREA
#define B_KERNEL_READ_AREA	B_READ_AREA
#define B_KERNEL_WRITE_AREA	B_WRITE_AREA
#define B_ANY_KERNEL_ADDRESS	B_ANY_ADDRESS

#include "shim_dprintf.h"

#endif	// _USB_AUDIO_TEST_SHIM_KERNEL_H
