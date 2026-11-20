/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DISPLAYS_VIEW_H
#define DISPLAYS_VIEW_H


#include <Accelerant.h>
#include <Screen.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <vector>


class BButton;
class BCheckBox;
class BMenuField;
class BPopUpMenu;
class BSpinner;
class BStringView;
class ArrangementView;


/*!	Screen -> Displays: one page for every display setting (Renga, batch 4;
	built as a page for the Control Panel, F-28, so it has no window of its
	own: DisplaysWindow hosts it for now).

	Arrangement, per-display resolution and refresh rate, scale, rotation,
	primary display and mirroring, VRR and HDR, colour profile, the Wayland
	switch, workspaces and GPU information. What the system or the driver
	cannot do yet is shown greyed with a note that says why, so the page
	does not change shape when support arrives (the multi-head interface,
	<add-ons/graphics/MultiHead.h>, and the GPU stage).
*/
class DisplaysView : public BView {
public:
								DisplaysView();

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

			// for the harness
			int32				CountDisplays() const;
			int32				CountModes() const;
			bool				FeatureEnabled(const char* name) const;
			const char*			GpuName() const;

private:
			void				_ScanDisplays();
			void				_SelectDisplay(int32 index);
			void				_FillModes();
			void				_FillRefreshRates();
			void				_UpdateGpuInfo();
			void				_Apply();
			BView*				_Feature(const char* name, BView* control,
									const char* note);

			struct Display {
				screen_id		id;
				BRect			frame;
				BString			name;
			};

			std::vector<Display> fDisplays;
			int32				fCurrent;
			std::vector<display_mode> fModes;

			ArrangementView*	fArrangement;
			BPopUpMenu*			fDisplayMenu;
			BMenuField*			fDisplayField;
			BPopUpMenu*			fResolutionMenu;
			BMenuField*			fResolutionField;
			BPopUpMenu*			fRefreshMenu;
			BMenuField*			fRefreshField;
			BMenuField*			fScaleField;
			BMenuField*			fRotationField;
			BCheckBox*			fPrimaryBox;
			BCheckBox*			fMirrorBox;
			BCheckBox*			fVrrBox;
			BCheckBox*			fHdrBox;
			BMenuField*			fColorField;
			BCheckBox*			fWaylandBox;
			BSpinner*			fWorkspaces;
			BStringView*		fGpuName;
			BStringView*		fGpuChipset;
			BStringView*		fGpuMemory;
			BStringView*		fGpuDriver;
			BButton*			fApplyButton;
};


class DisplaysWindow : public BWindow {
public:
								DisplaysWindow();
};


#endif	// DISPLAYS_VIEW_H
