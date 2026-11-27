/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RENGUIN_VIEW_H
#define RENGUIN_VIEW_H


#include <GroupView.h>
#include <String.h>


class BButton;
class BCheckBox;
class BListView;
class BMenuField;
class BMessageRunner;
class BStringView;
class BTabView;
class BTextControl;


/*!	Renguin Manager's page (batch 5, tabs since batch 10): "Linux system"
	(the compatibility layer's state, the Linux root, its packages),
	"Programs" (the Linux programs with their icons and per-program
	settings), "Running" (the Linux processes), "Settings" (every Renguin
	switch) and "Diagnostics" (the process trace). Used by the RenguinManager
	program and the Control Panel ("Linux programs").
*/
class RenguinView : public BGroupView {
public:
								RenguinView();
	virtual						~RenguinView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

			void				Refresh();

	// A Renguin switch: on or off, from its settings file.
	static	bool				SwitchValue(const char* key);
	static	void				PrintSwitches();

	// Renguin display scale (batch 12): the scale for Linux programs, and
	// a program's own (0: none), from the settings file the Wayland
	// compositor reads
	static	float				DisplayScale(const char* program = NULL);
	// the program name the compositor matches: an Exec line's program
	static	BString				ProgramOfExec(const char* exec);
	// set and applied at once (0.5-4; a program's own 0 is removed)
	static	bool				SetDisplayScale(float scale);
	static	bool				SetProgramDisplayScale(const char* program,
									float scale);

private:
			BView*				_SystemTab();
			BView*				_ProgramsTab();
			BView*				_RunningTab();
			BView*				_SettingsTab();
			BView*				_DiagnosticsTab();

			void				_SaveTrace();
			void				_SaveSwitches();
			void				_RefreshPrograms();
			void				_RefreshProcesses();
			void				_ProgramSelected();
			void				_SaveProgram();
			void				_SetDisplayScale(float scale);
			void				_MarkDisplayScale();

			BTabView*			fTabs;
			BStringView*		fLayer;
			BStringView*		fRoot;
			BStringView*		fRelease;
			BStringView*		fPackages;
			BButton*			fShell;
			BButton*			fSetup;
			BButton*			fUpgrade;
			BButton*			fRemoveRoot;
			BButton*			fDeskbar;
			BCheckBox*			fTrace;
			BTextControl*		fTracePattern;
			BCheckBox*			fTraceBuffer;

			BListView*			fPrograms;
			BCheckBox*			fProgramHidden;
			BTextControl*		fProgramOptions;
			BButton*			fProgramApply;
			BString				fSelectedProgram;
			BString				fSelectedExec;
			BMenuField*			fProgramScale;
			BMenuField*			fDisplayScale;

			BListView*			fProcesses;
			BButton*			fEndProcess;
			BMessageRunner*		fProcessRunner;

			BCheckBox*			fSwitches[16];
				// batch 11: 16 (10 switches now; 8 overflowed)
			int32				fSwitchCount;
};


#endif	// RENGUIN_VIEW_H
