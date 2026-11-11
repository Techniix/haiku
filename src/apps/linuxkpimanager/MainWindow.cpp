/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI Manager main window. All information comes from
 * liblinuxkpi_manage (core/); this file only presents it.
 */


#include "MainWindow.h"

#include <strings.h>

#include <algorithm>

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <File.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Path.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>

#include <string.h>

#include <lkpi_control.h>

#include "SettingsWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "MainWindow"


using namespace LKPI;


static BTextView*
make_text_view(const char* name)
{
	BTextView* view = new BTextView(name);
	view->MakeEditable(false);
	view->SetStylable(false);
	view->SetWordWrap(true);
	return view;
}


static BColumnListView*
make_list(const char* name)
{
	BColumnListView* list = new BColumnListView(name,
		B_NAVIGABLE | B_WILL_DRAW, B_FANCY_BORDER, true);
	list->SetSortingEnabled(true);
	return list;
}


static void
add_column(BColumnListView* list, const char* title, float width)
{
	list->AddColumn(new BStringColumn(title, width, 40, 1000,
		B_TRUNCATE_END), list->CountColumns());
}


static BRow*
add_row(BColumnListView* list, const char* a, const char* b = NULL,
	const char* c = NULL, const char* d = NULL, const char* e = NULL)
{
	const char* fields[] = { a, b, c, d, e };
	BRow* row = new BRow();
	for (int32 i = 0; i < list->CountColumns() && i < 5; i++)
		row->SetField(new BStringField(fields[i] ? fields[i] : ""), i);
	list->AddRow(row);
	return row;
}


static const char*
row_text(const BRow* row, int32 field)
{
	// every field in our lists is a BStringField (add_row())
	const BStringField* f = static_cast<const BStringField*>(
		row->GetField(field));
	return f != NULL ? f->String() : "";
}


// #pragma mark -


MainWindow::MainWindow()
	:
	BWindow(BRect(80, 80, 900, 640), B_TRANSLATE_SYSTEM_NAME("LinuxKPI Manager"),
		B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE),
	fSavePanel(NULL)
{
	BMenuBar* menuBar = new BMenuBar("menu");
	BMenu* file = new BMenu(B_TRANSLATE("File"));
	file->AddItem(new BMenuItem(B_TRANSLATE("Export diagnostic report" B_UTF8_ELLIPSIS),
		new BMessage(kMsgExportReport), 'S'));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem(B_TRANSLATE("Quit"),
		new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(file);
	BMenu* view = new BMenu(B_TRANSLATE("View"));
	view->AddItem(new BMenuItem(B_TRANSLATE("Refresh"),
		new BMessage(kMsgRefresh), 'R'));
	menuBar->AddItem(view);

	fTabs = new BTabView("tabs", B_WIDTH_FROM_LABEL);
	fTabs->AddTab(_BuildOverview());
	fTabs->AddTab(_BuildDrivers());
	fTabs->AddTab(_BuildHardware());
	fTabs->AddTab(_BuildDiagnostics());
	fTabs->AddTab(_BuildLog());
	fTabs->AddTab(_BuildCompatibility());
	fTabs->SetBorder(B_NO_BORDER);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_SMALL_SPACING,
				B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING)
			.Add(fTabs)
		.End();

	fLogs.LoadRules();
	_Refresh();
	// The layout's preferred size is narrow (the lists' columns are cut
	// off); start at the size the constructor's frame asked for.
	ResizeTo(std::max(Bounds().Width(), 760.0f),
		std::max(Bounds().Height(), 520.0f));
	CenterOnScreen();
}


MainWindow::~MainWindow()
{
	delete fSavePanel;
}


bool
MainWindow::QuitRequested()
{
	return true;
}


// #pragma mark - tabs


