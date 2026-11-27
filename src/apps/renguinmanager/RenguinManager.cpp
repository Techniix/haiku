/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renguin Manager (batch 5): Linux programs on Renga. The window holds
	RenguinView, which the Control Panel's "Linux programs (Renguin)" page
	shows too.

	  RenguinManager              the window
	  RenguinManager --status     "layer", "root", "release", "packages"
	                              and "trace" lines, for scripts and tests
*/


#include <Application.h>
#include <Catalog.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ScreenFit.h>

#include "RenguinView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "RenguinManager"


static const char* kSignature = "application/x-vnd.Renga-RenguinManager";


class RenguinWindow : public BWindow {
public:
	RenguinWindow()
		:
		BWindow(BRect(120, 100, 760, 560), B_TRANSLATE_SYSTEM_NAME(
			"Renguin Manager"), B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE
				| B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS)
	{
		// batch 12 (large-font audit): scrolls when the screen is smaller
		// than the view needs; sized from its layout, clamped to the screen
		float inset = be_control_look->ComposeSpacing(B_USE_WINDOW_SPACING);
		BSize limit = BPrivate::screen_content_limit();
		limit.width -= 2 * inset;
		limit.height -= 2 * inset;
		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(BPrivate::fit_to_screen(new RenguinView(), limit));
		BPrivate::size_window_to_screen(this);
		CenterOnScreen();
	}
};


static void
print_status()
{
	// the view's own reading, through a window that is never shown
	BWindow* window = new BWindow(BRect(0, 0, 10, 10), "status",
		B_TITLED_WINDOW, 0);
	RenguinView* view = new RenguinView();
	window->Lock();
	window->AddChild(view);
	view->Refresh();
	const char* names[] = { "layer", "root", "release", "packages" };
	for (size_t i = 0; i < 4; i++) {
		BStringView* line = dynamic_cast<BStringView*>(
			view->FindView(names[i]));
		printf("%s\t%s\n", names[i], line != NULL ? line->Text() : "");
	}
	RenguinView::PrintSwitches();
	window->Quit();
}


int
main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "--status") == 0) {
		BApplication app("application/x-vnd.Renga-RenguinManager-cli");
		print_status();
		return 0;
	}
	// batch 12: the display scale for Linux programs, from scripts and the
	// harness: "--display-scale" prints it, "--display-scale <S>
	// [<program>]" sets it (a program's own with 0 removed) and tells the
	// Wayland compositor, as the Settings and Programs tabs do. No
	// BApplication: a second one with this program's resources but
	// another signature stops in its constructor ("Signature in rsrc
	// doesn't match constructor arg"), and none is needed: the settings
	// file and a BMessenger with a synchronous reply work without one.
	if (argc >= 2 && strcmp(argv[1], "--display-scale") == 0) {
		if (argc == 2) {
			printf("scale\t%.3f\n", RenguinView::DisplayScale());
			return 0;
		}
		float scale = atof(argv[2]);
		if (argc == 4)
			return RenguinView::SetProgramDisplayScale(argv[3], scale)
				? 0 : 1;
		return RenguinView::SetDisplayScale(scale) ? 0 : 1;
	}
	BApplication app(kSignature);
	(new RenguinWindow())->Show();
	app.Run();
	return 0;
}
