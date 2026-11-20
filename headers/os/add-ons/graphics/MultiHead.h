/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MULTI_HEAD_H
#define _MULTI_HEAD_H


/*!	Several displays on one accelerant (Renga: multi-monitor in app_server).

	Version 2 (batch 6), as the Quasar NVIDIA driver asked in its R-19
	(docs/partners/QUASAR.md, 2026-11-20, accepted): version 1 was
	provisional and had no implementation, so its slots keep their places
	with the meanings below. Later changes are add-only: new hooks go at
	the end of multi_head_hooks, new fields into _reserved, and
	B_MULTI_HEAD_VERSION goes up. The accelerant fills in the version it
	implements; app_server uses only what that version has.

	Model:
	 - The table's numbers are display OUTPUTS, 0 .. count_outputs() - 1.
	   Connectors keep their numbers while they exist; MST displays come
	   after the connectors. Output 0 is the one the classic hooks
	   (B_SET_DISPLAY_MODE and friends) drive, so an accelerant without this
	   table is a one-output card, as today.
	 - Hardware heads (scan-out engines) are fewer than outputs on most
	   cards: count_hardware_heads(). Enabling more outputs than there are
	   heads returns B_BUSY (set_output_mode() and set_configuration()).
	 - Each enabled output has its own frame buffer, mode and DPMS state.
	   <GpuPresent.h>'s "head" arguments are these output numbers.
	 - set_configuration() applies several outputs at once, all or none:
	   which are enabled, their modes, VRR, and mirroring (source_output:
	   an output that scans out another output's frame buffer). With
	   B_MULTI_HEAD_TEST_ONLY nothing changes; the result says whether the
	   configuration would work.
	 - The cursor image is the classic hooks' (SET_CURSOR_SHAPE,
	   SET_CURSOR_BITMAP), shared by every output. move_output_cursor()
	   takes coordinates local to that output (the hot spot's position on
	   it, may be negative or past its size while the cursor crosses an
	   edge); show_output_cursor() shows or hides it per output.
	 - Hot-plug: event_fd() (the same kind as gpu_present_hooks::event_fd())
	   is readable after a connector changed; app_server then calls
	   get_output_info() for every output again.
	 - Refresh rates are in millihertz (*_millihz).

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
#define B_MULTI_HEAD_VERSION	2


// multi_head_info::flags
enum {
	B_OUTPUT_CONNECTED		= 0x01,	// a display is attached
	B_OUTPUT_ENABLED		= 0x02,	// scanning out (a mode is set)
	B_OUTPUT_VRR			= 0x04,	// the display and output can do VRR
	B_OUTPUT_HDR			= 0x08,	// the display reports HDR metadata
	B_OUTPUT_INTERNAL		= 0x10,	// built-in panel (laptop)
	B_OUTPUT_MST			= 0x20	// a DisplayPort MST display (version 2)
};

// version 1 names
#define B_HEAD_CONNECTED	B_OUTPUT_CONNECTED
#define B_HEAD_ENABLED		B_OUTPUT_ENABLED
#define B_HEAD_VRR			B_OUTPUT_VRR
#define B_HEAD_HDR			B_OUTPUT_HDR
#define B_HEAD_INTERNAL		B_OUTPUT_INTERNAL

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
	uint32		flags;				// B_OUTPUT_*
	uint32		connector;			// B_CONNECTOR_*
	uint32		connector_index;	// HDMI-1, HDMI-2...: 1, 2...
	char		name[32];			// the display's name (EDID), or ""
	char		vendor[16];			// EDID manufacturer, or ""
	uint32		serial;				// EDID serial number, or 0
	uint32		width_mm;			// physical size, 0 if unknown
	uint32		height_mm;
	display_mode preferred;			// EDID preferred mode, if connected
	uint32		max_refresh_millihz;	// highest refresh (VRR range)
	uint32		min_refresh_millihz;	// lowest, 0 if fixed
	// version 2, in what were reserved fields:
	uint32		max_bpc;			// bits per colour channel, 0 if unknown
	uint32		gamma_size;			// gamma table entries, 0 if none
	uint32		_reserved[6];
} multi_head_info;


// multi_head_config::flags
enum {
	B_OUTPUT_CONFIG_ENABLED		= 0x01,	// scan out; off without it
	B_OUTPUT_CONFIG_ALLOW_VRR	= 0x02	// variable refresh if supported
};

// set_configuration() flags
enum {
	B_MULTI_HEAD_TEST_ONLY		= 0x01	// check, change nothing
};

#define B_NO_SOURCE_OUTPUT		0xffffffff


typedef struct {
	uint32		output;
	uint32		flags;				// B_OUTPUT_CONFIG_*
	uint32		source_output;		// B_NO_SOURCE_OUTPUT, or the output
									// whose frame buffer this one mirrors
									// (its mode must fit that buffer)
	uint32		_reserved0;
	display_mode mode;				// ignored when not enabled, and taken
									// from the source when mirroring
	uint32		_reserved[8];
} multi_head_config;


typedef struct {
	uint32		version;			// B_MULTI_HEAD_VERSION implemented
	uint32		(*count_outputs)(void);
	status_t	(*get_output_info)(uint32 output, multi_head_info* info);
	uint32		(*mode_count)(uint32 output);
	status_t	(*get_mode_list)(uint32 output, display_mode* modes);
	status_t	(*set_output_mode)(uint32 output, const display_mode* mode);
		// NULL turns the output off; B_BUSY: no free hardware head
	status_t	(*get_output_mode)(uint32 output, display_mode* mode);
	status_t	(*get_frame_buffer_config)(uint32 output,
					frame_buffer_config* config);
	status_t	(*set_output_dpms)(uint32 output, uint32 dpmsFlags);
	status_t	(*move_output_cursor)(uint32 output, int32 x, int32 y);
		// output-local; the image is the classic hooks' (shared)
	status_t	(*show_output_cursor)(uint32 output, bool visible);
	status_t	(*get_output_edid)(uint32 output, void* data, size_t* _size);
		// raw EDID (128 bytes per block); B_BUFFER_OVERFLOW sets *_size
	int			(*event_fd)(void);
		// readable after hot-plug; app_server reads the outputs again

	// version 2
	uint32		(*count_hardware_heads)(void);
	status_t	(*set_configuration)(const multi_head_config* configs,
					uint32 count, uint32 flags);
		// every output not listed is turned off; all or nothing;
		// B_BUSY: more enabled outputs than heads; B_MULTI_HEAD_TEST_ONLY
		// in flags: only check
} multi_head_hooks;


#endif	// _MULTI_HEAD_H
