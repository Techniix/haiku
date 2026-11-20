/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "DisplaysView.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Font.h>
#include <GridLayoutBuilder.h>
#include <GroupLayoutBuilder.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Spinner.h>
#include <StringView.h>

#include "WaylandSwitch.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Displays"


static const uint32 kMsgDisplay = 'dsel';
static const uint32 kMsgResolution = 'dres';
static const uint32 kMsgRefresh = 'dref';
static const uint32 kMsgWayland = 'dwln';
static const uint32 kMsgWorkspaces = 'dwks';
static const uint32 kMsgApply = 'dapl';


static float
mode_refresh(const display_mode& mode)
{
	if (mode.timing.h_total == 0 || mode.timing.v_total == 0)
		return 0;
	return mode.timing.pixel_clock * 1000.0f
		/ (mode.timing.h_total * mode.timing.v_total);
}


/*!	The displays as rectangles in their desktop positions, the selected one
	highlighted. With one display there is nothing to drag; moving displays
	comes with the multi-head interface.
*/
class ArrangementView : public BView {
public:
	ArrangementView()
		:
		BView("arrangement", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fSelected(0)
	{
		SetExplicitMinSize(BSize(320, 140));
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	}

	void SetDisplays(const std::vector<BRect>& frames,
		const std::vector<BString>& names, int32 selected)
	{
		fFrames = frames;
		fNames = names;
		fSelected = selected;
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		if (fFrames.empty())
			return;
		BRect all = fFrames[0];
		for (size_t i = 1; i < fFrames.size(); i++)
			all = all | fFrames[i];
		BRect bounds = Bounds().InsetByCopy(12, 12);
		float scale = std::min(bounds.Width() / all.Width(),
			bounds.Height() / all.Height());
		float offsetX = bounds.left + (bounds.Width() - all.Width() * scale)
			/ 2;
		float offsetY = bounds.top + (bounds.Height() - all.Height() * scale)
			/ 2;
		for (size_t i = 0; i < fFrames.size(); i++) {
			BRect frame = fFrames[i];
			BRect rect((frame.left - all.left) * scale + offsetX,
				(frame.top - all.top) * scale + offsetY,
				(frame.right - all.left) * scale + offsetX,
				(frame.bottom - all.top) * scale + offsetY);
			rect.InsetBy(2, 2);
			SetHighUIColor((int32)i == fSelected ? B_CONTROL_HIGHLIGHT_COLOR
				: B_CONTROL_BACKGROUND_COLOR);
			FillRoundRect(rect, 4, 4);
			SetHighUIColor(B_CONTROL_BORDER_COLOR);
			StrokeRoundRect(rect, 4, 4);
			SetHighUIColor(B_PANEL_TEXT_COLOR);
			BString label;
			label << (int32)(i + 1) << ": " << fNames[i];
			font_height height;
			GetFontHeight(&height);
			DrawString(label.String(), BPoint(
				rect.left + (rect.Width() - StringWidth(label.String())) / 2,
				rect.top + rect.Height() / 2 + height.ascent / 2));
		}
	}

private:
	std::vector<BRect>		fFrames;
	std::vector<BString>	fNames;
	int32					fSelected;
};


DisplaysView::DisplaysView()
	:
	BView("Displays", 0),
	fCurrent(0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fArrangement = new ArrangementView();

	fDisplayMenu = new BPopUpMenu("display");
	fDisplayField = new BMenuField("display", B_TRANSLATE("Display:"),
		fDisplayMenu);
	fResolutionMenu = new BPopUpMenu("resolution");
	fResolutionField = new BMenuField("resolution",
		B_TRANSLATE("Resolution:"), fResolutionMenu);
	fRefreshMenu = new BPopUpMenu("refresh");
	fRefreshField = new BMenuField("refresh", B_TRANSLATE("Refresh rate:"),
		fRefreshMenu);

	BPopUpMenu* scaleMenu = new BPopUpMenu("scale");
	float scale = be_plain_font->Size() / 12.0f;
	BString scaleLabel;
	scaleLabel.SetToFormat("%d%%", (int)(scale * 100 + 0.5f));
	scaleMenu->AddItem(new BMenuItem(scaleLabel.String(), NULL));
	scaleMenu->ItemAt(0)->SetMarked(true);
	fScaleField = new BMenuField("scale", B_TRANSLATE("Scale:"), scaleMenu);

	BPopUpMenu* rotationMenu = new BPopUpMenu("rotation");
	rotationMenu->AddItem(new BMenuItem(B_TRANSLATE("Normal"), NULL));
	rotationMenu->AddItem(new BMenuItem(B_TRANSLATE("90°"), NULL));
	rotationMenu->AddItem(new BMenuItem(B_TRANSLATE("180°"), NULL));
	rotationMenu->AddItem(new BMenuItem(B_TRANSLATE("270°"), NULL));
	rotationMenu->ItemAt(0)->SetMarked(true);
	fRotationField = new BMenuField("rotation", B_TRANSLATE("Rotation:"),
		rotationMenu);

	fPrimaryBox = new BCheckBox("primary", B_TRANSLATE("Primary display"),
		NULL);
	fMirrorBox = new BCheckBox("mirror", B_TRANSLATE("Mirror displays"),
		NULL);
	fVrrBox = new BCheckBox("vrr", B_TRANSLATE("Variable refresh rate"),
		NULL);
	fHdrBox = new BCheckBox("hdr", B_TRANSLATE("HDR"), NULL);

	BPopUpMenu* colorMenu = new BPopUpMenu("color");
	colorMenu->AddItem(new BMenuItem(B_TRANSLATE("sRGB (no profile)"),
		NULL));
	colorMenu->ItemAt(0)->SetMarked(true);
	fColorField = new BMenuField("color", B_TRANSLATE("Colour profile:"),
		colorMenu);

	fWaylandBox = new BCheckBox("wayland",
		B_TRANSLATE("Show Wayland programs"), new BMessage(kMsgWayland));
	fWorkspaces = new BSpinner("workspaces", B_TRANSLATE("Workspaces:"),
		new BMessage(kMsgWorkspaces));
	fWorkspaces->SetRange(1, 32);

	fGpuName = new BStringView("gpu_name", "");
	fGpuChipset = new BStringView("gpu_chipset", "");
	fGpuMemory = new BStringView("gpu_memory", "");
	fGpuDriver = new BStringView("gpu_driver", "");

	fApplyButton = new BButton("apply", B_TRANSLATE("Apply"),
		new BMessage(kMsgApply));

	// One row per setting; features that cannot work yet are greyed, with
	// the reason next to them.
	const char* oneDisplay = B_TRANSLATE("Needs a second display");
	const char* driver = B_TRANSLATE("Needs driver support (multi-head "
		"interface)");
	const char* gpuStage = B_TRANSLATE("Needs a GPU driver that reports it");

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fArrangement, 2)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.AddMenuField(fDisplayField, 0, 0)
			.AddMenuField(fResolutionField, 0, 1)
			.AddMenuField(fRefreshField, 0, 2)
			.AddMenuField(fScaleField, 0, 3)
			.Add(new BStringView("scale_note", B_TRANSLATE(
				"Follows the system font size (Appearance)")), 2, 3)
			.AddMenuField(fRotationField, 0, 4)
			.Add(new BStringView("rotation_note", driver), 2, 4)
			.AddMenuField(fColorField, 0, 5)
			.Add(new BStringView("color_note", B_TRANSLATE(
				"Colour management is not available yet")), 2, 5)
		.End()
		.Add(_Feature("primary", fPrimaryBox, oneDisplay))
		.Add(_Feature("mirror", fMirrorBox, oneDisplay))
		.Add(_Feature("vrr", fVrrBox, gpuStage))
		.Add(_Feature("hdr", fHdrBox, gpuStage))
		.Add(fWaylandBox)
		.AddGroup(B_HORIZONTAL)
			.Add(fWorkspaces)
			.AddGlue()
		.End()
		.AddGroup(B_VERTICAL, 0)
			.Add(new BStringView("gpu_title", B_TRANSLATE("Graphics card")))
			.Add(fGpuName)
			.Add(fGpuChipset)
			.Add(fGpuMemory)
			.Add(fGpuDriver)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fApplyButton)
		.End();

	// not possible yet
	fScaleField->SetEnabled(false);
	fRotationField->SetEnabled(false);
	fColorField->SetEnabled(false);
	fVrrBox->SetEnabled(false);
	fHdrBox->SetEnabled(false);
}


