/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Welcome (Renga, batch 5; roadmap "First-boot welcome"): a short tour on
	the first login of an installed system, in pages, every one skippable:

	  welcome     what Renga is, and that this can be opened again
	  network     Wi-Fi and the network: the interfaces found, Network
	  graphics    NVIDIA graphics: whether Quasar is installed, and the
	              button to install it from the Renga repository; devices
	              a Linux driver supports that miss firmware (online driver
	              support through the Devices and drivers page)
	  software    the Renga repository: whether it is configured, and the
	              button to add it
	  linux       Linux programs: whether a Linux root is installed, and
	              the button that downloads one ("linux --setup")
	  gamepads    test a gamepad (the Gamepads preferences)
	  setup       the shortcut key (Ctrl as in Windows and Linux, or Alt
	              as in Haiku), applied at once
	  tips        workspaces and where help lives

	Every action is a button the user presses; nothing is installed or
	downloaded without it. The welcome shows at login until it is finished
	or closed with "Don't show again" (~/config/settings/Renga/welcome,
	"done true"); the launch job (data/launch/user_welcome) checks that file
	and stays off on read-only media, where the installation medium's own
	prompt is shown. It can be opened again from the Deskbar (Applications)
	and the Control Panel.

	  Welcome                    the window
	  Welcome --page <id>        the window, on that page
	  Welcome --status           what it finds, one "key value" per line
*/


#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CardLayout.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Path.h>
#include <RadioButton.h>
#include <Roster.h>
#include <SeparatorView.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextView.h>
#include <Url.h>
#include <Window.h>

#include <ScreenFit.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <ShortcutKeyStyle.h>

#include "Keymap.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Welcome"


static const char* kSignature = "application/x-vnd.Renga-Welcome";
static const char* kLinuxRootShell
	= "/boot/system/non-packaged/compat/linux/bin/sh";
static const char* kRenguinModule
	= "/boot/system/add-ons/kernel/personalities/renguin";
static const char* kRengaRepository
	= "https://techniix.github.io/RengaPorts/x86_64/current";

static const uint32 kMsgNext = 'next';
static const uint32 kMsgBack = 'back';
static const uint32 kMsgFinish = 'fini';
static const uint32 kMsgAction = 'actn';
static const uint32 kMsgShortcutCtrl = 'scCt';
static const uint32 kMsgShortcutAlt = 'scAl';
static const uint32 kMsgQuasarChecked = 'qsCk';
static const uint32 kMsgOpenQuasarInstructions = 'qsBi';
static const char* kQuasarInstructions
	= "https://github.com/Techniix/nvidia-haiku";
// from other programs (tests): "id" -> replies "page"
static const uint32 kMsgShowPage = 'wlPg';
// harness: the shown page's paragraphs, text height against view height
static const uint32 kMsgParagraphFit = 'wlFt';

static bool sAssumeNvidia = false;
	// "--test-assume-nvidia" (welcome_ui_test)

static const char* kPageIDs[] = { "welcome", "network", "graphics",
	"software", "linux", "gamepads", "setup", "tips" };
static const int32 kPageCount = 8;


// #pragma mark - what is found


static std::string
command_output(const char* command)
{
	std::string output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[1024];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, length);
	pclose(pipe);
	return output;
}


struct Findings {
	int32		interfaces;		// network interfaces other than loopback
	int32		wireless;
	int32		connected;		// with an address
	bool		nvidia;			// an NVIDIA display controller
	bool		quasar;			// Quasar's accelerant is installed
	bool		rengaRepository;
	bool		renguin;		// the Linux personality is there
	bool		linuxRoot;
	int32		missingFirmware;	// devices a Linux driver supports
};


static bool
any_file_starting(const char* directory, const char* prefix)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return false;
	bool found = false;
	while (dirent* entry = readdir(dir)) {
		if (strncasecmp(entry->d_name, prefix, strlen(prefix)) == 0)
			found = true;
	}
	closedir(dir);
	return found;
}


