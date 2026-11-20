/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "DisplaysView.h"

#include <errno.h>
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

#include <File.h>
#include <Screen.h>
#include <ScreenOutputs.h>

#include "WaylandSwitch.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Displays"


static const uint32 kMsgDisplay = 'dsel';
static const uint32 kMsgResolution = 'dres';
static const uint32 kMsgRefresh = 'dref';
static const uint32 kMsgWayland = 'dwln';
static const uint32 kMsgWorkspaces = 'dwks';
static const uint32 kMsgApply = 'dapl';
static const uint32 kMsgPrimary = 'dpri';
static const uint32 kMsgMirror = 'dmir';
static const uint32 kMsgPosition = 'dpos';
static const uint32 kMsgEnabled = 'denb';

// where a display is, next to the primary one (batch 6)
enum {
	kPositionRight = 0,
	kPositionLeft,
	kPositionAbove,
	kPositionBelow
};

static const char* const kDisplaySettings
	= "/boot/home/config/settings/system/app_server/displays";


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
		new BMessage(kMsgPrimary));
	fMirrorBox = new BCheckBox("mirror", B_TRANSLATE("Mirror the primary "
		"display"), new BMessage(kMsgMirror));
	fEnabledBox = new BCheckBox("enabled", B_TRANSLATE("Use this display"),
		new BMessage(kMsgEnabled));
	fPositionMenu = new BPopUpMenu("position");
	const char* positions[] = { B_TRANSLATE("Right of the primary display"),
		B_TRANSLATE("Left of the primary display"),
		B_TRANSLATE("Above the primary display"),
		B_TRANSLATE("Below the primary display") };
	for (int32 i = 0; i < 4; i++) {
		BMessage* message = new BMessage(kMsgPosition);
		message->AddInt32("position", i);
		fPositionMenu->AddItem(new BMenuItem(positions[i], message));
	}
	fPositionMenu->ItemAt(0)->SetMarked(true);
	fPositionField = new BMenuField("position", B_TRANSLATE("Position:"),
		fPositionMenu);
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
			.AddMenuField(fPositionField, 0, 6)
		.End()
		.Add(_Feature("enabled", fEnabledBox, oneDisplay))
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
	fPrimaryBox->SetTarget(this);
	fMirrorBox->SetTarget(this);
	fEnabledBox->SetTarget(this);
	fPositionMenu->SetTargetForItems(this);

	fWaylandBox->SetValue(wayland_compositor_enabled()
		? B_CONTROL_ON : B_CONTROL_OFF);
	fWorkspaces->SetValue(count_workspaces());

	_ScanDisplays();
	_SelectDisplay(0);
}


/*!	Renga (batch 6): the displays are the screen's outputs
	(BPrivate::get_screen_outputs()): one per connected output of a
	<MultiHead.h> card, or the one screen.
*/
void
DisplaysView::_ScanDisplays()
{
	fDisplays.clear();
	BScreen screen(Window());
	screen_output_info outputs[16];
	int32 count = 0;
	if (BPrivate::get_screen_outputs(outputs, 16, &count) != B_OK
		|| count < 1) {
		count = 1;
		outputs[0] = screen_output_info();
		outputs[0].frame = screen.Frame();
		outputs[0].mirror_of = 0xffffffff;
		outputs[0].enabled = outputs[0].primary = outputs[0].connected
			= true;
	}
	if (count > 16)
		count = 16;
	for (int32 i = 0; i < count; i++) {
		const screen_output_info& output = outputs[i];
		if (!output.connected)
			continue;
		Display display;
		display.id = screen.ID();
		display.frame = output.frame;
		display.output = output.index;
		display.primary = output.primary;
		display.enabled = output.enabled;
		display.mirrorOf = -1;
		display.width = output.width;
		display.height = output.height;
		display.name = output.name;
		monitor_info monitor;
		if (count == 1 && screen.GetMonitorInfo(&monitor) == B_OK
			&& monitor.name[0] != '\0')
			display.name = monitor.name;
		if (display.name.IsEmpty())
			display.name = B_TRANSLATE("Display");
		fDisplays.push_back(display);
	}
	for (size_t i = 0; i < fDisplays.size(); i++) {
		for (int32 j = 0; j < count; j++) {
			if (outputs[j].index != fDisplays[i].output
				|| outputs[j].mirror_of == 0xffffffff)
				continue;
			for (size_t k = 0; k < fDisplays.size(); k++) {
				if (fDisplays[k].output == outputs[j].mirror_of)
					fDisplays[i].mirrorOf = k;
			}
		}
	}
	if (fCurrent >= (int32)fDisplays.size())
		fCurrent = 0;

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
	_UpdateControls();
}


