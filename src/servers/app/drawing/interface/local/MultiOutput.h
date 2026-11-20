/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MULTI_OUTPUT_H
#define MULTI_OUTPUT_H


#include <Accelerant.h>
#include <GraphicsDefs.h>
#include <Rect.h>
#include <Region.h>

#include <add-ons/graphics/MultiHead.h>


/*!	app_server's side of <add-ons/graphics/MultiHead.h> version 2 (Renga
	batch 6): several outputs of one card as one desktop.

	Stage 1 ("spanning desktop"): the desktop is one frame buffer in memory,
	as large as the arrangement of the outputs; app_server draws into it as
	into one screen (back buffer, then this desktop buffer with the
	cursor), and every change is copied to each output's own frame buffer
	at that output's place. Windows move freely between outputs, since it
	is one coordinate space. Mirrored outputs scan out their source's frame
	buffer in hardware (source_output) and get no copy.

	The arrangement is the displays settings file (see Load()):
	~/config/settings/system/app_server/displays. Without one, every
	connected output is on at its preferred mode, left to right in output
	order, output 0 primary. "multi_screen false" in it turns all of this
	off (one screen, the classic hooks, as before).

	Not yet (recorded in the roadmap): direct windows and BWindowScreen on
	outputs other than through this copy, hardware cursors per output,
	Deskbar and workspaces per output, and hot-plug (event_fd() is read by
	nobody yet).
*/
class MultiOutput {
public:
	struct Output {
		uint32				index;
		bool				enabled;
		bool				primary;
		uint32				mirrorOf;	// B_NO_SOURCE_OUTPUT if none
		BRect				frame;		// in desktop coordinates
		display_mode		mode;
		frame_buffer_config	buffer;
		multi_head_info		info;
	};

	static	MultiOutput*		Create(GetAccelerantHook hook);
								~MultiOutput();

	// Applies the settings; \a primaryMode, if valid for the primary
	// output, replaces its mode. Fills in the desktop's mode and buffer.
			status_t			Apply(const display_mode* primaryMode,
									display_mode& desktopMode,
									frame_buffer_config& desktopBuffer);

	// Copies \a region of the desktop buffer to the outputs.
			void				Push(const BRegion& region) const;
			void				Push(const BRect& rect) const;

			int32				CountOutputs() const { return fCount; }
			const Output&		OutputAt(int32 index) const
									{ return fOutputs[index]; }
			BRect				PrimaryFrame() const;
			BRect				OutputFrameAt(BPoint point) const;

	static	const char*			SettingsPath();

	// True once the last Apply() committed a configuration to the driver,
	// even if Apply() failed after that: the outputs then have new frame
	// buffers (Quasar gives every output a new one, output 0 included).
			bool				Committed() const { return fCommitted; }

private:
								MultiOutput(multi_head_hooks* hooks);
			void				_Load();
			void				_Arrange();

			multi_head_hooks*	fHooks;
			Output				fOutputs[16];
			int32				fCount;
			uint8*				fDesktop;
			area_id				fDesktopArea;
			uint32				fDesktopBytesPerRow;
			BRect				fDesktopFrame;
			bool				fCommitted;
};


#endif	// MULTI_OUTPUT_H
