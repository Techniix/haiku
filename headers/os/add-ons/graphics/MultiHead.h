/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MULTI_HEAD_H
#define _MULTI_HEAD_H


/*!	Several displays on one accelerant (Renga, roadmap batch 4 item 3:
	multi-monitor in app_server).

	PROVISIONAL, version 1, like <GpuPresent.h> before the GPU stage: the
	table the accelerant offers, so app_server can drive several outputs of
	one card as separate screens. app_server does not call it yet; the first
	implementation is expected from the Quasar NVIDIA driver (QUASAR.md
	2026-11-20). Every change bumps B_MULTI_HEAD_VERSION, and the accelerant
	fills in the version it implements.

	Model:
	 - A head is a CRTC/scan-out engine; an output is a connector (HDMI,
	   DisplayPort...). Heads are numbered 0 .. count_heads() - 1; head 0 is
	   the one the classic hooks (B_SET_DISPLAY_MODE and friends) drive, so
	   an accelerant without this table is a one-head card, as today.
	 - Each head has its own frame buffer, mode, DPMS state, cursor and
	   (with <GpuPresent.h>) planes: the "head" arguments there are these
	   numbers.
	 - Hot-plug: event_fd() (the same kind as gpu_present_hooks::event_fd())
	   is readable after a connector changed; app_server then calls
	   get_head_info() for every head again.
	 - Mirroring is two heads scanning out the same frame buffer
	   (set_head_mode() with the same frame buffer); app_server decides.

	Offered through get_accelerant_hook():
		multi_head_hooks* hooks
			= (multi_head_hooks*)get_accelerant_hook(B_MULTI_HEAD_HOOKS,
				NULL);
	Called by app_server only (the accelerant's owner).
*/


#include "Accelerant.h"
#include <GraphicsDefs.h>
#include <OS.h>


// get_accelerant_hook() feature code ('RGB', vendor neutral; RGA is
// GpuPresent).
#define B_MULTI_HEAD_HOOKS		0x52474200
#define B_MULTI_HEAD_VERSION	1


// multi_head_info::flags
enum {
	B_HEAD_CONNECTED		= 0x01,	// a display is attached
	B_HEAD_ENABLED			= 0x02,	// scanning out (a mode is set)
	B_HEAD_VRR				= 0x04,	// the display and head can do VRR
	B_HEAD_HDR				= 0x08,	// the display reports HDR metadata
	B_HEAD_INTERNAL			= 0x10	// built-in panel (laptop)
};

// multi_head_info::connector
enum {
	B_CONNECTOR_UNKNOWN = 0,
	B_CONNECTOR_VGA,
	B_CONNECTOR_DVI,
	B_CONNECTOR_HDMI,
	B_CONNECTOR_DISPLAYPORT,
	B_CONNECTOR_EMBEDDED,		// eDP/LVDS
	B_CONNECTOR_VIRTUAL
};


typedef struct {
	uint32		flags;				// B_HEAD_*
	uint32		connector;			// B_CONNECTOR_*
	uint32		connector_index;	// HDMI-1, HDMI-2...: 1, 2...
	char		name[32];			// the display's name (EDID), or ""
	char		vendor[16];			// EDID manufacturer, or ""
	uint32		serial;				// EDID serial number, or 0
	uint32		width_mm;			// physical size, 0 if unknown
	uint32		height_mm;
	display_mode preferred;			// EDID preferred mode, if connected
	uint32		max_refresh_mhz;	// highest refresh in millihertz, VRR
	uint32		min_refresh_mhz;	// lowest (VRR range), 0 if fixed
	uint32		_reserved[8];
} multi_head_info;


typedef struct {
	uint32		version;			// B_MULTI_HEAD_VERSION implemented
	uint32		(*count_heads)(void);
	status_t	(*get_head_info)(uint32 head, multi_head_info* info);
	uint32		(*mode_count)(uint32 head);
	status_t	(*get_mode_list)(uint32 head, display_mode* modes);
	status_t	(*set_head_mode)(uint32 head, const display_mode* mode);
		// NULL turns the head off
	status_t	(*get_head_mode)(uint32 head, display_mode* mode);
	status_t	(*get_frame_buffer_config)(uint32 head,
					frame_buffer_config* config);
	status_t	(*set_head_dpms)(uint32 head, uint32 dpmsFlags);
	status_t	(*move_head_cursor)(uint32 head, uint16 x, uint16 y);
	status_t	(*show_head_cursor)(uint32 head, bool visible);
	status_t	(*get_head_edid)(uint32 head, void* data, size_t* _size);
		// raw EDID (128 bytes per block); B_BUFFER_OVERFLOW sets *_size
	int			(*event_fd)(void);
		// readable after hot-plug; app_server reads the heads again
} multi_head_hooks;


#endif	// _MULTI_HEAD_H