BView*
MainWindow::_BuildOverview()
{
	fOverviewText = make_text_view("overview text");
	BView* view = new BView(B_TRANSLATE("Overview"), 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(new BScrollView("overview scroll", fOverviewText, 0, false,
			true));
	return view;
}


BView*
MainWindow::_BuildDrivers()
{
	fDriverList = make_list("drivers");
	add_column(fDriverList, B_TRANSLATE("Driver"), 100);
	add_column(fDriverList, B_TRANSLATE("Version"), 100);
	add_column(fDriverList, B_TRANSLATE("Installed"), 90);
	add_column(fDriverList, B_TRANSLATE("At boot"), 70);
	add_column(fDriverList, B_TRANSLATE("Running"), 90);
	fDriverList->SetSelectionMessage(new BMessage(kMsgDriverSelected));

	fDriverDetails = make_text_view("driver details");
	fBootButton = new BButton("boot", B_TRANSLATE("Disable at boot"),
		new BMessage(kMsgToggleBoot));
	fSettingsButton = new BButton("settings",
		B_TRANSLATE("Settings" B_UTF8_ELLIPSIS), new BMessage(kMsgOpenSettings));
	fBootButton->SetEnabled(false);
	fSettingsButton->SetEnabled(false);

	BView* view = new BView(B_TRANSLATE("Drivers"), 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddSplit(B_VERTICAL)
			.Add(fDriverList, 1)
			.Add(new BScrollView("details scroll", fDriverDetails, 0, false,
				true), 2)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fBootButton)
			.Add(fSettingsButton)
		.End();
	return view;
}


BView*
MainWindow::_BuildHardware()
{
	fHardwareList = make_list("hardware");
	add_column(fHardwareList, B_TRANSLATE("PCI ID"), 90);
	add_column(fHardwareList, B_TRANSLATE("Device"), 160);
	add_column(fHardwareList, B_TRANSLATE("Type"), 190);
	add_column(fHardwareList, B_TRANSLATE("Driver"), 90);
	add_column(fHardwareList, B_TRANSLATE("Status"), 260);

	BView* view = new BView(B_TRANSLATE("Hardware"), 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(new BStringView("hardware hint", B_TRANSLATE(
			"PCI devices that a LinuxKPI driver supports.")))
		.Add(fHardwareList);
	return view;
}


BView*
MainWindow::_BuildDiagnostics()
{
	fDiagList = make_list("diagnostics");
	add_column(fDiagList, B_TRANSLATE("Severity"), 70);
	add_column(fDiagList, B_TRANSLATE("Driver"), 80);
	add_column(fDiagList, B_TRANSLATE("Problem"), 240);
	add_column(fDiagList, B_TRANSLATE("Message"), 400);
	fDiagList->SetSelectionMessage(new BMessage(kMsgDiagSelected));
	fDiagAdvice = make_text_view("advice");

	BView* view = new BView(B_TRANSLATE("Diagnostics"), 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddSplit(B_VERTICAL)
			.Add(fDiagList, 2)
			.Add(new BScrollView("advice scroll", fDiagAdvice, 0, false,
				true), 1)
		.End();
	return view;
}


BView*
MainWindow::_BuildLog()
{
	fLogSearch = new BTextControl("search", B_TRANSLATE("Search:"), "",
		new BMessage(kMsgLogFilter));
	fLogSearch->SetModificationMessage(new BMessage(kMsgLogFilter));
	fLogPrevious = new BCheckBox("previous", B_TRANSLATE("Include previous log"),
		new BMessage(kMsgRefresh));
	fLogProblemsOnly = new BCheckBox("problems",
		B_TRANSLATE("Problems only"), new BMessage(kMsgLogFilter));
	fLogText = make_text_view("log");
	fLogText->SetWordWrap(false);

	BView* view = new BView(B_TRANSLATE("Log"), 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.AddGroup(B_HORIZONTAL)
			.Add(fLogSearch)
			.Add(fLogProblemsOnly)
			.Add(fLogPrevious)
		.End()
		.Add(new BScrollView("log scroll", fLogText, 0, true, true));
	return view;
}


BView*
MainWindow::_BuildCompatibility()
{
	fCoverageList = make_list("coverage");
	add_column(fCoverageList, B_TRANSLATE("Area"), 80);
	add_column(fCoverageList, B_TRANSLATE("Interface"), 260);
	add_column(fCoverageList, B_TRANSLATE("Status"), 100);
	add_column(fCoverageList, B_TRANSLATE("Notes"), 320);
	fCoverageBar = new BStatusBar("coverage bar",
		B_TRANSLATE("Implemented"));
	fCoverageSummary = new BStringView("coverage summary", "");

	BView* view = new BView(B_TRANSLATE("Compatibility"), 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING)
		.Add(fCoverageBar)
		.Add(fCoverageSummary)
		.Add(fCoverageList);
	return view;
}


// #pragma mark - content


void
MainWindow::_Refresh()
{
	fCatalog.Load();
	ScanPCIDevices(fDevices);
	fLogs.Read(fCatalog, fLogPrevious->Value() == B_CONTROL_ON);
	LoadCoverage(fCoverage, fCoverageVersion);

	_FillOverview();
	_FillDrivers();
	_FillHardware();
	_FillDiagnostics();
	_FillLog();
	_FillCompatibility();
}


void
MainWindow::_FillOverview()
{
	BString text;
	int32 running = 0, problems = 0;
	for (int32 i = 0; i < fCatalog.CountDrivers(); i++) {
		if (!fCatalog.DriverAt(i)->interfaces.IsEmpty())
			running++;
	}
	for (int32 i = 0; i < fLogs.CountLines(); i++) {
		if (fLogs.LineAt(i)->severity == kSeverityError)
			problems++;
	}

	text << B_TRANSLATE("LinuxKPI lets Haiku run drivers written for Linux.")
		<< "\n\n";
	if (fCatalog.CountDrivers() == 0) {
		text << B_TRANSLATE("No LinuxKPI driver descriptions are installed. "
			"They belong in data/linuxkpi/drivers/ (see the project's "
			"testing guide).") << "\n";
	}

	BString line;
	line.SetToFormat(B_TRANSLATE("Drivers known: %" B_PRId32 ", running: %"
		B_PRId32), fCatalog.CountDrivers(), running);
	text << line << "\n";

	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		const PCIDevice* dev = fDevices.ItemAt(i);
		const DeviceId* id = NULL;
		Driver* d = fCatalog.FindDriverFor(dev->vendor, dev->device, &id);
		if (d == NULL)
			continue;
		line.SetToFormat(B_TRANSLATE("Found %s (%04x:%04x), driver %s: %s"),
			id->name.String(), dev->vendor, dev->device, d->name.String(),
			!d->interfaces.IsEmpty() ? B_TRANSLATE("running")
				: !d->Installed() ? B_TRANSLATE("not installed")
				: !d->enabledAtBoot ? B_TRANSLATE("disabled at boot")
				: B_TRANSLATE("installed, no interface (see Diagnostics)"));
		text << line << "\n";
	}

	line.SetToFormat(B_TRANSLATE("Errors in the log: %" B_PRId32), problems);
	text << "\n" << line << "\n";
	if (problems > 0)
		text << B_TRANSLATE("Open the Diagnostics tab for explanations.")
			<< "\n";
	fOverviewText->SetText(text.String());
}


Driver*
MainWindow::_SelectedDriver() const
{
	BRow* row = fDriverList->CurrentSelection();
	return row != NULL ? fCatalog.FindDriver(row_text(row, 0)) : NULL;
}


void
MainWindow::_FillDrivers()
{
	BString selected;
	if (Driver* d = _SelectedDriver())
		selected = d->name;

	fDriverList->Clear();
	for (int32 i = 0; i < fCatalog.CountDrivers(); i++) {
		Driver* d = fCatalog.DriverAt(i);
		BRow* row = add_row(fDriverList, d->name.String(), d->version.String(),
			!d->Installed() ? B_TRANSLATE("No")
				: d->nonPackaged ? B_TRANSLATE("Yes")
				: B_TRANSLATE("From package"),
			d->enabledAtBoot ? B_TRANSLATE("Yes") : B_TRANSLATE("No"),
			!d->interfaces.IsEmpty() ? B_TRANSLATE("Yes")
				: d->published ? B_TRANSLATE("Published")
				: B_TRANSLATE("No"));
		if (d->name == selected)
			fDriverList->AddToSelection(row);
	}
	_FillDriverDetails();
}


void
MainWindow::_FillDriverDetails()
{
	Driver* d = _SelectedDriver();
	fBootButton->SetEnabled(d != NULL && d->Installed() && d->nonPackaged);
	fSettingsButton->SetEnabled(d != NULL && !d->tunables.IsEmpty());
	if (d == NULL) {
		fDriverDetails->SetText(B_TRANSLATE("Select a driver."));
		return;
	}
	fBootButton->SetLabel(d->enabledAtBoot ? B_TRANSLATE("Disable at boot")
		: B_TRANSLATE("Enable at boot"));

	BString t;
	t << d->description << "\n\n";
	t << B_TRANSLATE("Version:") << " " << d->version << "\n";
	t << B_TRANSLATE("Origin:") << " " << d->origin << "\n";
	t << B_TRANSLATE("License:") << " " << d->license << "\n";
	t << B_TRANSLATE("Needs:") << " " << d->dependencies.Join(", ") << "\n";
	t << B_TRANSLATE("File:") << " " << (d->Installed()
		? d->binaryPath.String() : B_TRANSLATE("not installed")) << "\n";

	t << "\n" << B_TRANSLATE("Supported devices:") << "\n";
	for (int32 i = 0; i < d->devices.CountItems(); i++) {
		const DeviceId* id = d->devices.ItemAt(i);
		BString line;
		line.SetToFormat("  %04x:%04x  %s\n", id->vendor, id->device,
			id->name.String());
		t << line;
	}

	t << "\n" << B_TRANSLATE("Features:") << "\n";
	for (int32 i = 0; i < d->features.CountItems(); i++) {
		const Feature* f = d->features.ItemAt(i);
		t << "  " << f->label << ": " << FeatureStateName(f->state) << "\n";
	}

	for (int32 n = 0; n < d->interfaces.CountStrings(); n++) {
		const char* iface = d->interfaces.StringAt(n).String();
		t << "\n" << B_TRANSLATE("Running as") << " " << iface << ":\n";
		static const uint32 kOps[] = { LKPI_CTL_GET_INFO, LKPI_CTL_GET_STATS };
		for (size_t o = 0; o < B_COUNT_OF(kOps); o++) {
			BString reply;
			status_t status = KernelQuery::Query(iface, kOps[o], reply);
			if (status == B_NOT_SUPPORTED) {
				t << "  " << B_TRANSLATE("(this build has no status "
					"interface)") << "\n";
				break;
			}
			if (status != B_OK) {
				t << "  " << strerror(status) << "\n";
				break;
			}
			BStringList lines;
			reply.Split("\n", true, lines);
			for (int32 l = 0; l < lines.CountStrings(); l++)
				t << "  " << lines.StringAt(l) << "\n";
		}
	}
	fDriverDetails->SetText(t.String());
}


void
MainWindow::_FillHardware()
{
	fHardwareList->Clear();
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		const PCIDevice* dev = fDevices.ItemAt(i);
		const DeviceId* id = NULL;
		Driver* d = fCatalog.FindDriverFor(dev->vendor, dev->device, &id);
		if (d == NULL)
			continue;

		BString pci, status;
		pci.SetToFormat("%04x:%04x", dev->vendor, dev->device);
		BStringList missing;
		d->MissingFirmware(*id, missing);
		if (!d->Installed())
			status = B_TRANSLATE("Driver not installed");
		else if (!missing.IsEmpty()) {
			status.SetToFormat(B_TRANSLATE("Missing firmware: %s"),
				missing.Join(", ").String());
		} else if (!d->interfaces.IsEmpty())
			status = B_TRANSLATE("Working");
		else if (!d->enabledAtBoot)
			status = B_TRANSLATE("Driver disabled at boot");
		else
			status = B_TRANSLATE("Not running (see Diagnostics)");

		add_row(fHardwareList, pci.String(), id->name.String(),
			PCIClassName(dev->classBase, dev->classSub), d->name.String(),
			status.String());
	}
}


void
MainWindow::_FillDiagnostics()
{
	fDiagList->Clear();
	for (int32 i = 0; i < fLogs.CountLines(); i++) {
		const LogLine* line = fLogs.LineAt(i);
		if (line->rule < 0 || line->severity == kSeverityInfo)
			continue;
		const DiagnosticRule* rule = fLogs.RuleAt(line->rule);
		add_row(fDiagList, SeverityName(line->severity),
			line->driver.String(), rule->title.String(), line->text.String());
	}
	fDiagAdvice->SetText(fDiagList->CountRows() == 0
		? B_TRANSLATE("No known problems found in the log.")
		: B_TRANSLATE("Select a problem to see what to do about it."));
}


void
MainWindow::_FillLog()
{
	BString filter(fLogSearch->Text());
	bool problemsOnly = fLogProblemsOnly->Value() == B_CONTROL_ON;

	BString text;
	for (int32 i = 0; i < fLogs.CountLines(); i++) {
		const LogLine* line = fLogs.LineAt(i);
		if (problemsOnly && line->severity == kSeverityInfo)
			continue;
		if (!filter.IsEmpty() && line->text.IFindFirst(filter) < 0)
			continue;
		text << line->text << "\n";
	}
	fLogText->SetText(text.String());
}


void
MainWindow::_FillCompatibility()
{
	fCoverageList->Clear();
	int32 counts[kCoverageUnknown + 1] = {};
	for (int32 i = 0; i < fCoverage.CountItems(); i++) {
		const Coverage* c = fCoverage.ItemAt(i);
		counts[c->state]++;
		add_row(fCoverageList, c->group.String(), c->name.String(),
			CoverageStateName(c->state), c->note.String());
	}

	int32 total = fCoverage.CountItems();
	// partial counts half: a rough, honest single number
	float score = total > 0 ? (counts[kCoverageImplemented]
		+ counts[kCoveragePartial] * 0.5f) * 100.0f / total : 0;
	fCoverageBar->Reset(B_TRANSLATE("Implemented"));
	fCoverageBar->SetMaxValue(100);
	BString trailing;
	trailing.SetToFormat("%.0f%%", score);
	fCoverageBar->Update(score, NULL, trailing.String());

	BString summary;
	summary.SetToFormat(B_TRANSLATE("LinuxKPI %s: %" B_PRId32 " implemented, %"
		B_PRId32 " partial, %" B_PRId32 " stub, %" B_PRId32 " missing "
		"(of %" B_PRId32 " areas)"), fCoverageVersion.String(),
		counts[kCoverageImplemented], counts[kCoveragePartial],
		counts[kCoverageStub], counts[kCoverageMissing], total);
	fCoverageSummary->SetText(total > 0 ? summary.String()
		: B_TRANSLATE("Coverage data is not installed."));
}


// #pragma mark - actions


void
MainWindow::_ToggleBoot()
{
	Driver* d = _SelectedDriver();
	if (d == NULL)
		return;
	bool enable = !d->enabledAtBoot;

	BString text;
	if (enable) {
		text.SetToFormat(B_TRANSLATE("Load %s when Haiku starts?\n\n"
			"This takes effect at the next restart. If the computer does not "
			"start afterwards, choose \"Disable user add-ons\" in the boot "
			"menu's safe mode options and switch it off again here."),
			d->name.String());
	} else {
		text.SetToFormat(B_TRANSLATE("Stop loading %s when Haiku starts?\n\n"
			"This takes effect at the next restart. The driver stays "
			"installed."), d->name.String());
	}
	BAlert* alert = new BAlert(B_TRANSLATE("Driver at boot"), text.String(),
		B_TRANSLATE("Cancel"), enable ? B_TRANSLATE("Enable")
			: B_TRANSLATE("Disable"), NULL, B_WIDTH_AS_USUAL,
		B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	if (alert->Go() != 1)
		return;

	BString error;
	if (fCatalog.SetEnabledAtBoot(d, enable, error) != B_OK) {
		(new BAlert(B_TRANSLATE("Driver at boot"), error.String(),
			B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
			B_STOP_ALERT))->Go();
	}
	_FillDrivers();
	_FillOverview();
	_FillHardware();
}


void
MainWindow::_SaveReport(BMessage* message)
{
	entry_ref dir;
	const char* name;
	if (message->FindRef("directory", &dir) != B_OK
		|| message->FindString("name", &name) != B_OK)
		return;
	BPath path(&dir);
	path.Append(name);

	BString report;
	BuildReport(fCatalog, report);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() != B_OK
		|| file.Write(report.String(), report.Length()) != report.Length()) {
		(new BAlert(B_TRANSLATE("Export report"),
			B_TRANSLATE("The report could not be written."),
			B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
			B_STOP_ALERT))->Go();
		return;
	}
	(new BAlert(B_TRANSLATE("Export report"), B_TRANSLATE(
		"Report saved. Please read it before sharing: log lines can contain "
		"network names and hardware addresses."), B_TRANSLATE("OK")))->Go();
}


void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgRefresh:
		case kMsgSettingsChanged:
			_Refresh();
			break;

		case kMsgSelectTab:
		{
			// in the order of the constructor's AddTab() calls; the
			// labels are translated, so match on these fixed names
			static const char* kTabNames[] = { "overview", "drivers",
				"hardware", "diagnostics", "log", "compatibility" };
			const char* name = message->GetString("tab", "");
			for (int32 i = 0; i < (int32)B_COUNT_OF(kTabNames); i++) {
				if (strcasecmp(name, kTabNames[i]) == 0) {
					fTabs->Select(i);
					break;
				}
			}
			break;
		}

		case kMsgDriverSelected:
			_FillDriverDetails();
			break;

		case kMsgToggleBoot:
			_ToggleBoot();
			break;

		case kMsgOpenSettings:
			if (Driver* d = _SelectedDriver())
				(new SettingsWindow(d->name, BMessenger(this)))->Show();
			break;

		case kMsgDiagSelected:
		{
			BRow* row = fDiagList->CurrentSelection();
			if (row == NULL)
				break;
			const char* text = row_text(row, 3);
			for (int32 i = 0; i < fLogs.CountLines(); i++) {
				const LogLine* line = fLogs.LineAt(i);
				if (line->rule < 0 || line->text != text)
					continue;
				const DiagnosticRule* rule = fLogs.RuleAt(line->rule);
				BString advice;
				advice << rule->title << "\n\n" << (rule->advice.IsEmpty()
					? B_TRANSLATE("No specific advice for this message. "
						"Export a diagnostic report (File menu) and include "
						"it when asking for help.")
					: rule->advice.String());
				fDiagAdvice->SetText(advice.String());
				break;
			}
			break;
		}

		case kMsgLogFilter:
			_FillLog();
			break;

		case kMsgExportReport:
			if (message->HasRef("directory")) {
				_SaveReport(message);
				break;
			}
			if (fSavePanel == NULL) {
				BMessenger target(this);
				fSavePanel = new BFilePanel(B_SAVE_PANEL, &target, NULL, 0,
					false, new BMessage(kMsgExportReport));
			}
			fSavePanel->SetSaveText("linuxkpi-report.txt");
			fSavePanel->Show();
			break;

		default:
			BWindow::MessageReceived(message);
	}
}
