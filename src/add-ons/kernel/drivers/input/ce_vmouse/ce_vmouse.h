/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef CE_VMOUSE_H
#define CE_VMOUSE_H


/*!	Renga harness: the interface of the ce_vmouse test driver (harness
	images only, see ce_vmouse.cpp). A test program opens
	/dev/misc/ce_vmouse, plugs a virtual mouse in with CE_VMOUSE_PLUG, and
	write()s ce_vmouse_report records, which the mouse's nodes hand to
	input_server's mouse add-on like a real driver would.
*/


#include <Drivers.h>
#include <keyboard_mouse_driver.h>


#define CE_VMOUSE_CONTROL_NAME	"misc/ce_vmouse"
#define CE_VMOUSE_NODE_FORMAT	"input/mouse/virtual/%d"
	// the add-on names the nodes "Virtual Mouse 1", "Virtual Mouse 2"

enum {
	// how the nodes hand out reports, like the two kinds of real drivers
	CE_VMOUSE_QUEUED		= 0,
		// every report is kept and read one by one, and MS_NUM_EVENTS says
		// how many wait (PS/2 keeps a ring of packets)
	CE_VMOUSE_ON_DEMAND		= 1
		// motion that nobody has read yet is summed into one report, as the
		// mouse's firmware does between two USB polls; MS_NUM_EVENTS is
		// always 0 (usb_hid)
};

enum {
	CE_VMOUSE_PLUG = B_DEVICE_OP_CODES_END + 0xce00,
		// ce_vmouse_plug: publishes the nodes; the add-on finds them
	CE_VMOUSE_UNPLUG,
		// removes them again (also when the control node is closed)
	CE_VMOUSE_PENDING
		// returns how many reports are waiting to be read, all nodes
};

typedef struct {
	uint32		nodes;		// 1 or 2 HID mouse interfaces of one device
	uint32		mode;		// CE_VMOUSE_QUEUED or CE_VMOUSE_ON_DEMAND
} ce_vmouse_plug;

typedef struct {
	uint32		node;		// which interface reports it
	uint32		reserved;
	mouse_movement movement;
		// a zero timestamp is replaced by the time it is read
} ce_vmouse_report;

#define CE_VMOUSE_MAX_NODES	2
#define CE_VMOUSE_DEVICE_ID	0xce00
	// the USB device number both nodes give for MS_GET_DEVICE_IDENTITY


#endif	// CE_VMOUSE_H
