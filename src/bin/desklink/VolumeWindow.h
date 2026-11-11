/*
 * Copyright 2003-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Jérôme Duval
 *		François Revol
 *		Axel Dörfler, axeld@pinc-software.de.
 *		John Scipione, jscipione@gmail.com
 */
#ifndef VOLUME_WINDOW_H
#define VOLUME_WINDOW_H


#include <Window.h>

#include "MixerControl.h"


class ApplicationVolumesView;
class BSeparatorView;
class OutputMenuField;
class VolumeControl;

class VolumeWindow : public BWindow {
public:
							VolumeWindow(BRect frame);
	virtual					~VolumeWindow();

			VolumeControl*	VolumeSlider() const { return fVolumeSlider; };

protected:
	virtual	void			MessageReceived(BMessage* message);
	virtual	void			WindowActivated(bool active);

private:
			VolumeControl*	fVolumeSlider;
			OutputMenuField*	fOutput;
			ApplicationVolumesView* fApplications;
			BSeparatorView*	fSeparator;

			int32			fUpdatedCount;
};


#endif	// VOLUME_WINDOW_H