static Findings
find_things()
{
	Findings findings = {};

	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_LOOPBACK) != 0)
			continue;
		findings.interfaces++;
		BNetworkDevice device(interface.Name());
		if (device.IsWireless())
			findings.wireless++;
		BNetworkInterfaceAddress address;
		if (interface.CountAddresses() > 0
			&& interface.GetAddressAt(0, address) == B_OK
			&& !address.Address().IsEmpty())
			findings.connected++;
	}

	// NVIDIA: a display controller (class 3) of vendor 10de, from listdev
	std::string devices = command_output("/bin/listdev 2>/dev/null");
	size_t position = 0;
	while ((position = devices.find("vendor 10de", position))
			!= std::string::npos) {
		size_t start = devices.rfind("device ", position);
		std::string block = devices.substr(start == std::string::npos ? 0
			: start, position - (start == std::string::npos ? 0 : start));
		// "device <class name> [<base>|<sub>|<interface>]": base 3
		if (block.find("[3|") != std::string::npos)
			findings.nvidia = true;
		position++;
	}
	// Quasar comes as a package; Haiku's own nvidia.accelerant (for old
	// cards) does not count
	findings.quasar = any_file_starting("/boot/system/packages", "quasar")
		|| any_file_starting("/boot/home/config/packages", "quasar");

	findings.rengaRepository = any_file_starting(
		"/boot/system/settings/package-repositories", "Renga")
		|| any_file_starting("/boot/home/config/settings/package-repositories",
			"Renga");

	struct stat st;
	findings.renguin = stat(kRenguinModule, &st) == 0;
	findings.linuxRoot = stat(kLinuxRootShell, &st) == 0;

	std::string linuxkpi = command_output("/bin/lkpictl hardware 2>/dev/null");
	position = 0;
	while ((position = linuxkpi.find("missing firmware", position))
			!= std::string::npos) {
		findings.missingFirmware++;
		position++;
	}
	return findings;
}


/*!	Whether a configured repository offers the "quasar" package, from the
	repositories' cached data (pkgman search -a, no download). Runs a
	program, so the window asks it in a thread.
*/
static bool
quasar_available()
{
	std::string output = command_output(
		"/bin/pkgman search -a quasar 2>/dev/null");
	// one row per package: "<status>  <name>  <summary>"; the name column
	// must be exactly "quasar"
	size_t start = 0;
	while (start < output.size()) {
		size_t end = output.find('\n', start);
		if (end == std::string::npos)
			end = output.size();
		std::string line = output.substr(start, end - start);
		start = end + 1;
		size_t word = 0;
		std::vector<std::string> words;
		while (word < line.size()) {
			while (word < line.size() && (line[word] == ' '
					|| line[word] == '\t'))
				word++;
			size_t wordEnd = word;
			while (wordEnd < line.size() && line[wordEnd] != ' '
					&& line[wordEnd] != '\t')
				wordEnd++;
			if (wordEnd > word)
				words.push_back(line.substr(word, wordEnd - word));
			word = wordEnd;
		}
		for (size_t i = 0; i < words.size() && i < 2; i++) {
			if (words[i] == "quasar")
				return true;
		}
	}
	return false;
}


static BPath
settings_path()
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	path.Append("Renga/welcome");
	return path;
}


static void
mark_done()
{
	BPath path = settings_path();
	BPath parent;
	path.GetParent(&parent);
	create_directory(parent.Path(), 0755);
	FILE* file = fopen(path.Path(), "w");
	if (file != NULL) {
		fputs("done true\n", file);
		fclose(file);
	}
}


//!	The shortcut key of the keymap in use.
static BPrivate::shortcut_key_style
current_shortcut_style()
{
	Keymap keymap;
	if (keymap.SetToCurrent() != B_OK)
		return BPrivate::kDefaultShortcutKeyStyle;
	return BPrivate::get_shortcut_key_style(keymap.Map());
}


