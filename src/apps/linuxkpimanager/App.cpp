/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <Application.h>

#include <string.h>

#include "MainWindow.h"


static const char* kSignature = "application/x-vnd.LinuxKPI-Manager";


class App : public BApplication {
public:
	App()
		:
		BApplication(kSignature)
	{
	}

	virtual void ReadyToRun()
	{
		_Window();
	}

	// "LinuxKPIManager --tab <name>" opens (or, B_SINGLE_LAUNCH, brings
	// the running window) at that tab; see kMsgSelectTab.
	virtual void ArgvReceived(int32 argc, char** argv)
	{
		for (int32 i = 1; i + 1 < argc; i++) {
			if (strcmp(argv[i], "--tab") == 0) {
				BMessage select(kMsgSelectTab);
				select.AddString("tab", argv[i + 1]);
				_Window()->PostMessage(&select);
			}
		}
	}

private:
	MainWindow* _Window()
	{
		if (fWindow == NULL) {
			fWindow = new MainWindow();
			fWindow->Show();
		}
		return fWindow;
	}

	MainWindow*	fWindow = NULL;
};


int
main()
{
	App app;
	app.Run();
	return 0;
}