void
DisplaysView::_UpdateControls()
{
	bool several = fDisplays.size() > 1;
	if (fCurrent < 0 || fCurrent >= (int32)fDisplays.size())
		return;
	const Display& display = fDisplays[fCurrent];
	fPrimaryBox->SetEnabled(several && display.enabled);
	fMirrorBox->SetEnabled(several && !display.primary && display.enabled);
	fEnabledBox->SetEnabled(several && !display.primary);
	fPositionField->SetEnabled(several && !display.primary
		&& display.mirrorOf < 0 && display.enabled);
	fPrimaryBox->SetValue(display.primary ? B_CONTROL_ON : B_CONTROL_OFF);
	fMirrorBox->SetValue(display.mirrorOf >= 0 ? B_CONTROL_ON
		: B_CONTROL_OFF);
	fEnabledBox->SetValue(display.enabled ? B_CONTROL_ON : B_CONTROL_OFF);

	// where it is now, next to the primary display
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (!fDisplays[i].primary || (int32)i == fCurrent)
			continue;
		BRect primary = fDisplays[i].frame;
		int32 position = kPositionRight;
		if (display.frame.right < primary.left)
			position = kPositionLeft;
		else if (display.frame.bottom < primary.top)
			position = kPositionAbove;
		else if (display.frame.top > primary.bottom)
			position = kPositionBelow;
		if (BMenuItem* item = fPositionMenu->ItemAt(position))
			item->SetMarked(true);
	}
}


void
DisplaysView::_SelectDisplay(int32 index)
{
	if (index < 0 || index >= (int32)fDisplays.size())
		return;
	fCurrent = index;
	if (BMenuItem* item = fDisplayMenu->ItemAt(index))
		item->SetMarked(true);
	_UpdateControls();
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
	if (fDisplays.size() > 1 && !fDisplays[fCurrent].primary) {
		// Renga (batch 6): mode lists are the primary output's for now;
		// the others show the mode they have
		current.virtual_width = fDisplays[fCurrent].width;
		current.virtual_height = fDisplays[fCurrent].height;
		fModes.push_back(current);
	} else if (screen.GetModeList(&modes, &count) == B_OK) {
		for (uint32 i = 0; i < count; i++)
			fModes.push_back(modes[i]);
		free(modes);
	}
	if (fDisplays.size() > 1 && fDisplays[fCurrent].primary) {
		current.virtual_width = fDisplays[fCurrent].width;
		current.virtual_height = fDisplays[fCurrent].height;
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
	if (fDisplays.size() > 1) {
		// several outputs: the arrangement and the primary output's mode
		// go through the displays settings
		if (fDisplays[fCurrent].primary) {
			fDisplays[fCurrent].width = fModes[index].virtual_width;
			fDisplays[fCurrent].height = fModes[index].virtual_height;
		}
		if (_WriteSettings() == B_OK)
			BPrivate::reload_display_settings();
		_ScanDisplays();
		_SelectDisplay(fCurrent);
		return;
	}
	BScreen screen(fDisplays[fCurrent].id);
	screen.SetMode(&fModes[index], false);
	_ScanDisplays();
}


/*!	Writes the arrangement as app_server reads it (MultiOutput.cpp):
	"output <n> { enabled; primary; position x y; mode w h; mirror m }".
*/
status_t
DisplaysView::_WriteSettings()
{
	BString text("multi_screen true\n");
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const Display& display = fDisplays[i];
		text << "output " << display.output << " {\n";
		text << "\tenabled " << (display.enabled ? "true" : "false") << "\n";
		text << "\tprimary " << (display.primary ? "true" : "false") << "\n";
		if (display.mirrorOf >= 0) {
			text << "\tmirror " << fDisplays[display.mirrorOf].output
				<< "\n";
		} else {
			text << "\tposition " << (int32)display.frame.left << " "
				<< (int32)display.frame.top << "\n";
		}
		if (display.width > 0 && display.height > 0) {
			text << "\tmode " << display.width << " " << display.height
				<< "\n";
		}
		text << "}\n";
	}
	return WriteDisplaySettings(text.String());
}


