/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H


#include <Window.h>

#include "LinuxKPIManage.h"


class BButton;
class BCheckBox;
class BColumnListView;
class BFilePanel;
class BStatusBar;
class BStringView;
class BTabView;
class BTextControl;
class BTextView;


enum {
	kMsgRefresh				= 'rfsh',
	kMsgExportReport		= 'expr',
	kMsgDriverSelected		= 'drsl',
	kMsgToggleBoot			= 'tbot',
	kMsgOpenSettings		= 'opst',
	kMsgDiagSelected		= 'dgsl',
	kMsgLogFilter			= 'lgfl',
	kMsgSettingsChanged		= 'stch',
	kMsgSelectTab			= 'stab'
		// "tab": string, the tab's name in English ("overview",
		// "drivers", "hardware", "diagnostics", "log", "compatibility");
		// sent by App for "--tab <name>" and usable by other programs
};


class MainWindow : public BWindow {
public:
								MainWindow();
	virtual						~MainWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			BView*				_BuildOverview();
			BView*				_BuildDrivers();
			BView*				_BuildHardware();
			BView*				_BuildDiagnostics();
			BView*				_BuildLog();
			BView*				_BuildCompatibility();

			void				_Refresh();
			void				_FillOverview();
			void				_FillDrivers();
			void				_FillDriverDetails();
			void				_FillHardware();
			void				_FillDiagnostics();
			void				_FillLog();
			void				_FillCompatibility();

			LKPI::Driver*		_SelectedDriver() const;
			void				_ToggleBoot();
			void				_SaveReport(BMessage* message);

			LKPI::DriverCatalog	fCatalog;
			LKPI::LogReader		fLogs;
			BObjectList<LKPI::PCIDevice, true> fDevices;
			BObjectList<LKPI::Coverage, true> fCoverage;
			BString				fCoverageVersion;

			BTabView*			fTabs;

			BTextView*			fOverviewText;

			BColumnListView*	fDriverList;
			BTextView*			fDriverDetails;
			BButton*			fBootButton;
			BButton*			fSettingsButton;

			BColumnListView*	fHardwareList;

			BColumnListView*	fDiagList;
			BTextView*			fDiagAdvice;

			BTextControl*		fLogSearch;
			BCheckBox*			fLogPrevious;
			BCheckBox*			fLogProblemsOnly;
			BTextView*			fLogText;

			BColumnListView*	fCoverageList;
			BStatusBar*			fCoverageBar;
			BStringView*		fCoverageSummary;

			BFilePanel*			fSavePanel;
};


#endif	// MAIN_WINDOW_H