BView*
DisplaysView::_Feature(const char* name, BView* control, const char* note)
{
	BString noteName(name);
	noteName << "_note";
	BStringView* noteView = new BStringView(noteName.String(), note);
	noteView->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
	return BLayoutBuilder::Group<>(B_HORIZONTAL)
		.Add(control)
		.Add(noteView)
		.AddGlue()
		.View();
}


void
DisplaysView::AttachedToWindow()
{
	BView::AttachedToWindow();
	fDisplayMenu->SetTargetForItems(this);
	fWaylandBox->SetTarget(this);
	fWorkspaces->SetTarget(this);
	fApplyButton->SetTarget(this);

	fWaylandBox->SetValue(wayland_compositor_enabled()
		? B_CONTROL_ON : B_CONTROL_OFF);
	fWorkspaces->SetValue(count_workspaces());

	_ScanDisplays();
	_SelectDisplay(0);
}


void
DisplaysView::_ScanDisplays()
{
	fDisplays.clear();
	BScreen screen(Window());
	screen_id first = screen.ID();
	screen_id id = first;
	for (int32 guard = 0; guard < 16; guard++) {
		BScreen current(id);
		if (!current.IsValid())
			break;
		Display display;
		display.id = id;
		display.frame = current.Frame();
		monitor_info monitor;
		if (current.GetMonitorInfo(&monitor) == B_OK
			&& monitor.name[0] != '\0') {
			display.name = monitor.name;
		} else
			display.name = B_TRANSLATE("Display");
		fDisplays.push_back(display);
		if (current.SetToNext() != B_OK || current.ID().id == first.id)
			break;
		id = current.ID();
	}

	while (fDisplayMenu->CountItems() > 0)
		delete fDisplayMenu->RemoveItem((int32)0);
	std::vector<BRect> frames;
	std::vector<BString> names;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		BString label;
		label << (int32)(i + 1) << ": " << fDisplays[i].name;
		BMessage* message = new BMessage(kMsgDisplay);
		message->AddInt32("index", i);
		BMenuItem* item = new BMenuItem(label.String(), message);
		item->SetTarget(this);
		fDisplayMenu->AddItem(item);
		frames.push_back(fDisplays[i].frame);
		names.push_back(fDisplays[i].name);
	}
	fArrangement->SetDisplays(frames, names, fCurrent);

	bool several = fDisplays.size() > 1;
	fPrimaryBox->SetEnabled(several);
	fMirrorBox->SetEnabled(several);
	fPrimaryBox->SetValue(B_CONTROL_ON);
}


