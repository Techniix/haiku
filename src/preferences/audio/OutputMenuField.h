/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef OUTPUT_MENU_FIELD_H
#define OUTPUT_MENU_FIELD_H


#include <MenuField.h>
#include <String.h>

#include "AudioDevices.h"


/*!	The default output device as a menu (roadmap F-32): every device that
	plays, the default one marked; choosing one makes it the default
	output, which the system mixer follows. Shared by the Audio preferences'
	Output page and the Deskbar volume popup (desklink compiles this file),
	so that both offer the same choice in the same words.
*/
class OutputMenuField : public BMenuField {
public:
								OutputMenuField(const char* name,
									const char* label);

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

			//!	Reads the devices again; rebuilds the menu if they changed.
			void				Refresh();
			//!	The same, from a list the caller has just updated.
			void				Refresh(const AudioDeviceList& devices);

			BString				SelectedName() const;
			int32				CountChoices() const;

private:
			void				_Rebuild(const AudioDeviceList& devices);

			AudioDeviceList		fDevices;
			BString				fKey;
			bigtime_t			fChanged;
};


#endif	// OUTPUT_MENU_FIELD_H