status_t
DisplaysView::WriteDisplaySettings(const char* text)
{
	BString temporary(kDisplaySettings);
	temporary << ".new";
	BFile file(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;
	ssize_t length = strlen(text);
	if (file.Write(text, length) != length)
		return B_IO_ERROR;
	file.Sync();
	file.Unset();
	if (rename(temporary.String(), kDisplaySettings) != 0)
		return errno;
	return B_OK;
}


/*!	Makes \a primary the primary display, the selected display a mirror
	of \a mirrorOf (-1: none), or places it at \a position next to the
	primary display; then applies it. Also what the controls do.
*/
status_t
DisplaysView::ApplyArrangement(int32 primary, int32 mirrorOf, int32 position)
{
	int32 count = fDisplays.size();
	if (count < 2)
		return B_NOT_ALLOWED;
	if (primary >= 0 && primary < count) {
		for (int32 i = 0; i < count; i++)
			fDisplays[i].primary = i == primary;
		fDisplays[primary].mirrorOf = -1;
		fDisplays[primary].enabled = true;
	}
	Display& display = fDisplays[fCurrent];
	if (!display.primary) {
		display.mirrorOf = mirrorOf >= 0 && mirrorOf < count
			&& mirrorOf != fCurrent ? mirrorOf : -1;
		if (display.mirrorOf < 0 && position >= 0) {
			for (int32 i = 0; i < count; i++) {
				if (!fDisplays[i].primary)
					continue;
				BRect base = fDisplays[i].frame;
				float width = display.width;
				float height = display.height;
				BPoint at;
				switch (position) {
					case kPositionLeft:
						at.Set(base.left - width, base.top);
						break;
					case kPositionAbove:
						at.Set(base.left, base.top - height);
						break;
					case kPositionBelow:
						at.Set(base.left, base.bottom + 1);
						break;
					default:
						at.Set(base.right + 1, base.top);
				}
				display.frame.OffsetTo(at);
			}
		}
	}
	status_t status = _WriteSettings();
	if (status == B_OK)
		status = BPrivate::reload_display_settings();
	_ScanDisplays();
	_SelectDisplay(fCurrent);
	return status;
}


bool
DisplaysView::IsPrimary(int32 index) const
{
	return index >= 0 && index < (int32)fDisplays.size()
		&& fDisplays[index].primary;
}


int32
DisplaysView::MirrorOf(int32 index) const
{
	return index >= 0 && index < (int32)fDisplays.size()
		? fDisplays[index].mirrorOf : -1;
}


BRect
DisplaysView::DisplayFrame(int32 index) const
{
	return index >= 0 && index < (int32)fDisplays.size()
		? fDisplays[index].frame : BRect();
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
		case kMsgPrimary:
			if (fPrimaryBox->Value() == B_CONTROL_ON)
				ApplyArrangement(fCurrent, -1, -1);
			else
				_UpdateControls();
			break;
		case kMsgMirror:
		{
			int32 primary = -1;
			for (size_t i = 0; i < fDisplays.size(); i++) {
				if (fDisplays[i].primary)
					primary = i;
			}
			ApplyArrangement(-1, fMirrorBox->Value() == B_CONTROL_ON
				? primary : -1, fPositionMenu->IndexOf(
					fPositionMenu->FindMarked()));
			break;
		}
		case kMsgPosition:
			ApplyArrangement(-1, -1, message->GetInt32("position", 0));
			break;
		case kMsgEnabled:
			if (fCurrent >= 0 && fCurrent < (int32)fDisplays.size()) {
				fDisplays[fCurrent].enabled
					= fEnabledBox->Value() == B_CONTROL_ON;
				ApplyArrangement(-1, fDisplays[fCurrent].mirrorOf,
					-1);
			}
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