//!	Gives the keymap in use the shortcut key \a style, saves and uses it.
static status_t
set_shortcut_style(BPrivate::shortcut_key_style style)
{
	Keymap keymap;
	status_t status = keymap.SetToCurrent();
	if (status != B_OK)
		return status;
	if (!BPrivate::set_shortcut_key_style(keymap.Map(), style))
		return B_OK;
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return B_ERROR;
	path.Append("Key_map");
	entry_ref ref;
	status = get_ref_for_path(path.Path(), &ref);
	if (status == B_OK)
		status = keymap.Save(ref);
	if (status == B_OK)
		status = keymap.Use();
	return status;
}


static void
launch(const char* path, const char* argument = NULL)
{
	entry_ref ref;
	if (get_ref_for_path(path, &ref) != B_OK)
		return;
	if (argument != NULL) {
		const char* args[] = { argument, NULL };
		be_roster->Launch(&ref, 1, args);
	} else
		be_roster->Launch(&ref);
}


//!	Runs \a command in a Terminal, so its progress and questions are seen.
static void
in_terminal(const char* title, const char* command)
{
	const char* args[] = { "-t", title, "/bin/sh", "-c", command, NULL };
	be_roster->Launch("application/x-vnd.Haiku-Terminal", 5,
		(char**)args);
}


// #pragma mark - window


/*!	A paragraph: a read-only BTextView of a fixed size. A word-wrapping
	text view reports height-for-width, which, nested in the pages' groups
	inside a card layout, made the window's layout loop for ever.
*/
/*!	Batch 12 (large-font audit): a paragraph's width, 480 pixels at the
	12-point default font and wider with a larger font (fewer lines, so
	that the tallest page still fits the screen), but at most what the
	screen leaves beside the window's insets.
*/
static float
paragraph_width()
{
	float width = ceilf(480 * be_plain_font->Size() / 12);
	return std::min(width,
		BPrivate::screen_content_limit().width - 60);
}


/*!	A paragraph of wrapped text whose height follows its text: the number
	of lines at the width it gets, times the font's line height. A fixed
	pixel height cut text off on screens with a larger font (maintainer's
	hardware, batch 10 image: page 1 showed two and a half lines). The
	height is first estimated from the font and a 480-pixel line, then set
	from the real line count whenever the width or the font changes
	(FrameResized(), AttachedToWindow()). No height-for-width: that sent
	the layout round in a loop; a change of height only re-lays out when
	it really changed, and the width does not depend on it, so it settles.
*/
class Paragraph : public BTextView {
public:
	Paragraph(const char* name, const char* text)
		:
		BTextView(name),
		fHeight(-1),
		fWidth(-1)
	{
		SetText(text);
		MakeEditable(false);
		MakeSelectable(false);
		SetWordWrap(true);
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		_SetHeight(_Estimate());
	}

	virtual bool HasHeightForWidth()
	{
		return false;
	}

	virtual void AttachedToWindow()
	{
		BTextView::AttachedToWindow();
		_Measure();
	}

	virtual void FrameResized(float width, float height)
	{
		BTextView::FrameResized(width, height);
		if (width != fWidth)
			_Measure();
	}

	//!	For the harness: the height the text needs, the view's height.
	float TextNeeds()
	{
		return TextHeight(0, CountLines() - 1) + 2;
	}

private:
	float _LineHeight()
	{
		BFont font;
		GetFont(&font);
		font_height metrics;
		font.GetHeight(&metrics);
		return ceilf(metrics.ascent + metrics.descent + metrics.leading);
	}

	float _Estimate()
	{
		BFont font;
		GetFont(&font);
		// words do not fill lines completely: a fifth more
		float lines = ceilf(font.StringWidth(Text()) * 1.2f
			/ paragraph_width());
		for (const char* c = Text(); *c != '\0'; c++) {
			if (*c == '\n')
				lines++;
		}
		return std::max(1.0f, lines) * _LineHeight() + 2;
	}

	void _Measure()
	{
		fWidth = Bounds().Width();
		if (fWidth < 10 || Window() == NULL)
			return;
		_SetHeight(TextNeeds());
	}

