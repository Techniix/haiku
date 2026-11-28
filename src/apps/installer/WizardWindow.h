/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WIZARD_WINDOW_H
#define WIZARD_WINDOW_H


/*!	The Installer's wizard (roadmap F-23, batch 14, first piece): pages
	Welcome, Destination, Summary, Progress and Finish. It is the
	Installer's default window; the classic window stays one menu item
	away (Options > Use the classic Installer window, also on the Welcome
	page; saved in ~/config/settings/Renga/installer).

	Destination offers: erase a disk, install alongside in a disk's free
	space (both planned on the shared disk engine through
	DestinationPlan), an existing volume (upgrade, replace or erase, the
	WorkerThread modes), or manual partitioning (DriveSetup, then a
	volume). Summary lists every change (WizardPlan) with the disk before
	and after; nothing is written until Install is pressed, and erasing a
	disk asks once more, naming the disk. Progress applies the plan (the
	engine saves the partition table first) and then runs the classic
	installation (WorkerThread, CopyEngine).

	Hardware check, Boot, Account and network and Software pages are not
	part of the flow yet (roadmap F-23, next pieces).
*/


#include <String.h>
#include <Window.h>

#include "WizardPlan.h"


class BButton;
class BCardLayout;
class BCheckBox;
class BMenu;
class BMenuField;
class BMenuItem;
class BMessageRunner;
class BRadioButton;
class BStatusBar;
class BStringView;
class BTextControl;
class BTextView;
class DiskBarView;
class WorkerThread;


/*!	Command-line options for the wizard (InstallerCLI.cpp): "--wizard"
	opens it whatever the setting says, "--classic" the classic window.
	--page, --choice, --disk, --target, --mode and --name preset it (for
	screenshots and the harness, which cannot send the Installer
	messages); with --page or --auto the licence window is skipped.
	--auto presses Next on every page after a few seconds and installs;
	erasing a disk then needs --confirm-erase with the disk's path (the
	second confirmation), erasing a volume --confirm-erase with its name.
	With any of them, each page and the summary are printed on standard
	output ("Installer: wizard page <name>", "Installer: summary: ...").
*/
struct WizardOptions {
								WizardOptions();

			bool				wizard;
			bool				classic;
			bool				autoRun;
			bool				lean;
			BString				page;
			BString				choice;
			BString				disk;
			BString				target;
			BString				mode;
			BString				name;
			BString				confirmErase;

			bool				Given() const
									{ return wizard || autoRun
										|| page.Length() > 0; }
};

extern WizardOptions gWizardOptions;


//!	Whether the Installer opens the classic window (the setting; the
//!	wizard is the default).
bool installer_uses_classic_window();
status_t set_installer_uses_classic_window(bool classic);

//!	Sent to be_app: "wizard" (bool) chooses the window, saves the setting
//!	and closes the other one.
static const uint32 kMsgSwitchInstallerWindow = 'iSWW';

//!	Launches DriveSetup or BootManager (\a signature, else \a fileName in
//!	the system's apps folder); false if neither worked.
bool launch_installer_tool(const char* signature, const char* fileName);


class WizardWindow : public BWindow {
public:
								WizardWindow();
	virtual						~WizardWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			enum {
				kWelcomePage = 0,
				kDestinationPage,
				kSummaryPage,
				kProgressPage,
				kFinishPage,
				kPageCount
			};

			BView*				_MakeWelcomePage();
			BView*				_MakeDestinationPage();
			BView*				_MakeSummaryPage();
			BView*				_MakeProgressPage();
			BView*				_MakeFinishPage();

			void				_ShowPage(int32 page);
			void				_Next();
			void				_Back();
			void				_ApplyOptions();
			void				_ScanDisks();
			void				_ScanVolumes();
			void				_UpdateDestination();
			bool				_MakeChoice(WizardChoice& choice);
			void				_FillSummary();
			bool				_ConfirmDiskErase();
			bool				_SummaryStillValid();
			void				_RefuseWhileApplying();
			void				_KeepBackupOnTarget();
			void				_StartInstallation();
			void				_PlanApplied(BMessage* message);
			void				_StartCopy(partition_id target,
									partition_id newEFIPartition);
			void				_Finished(bool success, const char* text);
			void				_Stop(bool ask);
			void				_SetCancelSemaphore(sem_id semaphore,
									bool alreadyLocked = false);
			void				_ScheduleAuto(bigtime_t delay);
			void				_Report(const char* format, ...);

			BCardLayout*		fCards;
			int32				fPage;
			BStringView*		fTitle;
			BStringView*		fStep;
			BButton*			fBackButton;
			BButton*			fNextButton;
			BButton*			fCancelButton;
			BMenuItem*			fClassicItem;

			// Destination
			BRadioButton*		fEraseRadio;
			BRadioButton*		fAlongsideRadio;
			BRadioButton*		fExistingRadio;
			BRadioButton*		fManualRadio;
			BMenu*				fDiskMenu;
			BMenuField*			fDiskField;
			BTextControl*		fNameControl;
			BMenu*				fVolumeMenu;
			BMenuField*			fVolumeField;
			BMenu*				fModeMenu;
			BMenuField*			fModeField;
			BButton*			fDriveSetupButton;
			BTextView*			fDestinationNote;

			// Summary
			BTextView*			fChangesView;
			DiskBarView*		fBeforeBar;
			DiskBarView*		fAfterBar;
			BStringView*		fAfterLabel;
			BCheckBox*			fLeanCheckBox;
			BTextView*			fSummaryNote;

			// Progress and Finish
			BTextView*			fProgressText;
			BStatusBar*			fProgressBar;
			BTextView*			fFinishText;

			WizardChoice		fChoice;
			WizardSummary		fSummary;
			bool				fChoiceValid;
			WorkerThread*		fWorkerThread;
			sem_id				fCancelSemaphore;
			bool				fInstalling;
			bool				fInstalled;
			bool				fDriveSetupRunning;
			bool				fApplying;		// the disk plan runs
			BString				fBackupPath;	// its table backup
			partition_id		fTarget;
			firmware_kind		fFirmware;
			BMessageRunner*		fAutoRunner;
};


#endif	// WIZARD_WINDOW_H
