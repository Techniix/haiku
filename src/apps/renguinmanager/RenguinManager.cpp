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
#include <string.h>

#include "RenguinView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "RenguinManager"


static const char* kSignature = "application/x-vnd.Renga-RenguinManager";


class RenguinWindow : public BWindow {
public:
	RenguinWindow()
		:
		BWindow(BRect(120, 100, 640, 420), B_TRANSLATE_SYSTEM_NAME(
			"Renguin Manager"), B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE
				| B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS)
	{
		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(new RenguinView());
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
	BApplication app(kSignature);
	(new RenguinWindow())->Show();
	app.Run();
	return 0;
}
