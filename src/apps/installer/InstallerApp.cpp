/*
 * Copyright 2020, Panagiotis "Ivory" Vasilopoulos <git@n0toose.net>
 * Copyright 2015, Axel Dörfler <axeld@pinc-software.de>
 * Copyright 2009, Stephan Aßmus <superstippi@gmx.de>
 * Copyright 2005, Jérôme DUVAL.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "InstallerApp.h"

#include <stdio.h>
#include <unistd.h>

#include <Alert.h>
#include <Roster.h>
#include <TextView.h>

#include <syscalls.h>

#include "tracker_private.h"
#include "WizardWindow.h"
#include "Utility.h"


static const uint32 kMsgAgree = 'agre';
static const uint32 kMsgNext = 'next';

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InstallerApp"


// Renga (F-23): the command line (InstallerCLI.cpp), and the destination
// "--target <volume name>" preselects in the window.
const char* gPreselectedTarget = NULL;
bool gBeginAtOnce = false;
int installer_command_line(int argc, char** argv);


int main(int argc, char** argv)
{
	int status = installer_command_line(argc, argv);
	if (status >= 0)
		return status;

	InstallerApp installer;
	installer.Run();
	return 0;
}


InstallerApp::InstallerApp()
	:
	BApplication("application/x-vnd.Haiku-Installer"),
	fEULAWindow(NULL),
	fWindow(NULL)
{
}


/*!	Renga (F-23): the wizard, unless the setting (or --classic) asks for
	the classic window; "--target <name> --begin" is the classic window's.
*/
void
InstallerApp::_OpenWindow()
{
	bool classic = gBeginAtOnce || gWizardOptions.classic
		|| (!gWizardOptions.Given() && installer_uses_classic_window());
	printf("Installer: opening the %s\n", classic ? "classic window"
		: "wizard");
	fflush(stdout);
	if (classic)
		fWindow = new InstallerWindow();
	else
		fWindow = new WizardWindow();
}


void
InstallerApp::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAgree:
			fEULAWindow->PostMessage(B_QUIT_REQUESTED);
			break;
		case kMsgNext:
			_OpenWindow();
			break;
		case kMsgSwitchInstallerWindow:
		{
			// Options > Use the classic Installer window, Tools > Use the
			// installation wizard: saved, then the other window
			bool wizard = message->GetBool("wizard", true);
			set_installer_uses_classic_window(!wizard);
			gWizardOptions = WizardOptions();
			gWizardOptions.classic = !wizard;
			gWizardOptions.wizard = false;
			gBeginAtOnce = false;
			BWindow* old = fWindow;
			fWindow = NULL;
			_OpenWindow();
			if (old != NULL && old->Lock())
				old->Quit();
			break;
		}

		default:
			BApplication::MessageReceived(message);
	}
}


void
InstallerApp::ReadyToRun()
{
#if 1
	// "--target <name> --begin" (Renga, for automation and the test
	// harness) goes straight to the installation, which then asks how to
	// install over what is there.
	extern bool gBeginAtOnce;
	if (gBeginAtOnce || gWizardOptions.page.Length() > 0
		|| gWizardOptions.autoRun) {
		_OpenWindow();
		return;
	}
	// the setting for the window shown after the licence (the wizard by
	// default)
	if (gWizardOptions.Given() || gWizardOptions.classic) {
		printf("Installer: the licence window first\n");
		fflush(stdout);
	}
	// Show the EULA first.
	fEULAWindow = new EULAWindow();
#else
	// Show the installer window without EULA.
	new InstallerWindow();
#endif
}


void
InstallerApp::Quit()
{
	BApplication::Quit();

	if (!be_roster->IsRunning(kDeskbarSignature)) {
		if (CurrentMessage()->GetBool("install_complete")) {
			// Synchronize disks
			sync();

			if (Utility::IsReadOnlyVolume("/boot")) {
				// Unblock CD tray, and eject the CD
				Utility::BlockMedia("/boot", false);
				Utility::EjectMedia("/boot");
			}

			// Quickly reboot without touching anything
			// on disk (which we might just have ejected)
			_kern_shutdown(true);
		} else {
			// Return to FirstBootPrompt if the user hasn't
			// installed Haiku yet
			be_roster->Launch("application/x-vnd.Haiku-FirstBootPrompt");
		}
	}
}