	void _SetHeight(float height)
	{
		height = ceilf(height);
		if (fabsf(height - fHeight) < 1)
			return;
		fHeight = height;
		SetExplicitMinSize(BSize(paragraph_width(), height));
		SetExplicitPreferredSize(BSize(paragraph_width(), height));
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
		InvalidateLayout();
	}

	float	fHeight;
	float	fWidth;
};


class WelcomeWindow : public BWindow {
public:
	WelcomeWindow(const char* pageID)
		:
		BWindow(BRect(120, 90, 720, 520), B_TRANSLATE_SYSTEM_NAME("Welcome"),
			B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE | B_NOT_ZOOMABLE
				| B_AUTO_UPDATE_SIZE_LIMITS),
		fCurrent(0)
	{
		fGraphicsStatus = NULL;
		fInstallQuasar = NULL;
		fQuasarInstructions = NULL;
		fFindings = find_things();
		// tests only: behave as with an NVIDIA card (QEMU has none)
		if (sAssumeNvidia)
			fFindings.nvidia = true;

		fCards = new BCardLayout();
		BView* cards = new BView("pages", 0, fCards);
		cards->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		for (int32 i = 0; i < kPageCount; i++)
			fCards->AddView(_MakePage(kPageIDs[i]));

		fBack = new BButton("back", B_TRANSLATE("Back"),
			new BMessage(kMsgBack));
		fNext = new BButton("next", B_TRANSLATE("Next"),
			new BMessage(kMsgNext));
		fDontShow = new BCheckBox("dontShow", B_TRANSLATE(
			"Don't show this at login again"), NULL);
		fPageNumber = new BStringView("pageNumber", "");

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(cards, 1.0f)
			.Add(new BSeparatorView(B_HORIZONTAL))
			.AddGroup(B_HORIZONTAL)
				.Add(fDontShow)
				.AddGlue()
				.Add(fPageNumber)
				.Add(fBack)
				.Add(fNext)
			.End();
		fNext->MakeDefault(true);

		int32 start = 0;
		for (int32 i = 0; pageID != NULL && i < kPageCount; i++) {
			if (strcmp(kPageIDs[i], pageID) == 0)
				start = i;
		}
		_Show(start);
		CenterOnScreen();

		// whether a repository has Quasar: pkgman takes a moment, the
		// window does not wait for it
		if (fFindings.nvidia && !fFindings.quasar) {
			thread_id thread = spawn_thread(_CheckQuasarThread,
				"check quasar", B_LOW_PRIORITY, this);
			resume_thread(thread);
		}
	}

