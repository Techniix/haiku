/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WindowsPage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <AppServerLink.h>
#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Deskbar.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>

#include <ServerProtocol.h>
#include <window_layouts.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ControlPanel"


using WindowLayouts::Settings;


static const uint32 kMsgChanged = 'wpCh';
static const uint32 kMsgDefaultSpec = 'wpDf';
static const uint32 kMsgWorkspaceSpec = 'wpWs';
static const uint32 kMsgDisplaySpec = 'wpDs';
static const uint32 kMsgEditorGrid = 'wpEg';
static const uint32 kMsgEditorClear = 'wpEc';
static const uint32 kMsgEditorSave = 'wpEs';
static const uint32 kMsgAddRule = 'wpAr';
static const uint32 kMsgRemoveRule = 'wpRr';
static const uint32 kMsgSaveArrangement = 'wpSa';
static const uint32 kMsgRestoreArrangement = 'wpRa';
static const uint32 kMsgAddWorkspace = 'wpAw';
static const uint32 kMsgRemoveWorkspace = 'wpRw';
static const uint32 kMsgWorkspaceName = 'wpWn';
static const uint32 kMsgSwitcher = 'wpSw';
static const uint32 kMsgOverview = 'wpOv';
static const uint32 kMsgWallpaper = 'wpWp';
static const uint32 kMsgSaveSet = 'wpSs';
static const uint32 kMsgLoadSet = 'wpLs';

static const char* kPresets[] = { "1x2", "1x3", "2x2", "2x3", "3x3", "1x4",
	"2x4", "3x2" };
static const char* kDeskbarItem = "workspaces";
static const char* kWorkspacesApp = "/boot/system/apps/Workspaces";


