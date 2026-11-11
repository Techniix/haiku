/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SETTINGS_WINDOW_H
#define SETTINGS_WINDOW_H


#include <Messenger.h>
#include <ObjectList.h>
#include <Window.h>

#include "LinuxKPIManage.h"


class BControl;


class SettingsWindow : public BWindow {
public:
								SettingsWindow(const BString& driver,
									BMessenger target);

	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Save();
			void				_Reset();

			LKPI::DriverCatalog	fCatalog;
			LKPI::Driver*		fDriver;
			BMessenger			fTarget;
			BObjectList<BControl> fControls;	// one per tunable, same order
};


#endif	// SETTINGS_WINDOW_H
