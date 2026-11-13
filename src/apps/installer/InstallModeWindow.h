/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef INSTALL_MODE_WINDOW_H
#define INSTALL_MODE_WINDOW_H


#include <Window.h>

#include "InstallationInfo.h"


class BButton;
class BCheckBox;
class BRadioButton;


/*!	Asks what to do with a destination volume that is not empty (Renga,
	F-23): upgrade an installation keeping files and applications (the
	default, offered only when there is an installation), replace the
	system keeping files, or erase the volume. Go() blocks the calling
	thread, like BAlert::Go(), and returns the choice or
	INSTALL_MODE_CANCEL.
*/
class InstallModeWindow : public BWindow {
public:
								InstallModeWindow(const char* volumeName,
									const InstallationInfo& existing,
									bool* renameVolume = NULL);
	virtual						~InstallModeWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

			install_mode		Go();

private:
			BRadioButton*		fUpgrade;
			BRadioButton*		fReplace;
			BRadioButton*		fErase;
			sem_id				fDone;
			install_mode		fResult;
			BCheckBox*			fRename;		// F-59
			bool*				fRenameVolume;
};


#endif	// INSTALL_MODE_WINDOW_H