	virtual bool QuitRequested()
	{
		if (fDontShow->Value() != 0 || fCurrent == kPageCount - 1)
			mark_done();
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgNext:
				if (fCurrent == kPageCount - 1) {
					mark_done();
					PostMessage(B_QUIT_REQUESTED);
				} else
					_Show(fCurrent + 1);
				break;
			case kMsgBack:
				if (fCurrent > 0)
					_Show(fCurrent - 1);
				break;
			case kMsgAction:
				_Action(message->GetString("action", ""));
				break;
			case kMsgShortcutCtrl:
				set_shortcut_style(BPrivate::SHORTCUT_KEYS_CTRL);
				break;
			case kMsgShortcutAlt:
				set_shortcut_style(BPrivate::SHORTCUT_KEYS_ALT);
				break;
			case kMsgQuasarChecked:
				_QuasarChecked(message->GetBool("available", false));
				break;
			case kMsgOpenQuasarInstructions:
				BUrl(kQuasarInstructions, true).OpenWithPreferredApplication();
				break;
			case kMsgParagraphFit:
			{
				// harness: each shown paragraph's text height and view
				// height on the current page, after a layout pass
				Layout(true);
				BMessage reply(B_REPLY);
				BLayoutItem* page = fCards->ItemAt(fCurrent);
				_ParagraphFit(page != NULL ? page->View() : NULL, reply);
				reply.AddString("page", kPageIDs[fCurrent]);
				message->SendReply(&reply);
				break;
			}
			case kMsgShowPage:
			{
				const char* id = message->GetString("id", "");
				for (int32 i = 0; i < kPageCount; i++) {
					if (strcmp(kPageIDs[i], id) == 0)
						_Show(i);
				}
				BMessage reply(B_REPLY);
				reply.AddString("page", kPageIDs[fCurrent]);
				message->SendReply(&reply);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	BView* _Text(const char* name, const char* text)
	{
		return new Paragraph(name, text);
	}

	BButton* _ActionButton(const char* name, const char* label,
		const char* action, bool enabled = true)
	{
		BMessage* message = new BMessage(kMsgAction);
		message->AddString("action", action);
		BButton* button = new BButton(name, label, message);
		button->SetEnabled(enabled);
		return button;
	}

	BView* _Page(const char* id, const char* title, BView* body,
		BView* status, BButton* first = NULL, BButton* second = NULL,
		BView* extra = NULL)
	{
		BStringView* heading = new BStringView("title", title);
		BFont font(be_bold_font);
		font.SetSize(font.Size() * 1.5f);
		heading->SetFont(&font);
		BGroupView* page = new BGroupView(id, B_VERTICAL);
		BLayoutBuilder::Group<> builder(page);
		builder
			.Add(heading)
			.Add(body);
		if (status != NULL)
			builder.Add(status);
		if (first != NULL || second != NULL) {
			BGroupView* buttons = new BGroupView(B_HORIZONTAL);
			if (first != NULL)
				buttons->AddChild(first);
			if (second != NULL)
				buttons->AddChild(second);
			buttons->GroupLayout()->AddItem(
				BSpaceLayoutItem::CreateGlue());
			builder.Add(buttons);
		}
		if (extra != NULL)
			builder.Add(extra);
		builder.AddGlue();
		return page;
	}

	BView* _MakePage(const char* id)
	{
		const Findings& f = fFindings;
		BString status;
		if (strcmp(id, "welcome") == 0) {
			return _Page(id, B_TRANSLATE("Welcome to Renga"), _Text("body",
				B_TRANSLATE("Renga is a fast, lean desktop system built on "
				"Haiku. These few pages help you connect, get drivers and "
				"software, and try a gamepad. Every step can be skipped, and "
				"this welcome can be opened again from the Deskbar "
				"(Applications) or the Control Panel.")), NULL);
		}
		if (strcmp(id, "network") == 0) {
			status.SetToFormat(B_TRANSLATE("Network devices: %" B_PRId32
				" (%" B_PRId32 " Wi-Fi), connected: %" B_PRId32),
				f.interfaces, f.wireless, f.connected);
			return _Page(id, B_TRANSLATE("Network and Wi-Fi"), _Text("body",
				B_TRANSLATE("Choose a Wi-Fi network or check the wired "
				"connection. A connection lets Renga get drivers and "
				"software in the next steps.")),
				new BStringView("networkStatus", status),
				_ActionButton("openNetwork", B_TRANSLATE("Open Network"
					B_UTF8_ELLIPSIS), "network"));
		}
		if (strcmp(id, "graphics") == 0) {
			if (!f.nvidia)
				status = B_TRANSLATE("No NVIDIA graphics card found.");
			else if (f.quasar) {
				status = B_TRANSLATE("NVIDIA graphics card found; Quasar, "
					"the NVIDIA driver, is installed.");
			} else {
				status = B_TRANSLATE("NVIDIA graphics card found. Looking "
					"for Quasar, the NVIDIA driver, in the repositories"
					B_UTF8_ELLIPSIS);
			}
			BString drivers;
			if (f.missingFirmware > 0) {
				drivers.SetToFormat(B_TRANSLATE(" %" B_PRId32 " device(s) "
					"need firmware for their Linux driver: Devices and "
					"drivers can get it."), f.missingFirmware);
				status << drivers;
			}
			fGraphicsStatus = _Text("graphicsStatus", status);
			fDriverNote = drivers;
			// shown when a repository has the package (_QuasarChecked())
			fInstallQuasar = _ActionButton("installQuasar",
				B_TRANSLATE("Install Quasar"), "quasar");
			fInstallQuasar->Hide();
			fQuasarInstructions = new BButton("quasarInstructions",
				B_TRANSLATE("How to build Quasar" B_UTF8_ELLIPSIS),
				new BMessage(kMsgOpenQuasarInstructions));
			fQuasarInstructions->Hide();
			BGroupView* buttons = new BGroupView(B_HORIZONTAL);
			buttons->AddChild(fInstallQuasar);
			buttons->AddChild(fQuasarInstructions);
			buttons->AddChild(_ActionButton("openDevices", B_TRANSLATE(
				"Devices and drivers" B_UTF8_ELLIPSIS), "devices"));
			buttons->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
			return _Page(id, B_TRANSLATE("Graphics and drivers"),
				_Text("body", B_TRANSLATE("Renga finds the drivers your "
				"hardware needs. Drivers and firmware come as packages from "
				"the Renga repository, so updates and restore points cover "
				"them too.")), fGraphicsStatus, NULL, NULL, buttons);
		}
		if (strcmp(id, "software") == 0) {
			status = f.rengaRepository
				? B_TRANSLATE("The Renga repository is set up.")
				: B_TRANSLATE("The Renga repository is not set up.");
			return _Page(id, B_TRANSLATE("Software"), _Text("body",
				B_TRANSLATE("Renga's own packages (drivers, ports built for "
				"Renga) come from the Renga repository, next to HaikuPorts. "
				"HaikuDepot shows both.")),
				new BStringView("softwareStatus", status),
				_ActionButton("addRepository", B_TRANSLATE("Add the Renga "
					"repository"), "repository", !f.rengaRepository),
				_ActionButton("openDepot", B_TRANSLATE("Open HaikuDepot"),
					"depot"));
		}
		if (strcmp(id, "linux") == 0) {
			if (!f.renguin)
				status = B_TRANSLATE("Linux programs are not available on "
					"this system.");
			else if (f.linuxRoot)
				status = B_TRANSLATE("A Linux root is installed.");
			else {
				status = B_TRANSLATE("No Linux root yet: it is downloaded "
					"once (Alpine Linux).");
			}
			return _Page(id, B_TRANSLATE("Linux programs"), _Text("body",
				B_TRANSLATE("Renguin runs Linux programs next to Renga's "
				"own. They need a Linux root: a small Linux system they "
				"find their libraries in.")),
				new BStringView("linuxStatus", status),
				_ActionButton("getLinuxRoot", B_TRANSLATE("Download a Linux "
					"root"), "linux", f.renguin && !f.linuxRoot));
		}
		if (strcmp(id, "gamepads") == 0) {
			return _Page(id, B_TRANSLATE("Gamepads"), _Text("body",
				B_TRANSLATE("Connect a gamepad and try every button and "
				"stick. Xbox, PlayStation, Nintendo and most other pads "
				"work; the mapping can be changed.")), NULL,
				_ActionButton("testGamepad", B_TRANSLATE("Test a gamepad"
					B_UTF8_ELLIPSIS), "gamepads"));
		}
		if (strcmp(id, "setup") == 0) {
			BRadioButton* ctrl = new BRadioButton("shortcutCtrl",
				B_TRANSLATE("Ctrl, as in Windows and Linux (Ctrl+C copies)"),
				new BMessage(kMsgShortcutCtrl));
			BRadioButton* alt = new BRadioButton("shortcutAlt",
				B_TRANSLATE("Alt, as in Haiku and BeOS (Alt+C copies)"),
				new BMessage(kMsgShortcutAlt));
			if (current_shortcut_style() == BPrivate::SHORTCUT_KEYS_ALT)
				alt->SetValue(B_CONTROL_ON);
			else
				ctrl->SetValue(B_CONTROL_ON);
			BGroupView* choice = new BGroupView(B_VERTICAL, 0);
			choice->AddChild(ctrl);
			choice->AddChild(alt);
			return _Page(id, B_TRANSLATE("Your setup"), _Text("body",
				B_TRANSLATE("Which key do your shortcuts use? This can be "
				"changed any time in Keymap (Control Panel, Keymap).")),
				choice);
		}
		return _Page(id, B_TRANSLATE("Tips"), _Text("body",
			B_TRANSLATE("Workspaces: Renga has several desktops. Switch with "
			"the shortcut key + F1, F2, ...; move a window there by holding "
			"Shift as well. Workspaces sets how many; the switcher in the "
			"Deskbar shows them, its \"+\" adds one, and the shortcut key + "
			"Control + Up shows them all.\n\n"
			"Snap layouts: while dragging a window, hold Shift to see the "
			"zones and drop it into one; right-click a window's tab to move "
			"it to another workspace. Control Panel, Windows and workspaces "
			"has the layouts and switches.\n\n"
			"Help: the Control Panel has every setting with a search; "
			"Renga Doctor collects a report for a bug report; the "
			"documentation is in the Deskbar's Help menu.")), NULL,
			_ActionButton("openWorkspaces", B_TRANSLATE("Workspaces"
				B_UTF8_ELLIPSIS), "workspaces"),
			_ActionButton("openControlPanel", B_TRANSLATE("Control Panel"
				B_UTF8_ELLIPSIS), "controlpanel"));
	}

	void _ParagraphFit(BView* view, BMessage& reply)
	{
		if (view == NULL)
			return;
		for (int32 i = 0; BView* child = view->ChildAt(i); i++) {
			Paragraph* paragraph = dynamic_cast<Paragraph*>(child);
			if (paragraph != NULL && !paragraph->IsHidden(paragraph)) {
				reply.AddString("name", paragraph->Name());
				reply.AddFloat("needs", paragraph->TextNeeds());
				reply.AddFloat("height", paragraph->Bounds().Height() + 1);
			}
			_ParagraphFit(child, reply);
		}
	}

	/*!	Batch 12 (large-font audit): the window grows with a taller page
		(B_AUTO_UPDATE_SIZE_LIMITS); keep it on the screen. At 20 points
		the last page's window reached below a 768-line screen.
	*/
	virtual void FrameResized(float width, float height)
	{
		BWindow::FrameResized(width, height);
		MoveOnScreen(B_MOVE_IF_PARTIALLY_OFFSCREEN);
	}

	void _Show(int32 index)
	{
		fCurrent = index;
		fCards->SetVisibleItem(index);
		fBack->SetEnabled(index > 0);
		fNext->SetLabel(index == kPageCount - 1 ? B_TRANSLATE("Finish")
			: B_TRANSLATE("Next"));
		BString number;
		number.SetToFormat("%" B_PRId32 " / %" B_PRId32, index + 1,
			kPageCount);
		fPageNumber->SetText(number);
		printf("Welcome: page %s\n", kPageIDs[index]);
	}

	void _Action(const char* action)
	{
		if (strcmp(action, "network") == 0)
			launch("/boot/system/preferences/Network");
		else if (strcmp(action, "devices") == 0)
			launch("/boot/system/apps/LinuxKPIManager");
		else if (strcmp(action, "quasar") == 0) {
			in_terminal("Install Quasar", "pkgman install quasar; echo; "
				"echo 'Restart to use the new driver.'; read -p 'Press "
				"Return to close. '");
		} else if (strcmp(action, "repository") == 0) {
			BString command;
			command << "pkgman add-repo " << kRengaRepository
				<< " && pkgman refresh; echo; read -p 'Press Return to "
				"close. '";
			in_terminal("Renga repository", command.String());
		} else if (strcmp(action, "depot") == 0)
			launch("/boot/system/apps/HaikuDepot");
		else if (strcmp(action, "linux") == 0) {
			in_terminal("Linux root", "linux --setup; echo; read -p 'Press "
				"Return to close. '");
		} else if (strcmp(action, "gamepads") == 0)
			launch("/boot/system/preferences/Gamepads");
		else if (strcmp(action, "workspaces") == 0)
			launch("/boot/system/apps/Workspaces");
		else if (strcmp(action, "controlpanel") == 0)
			launch("/boot/system/apps/ControlPanel");
	}

	static status_t _CheckQuasarThread(void* data)
	{
		BMessage done(kMsgQuasarChecked);
		done.AddBool("available", quasar_available());
		((WelcomeWindow*)data)->PostMessage(&done);
		return B_OK;
	}

	/*!	Quasar is only offered when a configured repository has it;
		otherwise the page says so and links its build instructions.
	*/
	void _QuasarChecked(bool available)
	{
		if (fGraphicsStatus == NULL)
			return;
		BString status;
		if (available) {
			status = B_TRANSLATE("NVIDIA graphics card found. Quasar, the "
				"NVIDIA driver, can be installed from your repositories.");
			if (fInstallQuasar->IsHidden())
				fInstallQuasar->Show();
		} else {
			status = B_TRANSLATE("NVIDIA graphics card found. Quasar, the "
				"NVIDIA driver, is not in your repositories yet; it can be "
				"built from its source.");
			if (fQuasarInstructions->IsHidden())
				fQuasarInstructions->Show();
		}
		status << fDriverNote;
		((BTextView*)fGraphicsStatus)->SetText(status);
		printf("Welcome: Quasar %s in the repositories\n",
			available ? "found" : "not found");
	}

	BCardLayout*	fCards;
	BView*			fGraphicsStatus;
	BButton*		fInstallQuasar;
	BButton*		fQuasarInstructions;
	BString			fDriverNote;
	BButton*		fBack;
	BButton*		fNext;
	BCheckBox*		fDontShow;
	BStringView*	fPageNumber;
	int32			fCurrent;
	Findings		fFindings;
};


class WelcomeApp : public BApplication {
public:
	WelcomeApp(const char* page)
		:
		BApplication(kSignature),
		fPage(page),
		fWindow(NULL)
	{
	}

	virtual void ReadyToRun()
	{
		fWindow = new WelcomeWindow(fPage);
		fWindow->Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if ((message->what == kMsgShowPage
				|| message->what == kMsgParagraphFit) && fWindow != NULL) {
			fWindow->PostMessage(message);
			return;
		}
		BApplication::MessageReceived(message);
	}

private:
	const char*		fPage;
	WelcomeWindow*	fWindow;
};


int
main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "--status") == 0) {
		BApplication app("application/x-vnd.Renga-Welcome-cli");
		Findings f = find_things();
		printf("interfaces %" B_PRId32 "\nwireless %" B_PRId32 "\n"
			"connected %" B_PRId32 "\nnvidia %s\nquasar %s\n"
			"renga_repository %s\nrenguin %s\nlinux_root %s\n"
			"missing_firmware %" B_PRId32 "\nshortcut_key %s\n"
			"done %s\nquasar_available %s\n", f.interfaces, f.wireless, f.connected,
			f.nvidia ? "yes" : "no", f.quasar ? "yes" : "no",
			f.rengaRepository ? "yes" : "no", f.renguin ? "yes" : "no",
			f.linuxRoot ? "yes" : "no", f.missingFirmware,
			current_shortcut_style() == BPrivate::SHORTCUT_KEYS_ALT
				? "alt" : "ctrl",
			access(settings_path().Path(), F_OK) == 0 ? "yes" : "no",
			quasar_available() ? "yes" : "no");
		return 0;
	}
	const char* page = NULL;
	if (argc >= 2 && strcmp(argv[argc - 1], "--test-assume-nvidia") == 0) {
		sAssumeNvidia = true;
		argc--;
	}
	if (argc == 3 && strcmp(argv[1], "--page") == 0)
		page = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "usage: Welcome [--page <id> | --status]\n");
		return 1;
	}
	WelcomeApp app(page);
	app.Run();
	return 0;
}
