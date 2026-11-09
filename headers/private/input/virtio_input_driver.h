/*
 * Copyright 2021-26, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VIRTIO_INPUT_DRIVER_H_
#define _VIRTIO_INPUT_DRIVER_H_


#include <Drivers.h>


enum {
	virtioInputRead = B_DEVICE_OP_CODES_END + 1,
	virtioInputCancelIO = B_DEVICE_OP_CODES_END + 2,
	virtioInputGetType = B_DEVICE_OP_CODES_END + 3,
	virtioInputGetAbsRange = B_DEVICE_OP_CODES_END + 4,
		// VirtioInputAbsRange: a tablet's ABS_X and ABS_Y ranges
};


enum VirtioInputType {
	kVirtioInputUnknown = 0,
	kVirtioInputTablet,
	kVirtioInputKeyboard,
};


// The value ranges of a tablet's absolute axes, as its device configuration
// gives them (VIRTIO_INPUT_CFG_ABS_INFO).
struct VirtioInputAbsRange {
	int32 xMin, xMax;
	int32 yMin, yMax;
};


#endif	// _VIRTIO_INPUT_DRIVER_H_