void
DisplaysView::_SelectDisplay(int32 index)
{
	if (index < 0 || index >= (int32)fDisplays.size())
		return;
	fCurrent = index;
	if (BMenuItem* item = fDisplayMenu->ItemAt(index))
		item->SetMarked(true);
	_FillModes();
	_UpdateGpuInfo();
	std::vector<BRect> frames;
	std::vector<BString> names;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		frames.push_back(fDisplays[i].frame);
		names.push_back(fDisplays[i].name);
	}
	fArrangement->SetDisplays(frames, names, fCurrent);
}


void
DisplaysView::_FillModes()
{
	fModes.clear();
	while (fResolutionMenu->CountItems() > 0)
		delete fResolutionMenu->RemoveItem((int32)0);

	BScreen screen(fDisplays[fCurrent].id);
	display_mode* modes = NULL;
	uint32 count = 0;
	display_mode current;
	screen.GetMode(&current);
	if (screen.GetModeList(&modes, &count) == B_OK) {
		for (uint32 i = 0; i < count; i++)
			fModes.push_back(modes[i]);
		free(modes);
	}

	// one entry per size (refresh rates have their own menu)
	std::vector<std::pair<uint16, uint16> > sizes;
	for (size_t i = 0; i < fModes.size(); i++) {
		std::pair<uint16, uint16> size(fModes[i].virtual_width,
			fModes[i].virtual_height);
		bool known = false;
		for (size_t s = 0; s < sizes.size() && !known; s++)
			known = sizes[s] == size;
		if (known)
			continue;
		sizes.push_back(size);
		BString label;
		label << size.first << " × " << size.second;
		BMessage* message = new BMessage(kMsgResolution);
		message->AddInt32("width", size.first);
		message->AddInt32("height", size.second);
		BMenuItem* item = new BMenuItem(label.String(), message);
		item->SetTarget(this);
		if (size.first == current.virtual_width
			&& size.second == current.virtual_height)
			item->SetMarked(true);
		fResolutionMenu->AddItem(item);
	}
	_FillRefreshRates();
}