/*!	The zone editor: a grid of rows x columns cells on a picture of the
	screen; dragging over cells makes them one zone. Zones may not
	overlap: a new zone replaces those it covers.
*/
class ZoneEditor : public BView {
public:
	ZoneEditor()
		:
		BView("zoneEditor", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fRows(2),
		fColumns(3),
		fDragging(false)
	{
		SetExplicitMinSize(BSize(240, 135));
		SetExplicitPreferredSize(BSize(320, 180));
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	}

	void SetGrid(int32 rows, int32 columns)
	{
		fRows = rows;
		fColumns = columns;
		fZones.clear();
		Invalidate();
	}

	void Clear()
	{
		fZones.clear();
		Invalidate();
	}

	const std::vector<WindowLayouts::Zone>& Zones() const { return fZones; }

	virtual void Draw(BRect updateRect)
	{
		BRect screen = _Screen();
		SetHighUIColor(B_DOCUMENT_BACKGROUND_COLOR);
		FillRect(screen);
		SetHighUIColor(B_CONTROL_BORDER_COLOR);
		for (int32 i = 1; i < fColumns; i++) {
			float x = screen.left + screen.Width() * i / fColumns;
			StrokeLine(BPoint(x, screen.top), BPoint(x, screen.bottom),
				B_MIXED_COLORS);
		}
		for (int32 i = 1; i < fRows; i++) {
			float y = screen.top + screen.Height() * i / fRows;
			StrokeLine(BPoint(screen.left, y), BPoint(screen.right, y),
				B_MIXED_COLORS);
		}
		for (size_t i = 0; i < fZones.size(); i++)
			_DrawZone(fZones[i], i + 1, false);
		if (fDragging)
			_DrawZone(_DragZone(), 0, true);
		SetHighUIColor(B_PANEL_TEXT_COLOR);
		StrokeRect(screen);
	}

	virtual void MouseDown(BPoint where)
	{
		if (!_Screen().Contains(where))
			return;
		fDragging = true;
		fStart = fEnd = _Cell(where);
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Invalidate();
	}

	virtual void MouseMoved(BPoint where, uint32 transit,
		const BMessage* dragMessage)
	{
		if (!fDragging)
			return;
		BPoint cell = _Cell(where);
		if (cell != fEnd) {
			fEnd = cell;
			Invalidate();
		}
	}

	virtual void MouseUp(BPoint where)
	{
		if (!fDragging)
			return;
		fDragging = false;
		WindowLayouts::Zone zone = _DragZone();
		for (size_t i = 0; i < fZones.size();) {
			const WindowLayouts::Zone& other = fZones[i];
			if (other.left < zone.right - 0.001f
				&& other.right > zone.left + 0.001f
				&& other.top < zone.bottom - 0.001f
				&& other.bottom > zone.top + 0.001f)
				fZones.erase(fZones.begin() + i);
			else
				i++;
		}
		if (fZones.size() < WindowLayouts::kMaxZones)
			fZones.push_back(zone);
		Invalidate();
	}

private:
	BRect _Screen() const
	{
		BRect bounds = Bounds().InsetByCopy(2, 2);
		BScreen screen;
		float aspect = screen.Frame().Width() / screen.Frame().Height();
		if (!(aspect > 0.2f && aspect < 5))
			aspect = 16.0f / 9;
		float width = bounds.Width();
		float height = width / aspect;
		if (height > bounds.Height()) {
			height = bounds.Height();
			width = height * aspect;
		}
		return BRect(bounds.left, bounds.top, bounds.left + width,
			bounds.top + height);
	}

	BPoint _Cell(BPoint where) const
	{
		BRect screen = _Screen();
		int32 column = (int32)((where.x - screen.left) * fColumns
			/ (screen.Width() + 1));
		int32 row = (int32)((where.y - screen.top) * fRows
			/ (screen.Height() + 1));
		column = std::max((int32)0, std::min(column, fColumns - 1));
		row = std::max((int32)0, std::min(row, fRows - 1));
		return BPoint(column, row);
	}

	WindowLayouts::Zone _DragZone() const
	{
		WindowLayouts::Zone zone;
		zone.left = std::min(fStart.x, fEnd.x) / fColumns;
		zone.right = (std::max(fStart.x, fEnd.x) + 1) / fColumns;
		zone.top = std::min(fStart.y, fEnd.y) / fRows;
		zone.bottom = (std::max(fStart.y, fEnd.y) + 1) / fRows;
		return zone;
	}

	void _DrawZone(const WindowLayouts::Zone& zone, int32 number,
		bool dragging)
	{
		BRect screen = _Screen();
		BRect frame(screen.left + zone.left * screen.Width() + 2,
			screen.top + zone.top * screen.Height() + 2,
			screen.left + zone.right * screen.Width() - 2,
			screen.top + zone.bottom * screen.Height() - 2);
		rgb_color color = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
		if (dragging)
			color.alpha = 140;
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(color);
		FillRect(frame);
		SetDrawingMode(B_OP_COPY);
		SetHighUIColor(B_PANEL_TEXT_COLOR);
		StrokeRect(frame);
		if (number > 0) {
			char label[8];
			snprintf(label, sizeof(label), "%d", (int)number);
			DrawString(label, BPoint(frame.left + 4, frame.top + 14));
		}
	}

	int32							fRows;
	int32							fColumns;
	bool							fDragging;
	BPoint							fStart;
	BPoint							fEnd;
	std::vector<WindowLayouts::Zone> fZones;
};


class WindowsPage : public BGroupView {
public:
	WindowsPage()
		:
		BGroupView("workspaces", B_VERTICAL)
	{
		fSnap = _Check("snap", B_TRANSLATE("Snap layouts: hold a key while "
			"dragging a window to put it into a zone"));
		fModifier = _Menu("modifier", B_TRANSLATE("Key:"), NULL, 0);
		const char* modifiers[] = { "shift", "control", "option", "command" };
		const char* labels[] = { B_TRANSLATE("Shift"), B_TRANSLATE("Control"),
			B_TRANSLATE("Option"), B_TRANSLATE("Command") };
		for (int i = 0; i < 4; i++) {
			BMessage* message = new BMessage(kMsgChanged);
			message->AddString("modifier", modifiers[i]);
			fModifier->Menu()->AddItem(new BMenuItem(labels[i], message));
		}
		fSpan = _Check("span", B_TRANSLATE("Hold Ctrl as well to span "
			"several zones"));
		fGrid = _Check("showGrid", B_TRANSLATE("Show every zone while "
			"choosing"));
		fGap = new BSlider("gap", B_TRANSLATE("Gap between windows"),
			new BMessage(kMsgChanged), 0, 32, B_HORIZONTAL);
		fGap->SetHashMarks(B_HASH_MARKS_BOTTOM);
		fGap->SetHashMarkCount(5);
		fGap->SetLimitLabels("0", "32");
		fGap->SetModificationMessage(NULL);
		fDefault = _Menu("defaultLayout", B_TRANSLATE("Layout:"),
			NULL, kMsgDefaultSpec);
		fWorkspaceLayout = _Menu("workspaceLayout",
			B_TRANSLATE("This workspace:"), B_TRANSLATE("Same as the screen"),
			kMsgWorkspaceSpec);
		fDisplayLayout = _Menu("displayLayout", B_TRANSLATE("This screen:"),
			B_TRANSLATE("Same as the layout"), kMsgDisplaySpec);

		fEditor = new ZoneEditor();
		fEditorGrid = _Menu("editorGrid", B_TRANSLATE("Editor grid:"), NULL,
			0);
		const char* grids[] = { "2x2", "2x3", "2x4", "3x3", "3x4", "4x4",
			"6x6" };
		for (int i = 0; i < 7; i++) {
			BMessage* message = new BMessage(kMsgEditorGrid);
			message->AddString("grid", grids[i]);
			BMenuItem* item = new BMenuItem(grids[i], message);
			item->SetMarked(i == 1);
			fEditorGrid->Menu()->AddItem(item);
		}
		fLayoutName = new BTextControl("layoutName",
			B_TRANSLATE("Name:"), "", NULL);

		fShortcuts = _Check("zoneShortcuts", B_TRANSLATE(
			"Command+Control+1 to 9 put the active window into a zone, +0 "
			"restores it; Command+Control+Up shows the overview"));
		fTabMenu = _Check("tabMenu", B_TRANSLATE("Right-click on a window's "
			"tab opens the window menu (Move to workspace, Snap to)"));
		fRules = _Check("rules", B_TRANSLATE("Per-application rules: open "
			"an application's windows in a zone"));
		fRuleList = new BListView("ruleList");
		fRuleSignature = new BTextControl("ruleSignature",
			B_TRANSLATE("Application signature:"), "", NULL);
		fRuleZone = new BTextControl("ruleZone", B_TRANSLATE("Zone:"), "1",
			NULL);
		fArrangement = new BTextControl("arrangement",
			B_TRANSLATE("Arrangement:"), "default", NULL);
		fArrangementStatus = new BStringView("arrangementStatus", "");

		fCount = new BStringView("workspaceCount", "");
		fName = new BTextControl("workspaceName",
			B_TRANSLATE("Name of this workspace:"), "",
			new BMessage(kMsgWorkspaceName));
		fSwitcher = new BCheckBox("switcher", B_TRANSLATE(
			"Show the workspace switcher in the Deskbar"),
			new BMessage(kMsgSwitcher));
		fPlus = _Check("switcherPlus", B_TRANSLATE("End the switcher with a "
			"\"+\" that adds a workspace"));
		fSetName = new BTextControl("setName", B_TRANSLATE("Workspace set:"),
			"", NULL);
		fSetStatus = new BStringView("setStatus", "");

		BScrollView* ruleScroll = new BScrollView("ruleScroll", fRuleList, 0,
			false, true);
		ruleScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 60));

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(_Heading(B_TRANSLATE("Windows and workspaces"),
				B_TRANSLATE("Snap layouts, the window menu and workspaces.")))
			.Add(new BSeparatorView(B_HORIZONTAL))
			.Add(fSnap)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
				.AddGroup(B_HORIZONTAL)
					.Add(fModifier)
					.Add(fDefault)
					.AddGlue()
				.End()
				.AddGroup(B_HORIZONTAL)
					.Add(fWorkspaceLayout)
					.Add(fDisplayLayout)
					.AddGlue()
				.End()
				.Add(fSpan)
				.Add(fGrid)
				.Add(fGap)
				.Add(new BStringView("editorTitle", B_TRANSLATE(
					"Zone editor: drag over cells to make a zone")))
				.AddGroup(B_HORIZONTAL)
					.Add(fEditor)
					.AddGroup(B_VERTICAL)
						.Add(fEditorGrid)
						.Add(fLayoutName)
						.AddGroup(B_HORIZONTAL)
							.Add(new BButton("editorClear",
								B_TRANSLATE("Clear"),
								new BMessage(kMsgEditorClear)))
							.Add(new BButton("editorSave",
								B_TRANSLATE("Save layout"),
								new BMessage(kMsgEditorSave)))
						.End()
						.AddGlue()
					.End()
				.End()
			.End()
			.Add(fShortcuts)
			.Add(fTabMenu)
			.Add(fRules)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
				.Add(ruleScroll)
				.AddGroup(B_HORIZONTAL)
					.Add(fRuleSignature)
					.Add(fRuleZone)
					.Add(new BButton("addRule", B_TRANSLATE("Add"),
						new BMessage(kMsgAddRule)))
					.Add(new BButton("removeRule", B_TRANSLATE("Remove"),
						new BMessage(kMsgRemoveRule)))
				.End()
			.End()
			.AddGroup(B_HORIZONTAL)
				.Add(fArrangement)
				.Add(new BButton("saveArrangement", B_TRANSLATE(
					"Save window arrangement"),
					new BMessage(kMsgSaveArrangement)))
				.Add(new BButton("restoreArrangement", B_TRANSLATE("Restore"),
					new BMessage(kMsgRestoreArrangement)))
				.Add(fArrangementStatus)
				.AddGlue()
			.End()
			.Add(new BSeparatorView(B_HORIZONTAL))
			.AddGroup(B_HORIZONTAL)
				.Add(fCount)
				.Add(new BButton("addWorkspace", B_TRANSLATE("Add workspace"),
					new BMessage(kMsgAddWorkspace)))
				.Add(new BButton("removeWorkspace", B_TRANSLATE(
					"Remove last"), new BMessage(kMsgRemoveWorkspace)))
				.Add(new BButton("overview", B_TRANSLATE("Overview"),
					new BMessage(kMsgOverview)))
				.AddGlue()
			.End()
			.AddGroup(B_HORIZONTAL)
				.Add(fName)
				.Add(new BButton("wallpaper", B_TRANSLATE("Wallpaper"
					B_UTF8_ELLIPSIS), new BMessage(kMsgWallpaper)))
			.End()
			.Add(fSwitcher)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
				.Add(fPlus)
			.End()
			.AddGroup(B_HORIZONTAL)
				.Add(fSetName)
				.Add(new BButton("saveSet", B_TRANSLATE("Save set"),
					new BMessage(kMsgSaveSet)))
				.Add(new BButton("loadSet", B_TRANSLATE("Load set"),
					new BMessage(kMsgLoadSet)))
				.Add(fSetStatus)
				.AddGlue()
			.End()
			.AddGlue();
	}

	virtual void AttachedToWindow()
	{
		BGroupView::AttachedToWindow();
		_SetTargets(this);
		_Show();
	}

	virtual void WindowActivated(bool active)
	{
		if (active)
			_Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		Settings settings;
		WindowLayouts::load(settings);
		bool save = true;
		switch (message->what) {
			case kMsgChanged:
			{
				const char* modifier;
				if (message->FindString("modifier", &modifier) == B_OK)
					settings.modifier = WindowLayouts::parse_modifier(modifier);
				settings.snap = fSnap->Value() != 0;
				settings.span = fSpan->Value() != 0;
				settings.showGrid = fGrid->Value() != 0;
				settings.gap = fGap->Value();
				settings.zoneShortcuts = fShortcuts->Value() != 0;
				settings.tabMenu = fTabMenu->Value() != 0;
				settings.rules = fRules->Value() != 0;
				bool plus = fPlus->Value() != 0;
				bool plusChanged = plus != settings.switcherPlus;
				settings.switcherPlus = plus;
				if (plusChanged) {
					WindowLayouts::save(settings);
					save = false;
					_ReaddSwitcher();
				}
				break;
			}
			case kMsgDefaultSpec:
			case kMsgWorkspaceSpec:
			case kMsgDisplaySpec:
			{
				const char* spec = message->GetString("spec", "");
				if (message->what == kMsgDefaultSpec)
					strlcpy(settings.defaultSpec, spec,
						sizeof(settings.defaultSpec));
				else if (message->what == kMsgWorkspaceSpec) {
					int32 index = current_workspace();
					if (index < WindowLayouts::kMaxWorkspaces)
						strlcpy(settings.workspaceSpec[index], spec,
							sizeof(settings.workspaceSpec[index]));
				} else
					strlcpy(settings.displaySpec[0], spec,
						sizeof(settings.displaySpec[0]));
				break;
			}
			case kMsgEditorGrid:
			{
				int32 rows, columns;
				if (WindowLayouts::parse_grid(message->GetString("grid", ""),
						rows, columns))
					fEditor->SetGrid(rows, columns);
				save = false;
				break;
			}
			case kMsgEditorClear:
				fEditor->Clear();
				save = false;
				break;
			case kMsgEditorSave:
			{
				BString name(fLayoutName->Text());
				name.ReplaceAll(' ', '_');
				int32 rows, columns;
				if (name.IsEmpty() || fEditor->Zones().empty()
					|| WindowLayouts::parse_grid(name.String(), rows,
						columns)) {
					save = false;
					break;
				}
				int32 index = 0;
				while (index < settings.layoutCount
					&& name != settings.layouts[index].name)
					index++;
				if (index >= WindowLayouts::kMaxLayouts) {
					save = false;
					break;
				}
				if (index == settings.layoutCount)
					settings.layoutCount++;
				WindowLayouts::Layout& layout = settings.layouts[index];
				strlcpy(layout.name, name.String(), sizeof(layout.name));
				layout.count = fEditor->Zones().size();
				for (int32 i = 0; i < layout.count; i++)
					layout.zones[i] = fEditor->Zones()[i];
				strlcpy(settings.defaultSpec, layout.name,
					sizeof(settings.defaultSpec));
				break;
			}
			case kMsgAddRule:
			{
				int32 zone = atoi(fRuleZone->Text());
				const char* signature = fRuleSignature->Text();
				if (signature[0] == '\0' || strchr(signature, ' ') != NULL
					|| zone < 1 || zone > WindowLayouts::kMaxZones
					|| settings.ruleCount >= WindowLayouts::kMaxRules) {
					save = false;
					break;
				}
				WindowLayouts::Rule& rule
					= settings.ruleList[settings.ruleCount++];
				strlcpy(rule.signature, signature, sizeof(rule.signature));
				rule.zone = zone;
				fRuleSignature->SetText("");
				break;
			}
			case kMsgRemoveRule:
			{
				int32 index = fRuleList->CurrentSelection();
				if (index < 0 || index >= settings.ruleCount) {
					save = false;
					break;
				}
				memmove(&settings.ruleList[index],
					&settings.ruleList[index + 1],
					(settings.ruleCount - index - 1)
						* sizeof(WindowLayouts::Rule));
				settings.ruleCount--;
				break;
			}
			case kMsgSaveArrangement:
			case kMsgRestoreArrangement:
			{
				save = false;
				BPrivate::AppServerLink link;
				link.StartMessage(AS_WINDOW_ARRANGEMENT);
				link.Attach<int32>(
					message->what == kMsgSaveArrangement ? 0 : 1);
				link.AttachString(fArrangement->Text());
				int32 code;
				int32 restored = 0;
				if (link.FlushWithReply(code) == B_OK)
					link.Read<int32>(&restored);
				else
					code = B_ERROR;
				BString text;
				if (code != B_OK)
					text = B_TRANSLATE("Failed");
				else if (message->what == kMsgSaveArrangement)
					text = B_TRANSLATE("Saved");
				else
					text.SetToFormat(B_TRANSLATE("%" B_PRId32 " windows "
						"restored"), restored);
				fArrangementStatus->SetText(text.String());
				break;
			}
			case kMsgAddWorkspace:
				save = false;
				if (count_workspaces() < 32)
					set_workspace_count(count_workspaces() + 1);
				break;
			case kMsgRemoveWorkspace:
				save = false;
				if (count_workspaces() > 1)
					set_workspace_count(count_workspaces() - 1);
				break;
			case kMsgWorkspaceName:
			{
				int32 index = current_workspace();
				BString name(fName->Text());
				name.ReplaceAll('\n', ' ');
				if (index < WindowLayouts::kMaxWorkspaces)
					strlcpy(settings.workspaceName[index], name.String(),
						sizeof(settings.workspaceName[index]));
				break;
			}
			case kMsgSwitcher:
			{
				save = false;
				BDeskbar deskbar;
				if (fSwitcher->Value() != 0) {
					BEntry entry(kWorkspacesApp);
					entry_ref ref;
					if (!deskbar.HasItem(kDeskbarItem)
						&& entry.GetRef(&ref) == B_OK)
						deskbar.AddItem(&ref);
				} else
					deskbar.RemoveItem(kDeskbarItem);
				break;
			}
			case kMsgOverview:
			{
				save = false;
				const char* args[] = { "--overview", NULL };
				be_roster->Launch("application/x-vnd.Be-WORK", 1, args);
				break;
			}
			case kMsgWallpaper:
				save = false;
				be_roster->Launch("application/x-vnd.Haiku-Backgrounds");
				break;
			case kMsgSaveSet:
			case kMsgLoadSet:
			{
				save = false;
				bool ok;
				if (message->what == kMsgSaveSet) {
					ok = WindowLayouts::save_set(fSetName->Text(), settings,
						count_workspaces());
				} else {
					int32 count = WindowLayouts::load_set(fSetName->Text(),
						settings);
					ok = count > 0 && WindowLayouts::save(settings);
					if (ok)
						set_workspace_count(count);
				}
				fSetStatus->SetText(ok ? (message->what == kMsgSaveSet
					? B_TRANSLATE("Saved") : B_TRANSLATE("Loaded"))
					: B_TRANSLATE("Failed"));
				break;
			}
			default:
				BGroupView::MessageReceived(message);
				return;
		}
		if (save)
			WindowLayouts::save(settings);
		_Show();
	}

