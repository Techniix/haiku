#pragma once
#include "WlGlobal.h"
#include <SupportDefs.h>


/*	Renga: smaller protocols of the compositor (RengaProtocols.cpp).
	- wl_fixes: wl_registry destruction;
	- wp_fractional_scale_manager_v1: the preferred scale (Renga's UI scale,
	  from the system font size);
	- zwp_idle_inhibit_manager_v1: keeps the screen saver from starting
	  while a client inhibits (ScreenSaver preferences can turn this off);
	- wp_cursor_shape_manager_v1: named cursors as Haiku's system cursors;
	- xdg_toplevel_icon_manager_v1: accepted; icons are not shown yet;
	- zwp_primary_selection_device_manager_v1 (HaikuPrimarySelection.cpp);
	- xdg_activation_v1: tokens, and activate() raises the window.
*/
#define RENGA_SMALL_GLOBAL(name) \
	class name: public WlGlocal { \
	public: \
		static name *Create(struct wl_display *display); \
		virtual ~name() = default; \
		void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override; \
	};

RENGA_SMALL_GLOBAL(HaikuFixesGlobal)
RENGA_SMALL_GLOBAL(HaikuFractionalScaleGlobal)
RENGA_SMALL_GLOBAL(HaikuIdleInhibitGlobal)
RENGA_SMALL_GLOBAL(HaikuCursorShapeGlobal)
RENGA_SMALL_GLOBAL(HaikuToplevelIconGlobal)
RENGA_SMALL_GLOBAL(HaikuPrimarySelectionGlobal)
RENGA_SMALL_GLOBAL(HaikuPointerConstraintsGlobal)
RENGA_SMALL_GLOBAL(HaikuRelativePointerGlobal)
RENGA_SMALL_GLOBAL(HaikuActivationGlobal)
	// HaikuPointerConstraints.cpp
	// HaikuPrimarySelection.cpp

#undef RENGA_SMALL_GLOBAL

class HaikuSurface;
class BPoint;
//! Pointer motion over a surface; true: consumed (the pointer is locked).
bool HaikuPointerConstraintsMotion(HaikuSurface *surface, bigtime_t when,
	BPoint viewWhere);
//! The surface's window lost activation.
void HaikuPointerConstraintsFocusLost(HaikuSurface *surface);
void HaikuPointerConstraintsSurfaceGone(HaikuSurface *surface);

//! The scale wp_fractional_scale_v1 reports, in 1/120 (120 = 1.0).
uint32_t HaikuPreferredScale120();