void
DisplaysView::_FillRefreshRates()
{
	while (fRefreshMenu->CountItems() > 0)
		delete fRefreshMenu->RemoveItem((int32)0);
	BMenuItem* marked = fResolutionMenu->FindMarked();
	if (marked == NULL)
		return;
	int32 width = marked->Message()->GetInt32("width", 0);
	int32 height = marked->Message()->GetInt32("height", 0);
	BScreen screen(fDisplays[fCurrent].id);
	display_mode current;
	screen.GetMode(&current);
	float currentRefresh = mode_refresh(current);

	std::vector<int> rates;
	for (size_t i = 0; i < fModes.size(); i++) {
		if (fModes[i].virtual_width != width
			|| fModes[i].virtual_height != height)
			continue;
		int rate = (int)(mode_refresh(fModes[i]) * 10 + 0.5f);
		bool known = false;
		for (size_t r = 0; r < rates.size() && !known; r++)
			known = rates[r] == rate;
		if (known || rate <= 0)
			continue;
		rates.push_back(rate);
		BString label;
		label.SetToFormat("%.1f Hz", rate / 10.0f);
		BMessage* message = new BMessage(kMsgRefresh);
		message->AddInt32("mode", i);
		BMenuItem* item = new BMenuItem(label.String(), message);
		item->SetTarget(this);
		if (abs(rate - (int)(currentRefresh * 10 + 0.5f)) <= 1)
			item->SetMarked(true);
		fRefreshMenu->AddItem(item);
	}
	if (fRefreshMenu->FindMarked() == NULL && fRefreshMenu->CountItems() > 0)
		fRefreshMenu->ItemAt(0)->SetMarked(true);
}


void
DisplaysView::_UpdateGpuInfo()
{
	BScreen screen(fDisplays[fCurrent].id);
	accelerant_device_info info;
	if (screen.GetDeviceInfo(&info) != B_OK) {
		fGpuName->SetText(B_TRANSLATE("No information from the driver"));
		return;
	}
	BString text;
	text << B_TRANSLATE("Name:") << " " << info.name;
	fGpuName->SetText(text.String());
	text = "";
	text << B_TRANSLATE("Chipset:") << " " << info.chipset;
	fGpuChipset->SetText(text.String());
	text = "";
	text << B_TRANSLATE("Memory:") << " " << info.memory / (1024 * 1024)
		<< " MiB";
	fGpuMemory->SetText(text.String());
	text = "";
	text << B_TRANSLATE("Driver interface version:") << " " << info.version;
	fGpuDriver->SetText(text.String());
}


void
DisplaysView::_Apply()
{
	BMenuItem* refresh = fRefreshMenu->FindMarked();
	if (refresh == NULL)
		return;
	int32 index = refresh->Message()->GetInt32("mode", -1);
	if (index < 0 || index >= (int32)fModes.size())
		return;
	BScreen screen(fDisplays[fCurrent].id);
	screen.SetMode(&fModes[index], false);
	_ScanDisplays();
}


void
DisplaysView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgDisplay:
			_SelectDisplay(message->GetInt32("index", 0));
			break;
		case kMsgResolution:
			_FillRefreshRates();
			break;
		case kMsgRefresh:
			break;
		case kMsgWayland:
			set_wayland_compositor_enabled(
				fWaylandBox->Value() == B_CONTROL_ON);
			break;
		case kMsgWorkspaces:
			set_workspace_count(fWorkspaces->Value());
			break;
		case kMsgApply:
			_Apply();
			break;
		default:
			BView::MessageReceived(message);
	}
}


int32
DisplaysView::CountDisplays() const
{
	return fDisplays.size();
}


int32
DisplaysView::CountModes() const
{
	return fModes.size();
}


bool
DisplaysView::FeatureEnabled(const char* name) const
{
	BView* view = FindView(name);
	BControl* control = dynamic_cast<BControl*>(view);
	if (control != NULL)
		return control->IsEnabled();
	BMenuField* field = dynamic_cast<BMenuField*>(view);
	return field != NULL && field->IsEnabled();
}


const char*
DisplaysView::GpuName() const
{
	return fGpuName->Text();
}


//	#pragma mark - DisplaysWindow


DisplaysWindow::DisplaysWindow()
	:
	BWindow(BRect(80, 80, 600, 700), B_TRANSLATE("Displays"),
		B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS)
{
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(new DisplaysView());
	CenterOnScreen();
}