private:
	BCheckBox* _Check(const char* name, const char* label)
	{
		return new BCheckBox(name, label, new BMessage(kMsgChanged));
	}

	BMenuField* _Menu(const char* name, const char* label,
		const char* inherit, uint32 what)
	{
		BPopUpMenu* menu = new BPopUpMenu(name);
		BMenuField* field = new BMenuField(name, label, menu);
		if (what != 0)
			field->SetExplicitMaxSize(BSize(260, B_SIZE_UNSET));
		return field;
	}

	BView* _Heading(const char* title, const char* text)
	{
		BStringView* heading = new BStringView("heading", title);
		BFont font(be_bold_font);
		font.SetSize(font.Size() * 1.3f);
		heading->SetFont(&font);
		BGroupView* group = new BGroupView(B_VERTICAL, 0);
		group->AddChild(heading);
		group->AddChild(new BStringView("description", text));
		return group;
	}

	void _FillSpecMenu(BMenuField* field, uint32 what, const char* inherit,
		const char* current, const Settings& settings)
	{
		BMenu* menu = field->Menu();
		while (menu->CountItems() > 0)
			delete menu->RemoveItem((int32)0);
		if (inherit != NULL) {
			BMessage* message = new BMessage(what);
			message->AddString("spec", "");
			BMenuItem* item = new BMenuItem(inherit, message);
			item->SetMarked(current[0] == '\0');
			menu->AddItem(item);
			menu->AddSeparatorItem();
		}
		for (size_t i = 0; i < sizeof(kPresets) / sizeof(kPresets[0]); i++) {
			int32 rows = 1;
			int32 columns = 2;
			WindowLayouts::parse_grid(kPresets[i], rows, columns);
			BString label;
			label.SetToFormat(B_TRANSLATE("%" B_PRId32 " rows, %" B_PRId32
				" columns"), rows, columns);
			if (rows == 1)
				label.SetToFormat(B_TRANSLATE("%" B_PRId32 " columns"),
					columns);
			BMessage* message = new BMessage(what);
			message->AddString("spec", kPresets[i]);
			BMenuItem* item = new BMenuItem(label.String(), message);
			item->SetMarked(strcmp(current, kPresets[i]) == 0);
			menu->AddItem(item);
		}
		if (settings.layoutCount > 0)
			menu->AddSeparatorItem();
		for (int32 i = 0; i < settings.layoutCount; i++) {
			BMessage* message = new BMessage(what);
			message->AddString("spec", settings.layouts[i].name);
			BMenuItem* item = new BMenuItem(settings.layouts[i].name,
				message);
			item->SetMarked(strcmp(current, settings.layouts[i].name) == 0);
			menu->AddItem(item);
		}
		menu->SetTargetForItems(this);
		if (menu->FindMarked() == NULL && current[0] != '\0') {
			// a spec typed into the file by hand
			BMessage* message = new BMessage(what);
			message->AddString("spec", current);
			BMenuItem* item = new BMenuItem(current, message);
			item->SetMarked(true);
			item->SetTarget(this);
			menu->AddItem(item);
		}
	}

	void _SetTargets(BView* view)
	{
		for (int32 i = 0; i < view->CountChildren(); i++) {
			BView* child = view->ChildAt(i);
			BControl* control = dynamic_cast<BControl*>(child);
			if (control != NULL)
				control->SetTarget(this);
			_SetTargets(child);
		}
		fModifier->Menu()->SetTargetForItems(this);
		fEditorGrid->Menu()->SetTargetForItems(this);
	}

	void _ReaddSwitcher()
	{
		BDeskbar deskbar;
		if (!deskbar.HasItem(kDeskbarItem))
			return;
		deskbar.RemoveItem(kDeskbarItem);
		BEntry entry(kWorkspacesApp);
		entry_ref ref;
		if (entry.GetRef(&ref) == B_OK)
			deskbar.AddItem(&ref);
	}

	void _Show()
	{
		Settings settings;
		WindowLayouts::load(settings);
		fSnap->SetValue(settings.snap);
		const char* modifier = WindowLayouts::modifier_name(settings.modifier);
		BMenu* menu = fModifier->Menu();
		for (int32 i = 0; i < menu->CountItems(); i++) {
			BMenuItem* item = menu->ItemAt(i);
			item->SetMarked(strcmp(item->Message()->GetString("modifier", ""),
				modifier) == 0);
		}
		fSpan->SetValue(settings.span);
		fSpan->SetLabel(settings.modifier == B_CONTROL_KEY
				|| settings.modifier == B_COMMAND_KEY
			? B_TRANSLATE("Hold Shift as well to span several zones")
			: B_TRANSLATE("Hold Ctrl as well to span several zones"));
		fGrid->SetValue(settings.showGrid);
		fGap->SetValue(settings.gap);
		int32 workspace = current_workspace();
		_FillSpecMenu(fDefault, kMsgDefaultSpec, NULL, settings.defaultSpec,
			settings);
		_FillSpecMenu(fWorkspaceLayout, kMsgWorkspaceSpec,
			B_TRANSLATE("Same as the screen"),
			workspace < WindowLayouts::kMaxWorkspaces
				? settings.workspaceSpec[workspace] : "", settings);
		_FillSpecMenu(fDisplayLayout, kMsgDisplaySpec,
			B_TRANSLATE("Same as the layout"), settings.displaySpec[0],
			settings);
		BControl* snapParts[] = { fSpan, fGrid, fGap };
		for (int i = 0; i < 3; i++)
			snapParts[i]->SetEnabled(settings.snap);
		fModifier->SetEnabled(settings.snap);
		fShortcuts->SetValue(settings.zoneShortcuts);
		fTabMenu->SetValue(settings.tabMenu);
		fRules->SetValue(settings.rules);
		fRuleList->MakeEmpty();
		for (int32 i = 0; i < settings.ruleCount; i++) {
			BString text;
			text.SetToFormat(B_TRANSLATE("%s: zone %" B_PRId32),
				settings.ruleList[i].signature, settings.ruleList[i].zone);
			fRuleList->AddItem(new BStringItem(text.String()));
		}
		BString count;
		count.SetToFormat(B_TRANSLATE("Workspaces: %" B_PRId32),
			count_workspaces());
		fCount->SetText(count.String());
		if (!fName->TextView()->IsFocus()) {
			fName->SetText(workspace < WindowLayouts::kMaxWorkspaces
				? settings.workspaceName[workspace] : "");
		}
		BDeskbar deskbar;
		fSwitcher->SetValue(deskbar.HasItem(kDeskbarItem));
		fPlus->SetValue(settings.switcherPlus);
	}

	BCheckBox*		fSnap;
	BMenuField*		fModifier;
	BCheckBox*		fSpan;
	BCheckBox*		fGrid;
	BSlider*		fGap;
	BMenuField*		fDefault;
	BMenuField*		fWorkspaceLayout;
	BMenuField*		fDisplayLayout;
	ZoneEditor*		fEditor;
	BMenuField*		fEditorGrid;
	BTextControl*	fLayoutName;
	BCheckBox*		fShortcuts;
	BCheckBox*		fTabMenu;
	BCheckBox*		fRules;
	BListView*		fRuleList;
	BTextControl*	fRuleSignature;
	BTextControl*	fRuleZone;
	BTextControl*	fArrangement;
	BStringView*	fArrangementStatus;
	BStringView*	fCount;
	BTextControl*	fName;
	BCheckBox*		fSwitcher;
	BCheckBox*		fPlus;
	BTextControl*	fSetName;
	BStringView*	fSetStatus;
};


BView*
create_windows_page()
{
	return new WindowsPage();
}
