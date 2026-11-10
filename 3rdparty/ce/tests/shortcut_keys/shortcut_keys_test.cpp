/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Ctrl is the shortcut key in Renga (Windows/Linux style),
	not Alt as in BeOS and Haiku, and Terminal keeps Ctrl+C for the shell.

	Keymap checks (no desktop needed):
	  default_keymap      the active keymap (get_key_map()) puts B_COMMAND_KEY
	                      on the Ctrl keys and B_CONTROL_KEY on the Alt keys
	  key_map_file        input_server saved the same into
	                      ~/config/settings/Key_map
	  layout_keeps_style  a keymap file from /boot/system/data/Keymaps (Alt
	                      style, as all of them) gets the active style with
	                      the helper Keymap preferences and FirstBootPrompt
	                      use, and nothing else in it changes

	Typing checks: the test asks the host to press keys through QEMU's
	monitor ("CE-HOST sendkey <keys>", see ce-test.py) and looks at the
	result:
	  ctrl_v_pastes            Ctrl+V pastes the clipboard into a BTextView
	  alt_v_does_not_paste     Alt+V does not
	  terminal_ctrl_c          Ctrl+C in Terminal interrupts the shell's
	                           foreground command (SIGINT)
	  terminal_ctrl_shift_v    Ctrl+Shift+V pastes into Terminal
	Output format: see 3rdparty/ce/README.md.
*/


#include <Application.h>
#include <Clipboard.h>
#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollView.h>
#include <TextView.h>
#include <Window.h>

#include <ShortcutKeyStyle.h>

#include <ByteOrder.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define SUITE "shortcut_keys"

static const char* kTerminalDir = "/tmp/ce-shortcut-keys";
static const char* kTerminalSignature = "application/x-vnd.Haiku-Terminal";

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


//! Asks the host to press \a keys (QEMU sendkey syntax, e.g. "ctrl-v").
static void
send_keys(const char* keys)
{
	printf("CE-HOST sendkey %s\n", keys);
	fflush(stdout);
}


static const char*
style_name(BPrivate::shortcut_key_style style)
{
	switch (style) {
		case BPrivate::SHORTCUT_KEYS_CTRL:
			return "Ctrl";
		case BPrivate::SHORTCUT_KEYS_ALT:
			return "Alt";
		default:
			return "other";
	}
}


/*!	Reads a keymap file as Keymap preferences and input_server write it:
	the key_map structure and the character table size, big-endian.
*/
static bool
read_keymap_file(const char* path, key_map& map)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	bool ok = fread(&map, sizeof(map), 1, file) == 1;
	fclose(file);
	if (!ok)
		return false;
	for (uint32 i = 0; i < sizeof(map) / 4; i++)
		((uint32*)&map)[i] = B_BENDIAN_TO_HOST_INT32(((uint32*)&map)[i]);
	return true;
}


static void
test_keymap(BPrivate::shortcut_key_style& activeStyle)
{
	key_map* active = NULL;
	char* chars = NULL;
	// input_server may still be starting.
	for (int i = 0; i < 60 && active == NULL; i++) {
		get_key_map(&active, &chars);
		if (active == NULL)
			snooze(500000);
	}
	activeStyle = active != NULL
		? BPrivate::get_shortcut_key_style(*active)
		: BPrivate::SHORTCUT_KEYS_OTHER;
	report("default_keymap", active != NULL
			&& activeStyle == BPrivate::SHORTCUT_KEYS_CTRL,
		"%s shortcut key (command keys 0x%" B_PRIx32 "/0x%" B_PRIx32
		", control keys 0x%" B_PRIx32 "/0x%" B_PRIx32 "; want Ctrl: "
		"command 0x5c/0x60, control 0x5d/0x67)", style_name(activeStyle),
		active ? active->left_command_key : 0,
		active ? active->right_command_key : 0,
		active ? active->left_control_key : 0,
		active ? active->right_control_key : 0);

	BPath path;
	key_map saved;
	bool haveFile = find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("Key_map") == B_OK
		&& read_keymap_file(path.Path(), saved);
	report("key_map_file", haveFile && active != NULL
			&& saved.left_command_key == active->left_command_key
			&& saved.right_command_key == active->right_command_key
			&& saved.left_control_key == active->left_control_key
			&& saved.right_control_key == active->right_control_key,
		"%s: %s", path.Path(), haveFile
			? style_name(BPrivate::get_shortcut_key_style(saved))
			: "cannot be read");

	key_map german;
	key_map converted;
	bool haveGerman = read_keymap_file("/boot/system/data/Keymaps/German",
		german);
	bool unchangedElsewhere = false;
	BPrivate::shortcut_key_style convertedStyle = BPrivate::SHORTCUT_KEYS_OTHER;
	if (haveGerman) {
		converted = german;
		BPrivate::set_shortcut_key_style(converted, activeStyle);
		convertedStyle = BPrivate::get_shortcut_key_style(converted);
		key_map expected = converted;
		expected.left_command_key = german.left_command_key;
		expected.right_command_key = german.right_command_key;
		expected.left_control_key = german.left_control_key;
		expected.right_control_key = german.right_control_key;
		unchangedElsewhere = memcmp(&expected, &german, sizeof(german)) == 0;
	}
	report("layout_keeps_style", haveGerman
			&& BPrivate::get_shortcut_key_style(german)
				== BPrivate::SHORTCUT_KEYS_ALT
			&& convertedStyle == activeStyle && unchangedElsewhere,
		"German keymap file: %s, given the active style: %s, rest %s",
		haveGerman ? style_name(BPrivate::get_shortcut_key_style(german))
			: "missing", style_name(convertedStyle),
		unchangedElsewhere ? "unchanged" : "changed");
}


// #pragma mark - typing into a BTextView


class TypingWindow : public BWindow {
public:
	TypingWindow()
		:
		BWindow(BRect(100, 100, 500, 300), "shortcut_keys test",
			B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS)
	{
		BRect bounds = Bounds();
		fTextView = new BTextView(bounds, "text", bounds.InsetByCopy(4, 4),
			B_FOLLOW_ALL, B_WILL_DRAW);
		AddChild(fTextView);
		fTextView->MakeFocus(true);
	}

	BString Text()
	{
		BString text;
		if (LockWithTimeout(1000000) == B_OK) {
			text = fTextView->Text();
			Unlock();
		}
		return text;
	}

	void Clear()
	{
		if (LockWithTimeout(1000000) == B_OK) {
			fTextView->SetText("");
			fTextView->MakeFocus(true);
			Unlock();
		}
	}

private:
	BTextView*	fTextView;
};


static bool
set_clipboard(const char* text)
{
	if (!be_clipboard->Lock())
		return false;
	be_clipboard->Clear();
	BMessage* clip = be_clipboard->Data();
	clip->AddData("text/plain", B_MIME_TYPE, text, strlen(text));
	bool ok = be_clipboard->Commit() == B_OK;
	be_clipboard->Unlock();
	return ok;
}


//! Waits up to \a seconds for the text view to contain \a text.
static bool
wait_for_text(TypingWindow* window, const char* text, int seconds,
	BString& seen)
{
	for (int i = 0; i < seconds * 10; i++) {
		seen = window->Text();
		if (seen.FindFirst(text) >= 0)
			return true;
		snooze(100000);
	}
	return false;
}


static bool
wait_for_active(BWindow* window, int seconds)
{
	for (int i = 0; i < seconds * 10; i++) {
		if (window->IsActive())
			return true;
		snooze(100000);
	}
	return false;
}


static void
test_text_view()
{
	const char* clip = "renga-paste-check";
	TypingWindow* window = new TypingWindow;
	window->Show();
	if (window->Lock()) {
		window->Activate();
		window->Unlock();
	}
	bool active = wait_for_active(window, 10);
	bool clipped = set_clipboard(clip);

	BString seen;
	send_keys("ctrl-v");
	bool pasted = wait_for_text(window, clip, 10, seen);
	report("ctrl_v_pastes", active && clipped && pasted,
		"window %s, clipboard %s, text after Ctrl+V: \"%s\"",
		active ? "active" : "never active", clipped ? "set" : "not set",
		seen.String());

	window->Clear();
	send_keys("alt-v");
	// A paste would show within the time Ctrl+V took above; wait longer.
	bool altPasted = wait_for_text(window, clip, 5, seen);
	report("alt_v_does_not_paste", active && !altPasted,
		"text after Alt+V: \"%s\"", seen.String());

	if (window->Lock())
		window->Quit();
}


// #pragma mark - Terminal


static bool
read_file(const char* name, BString& content)
{
	BString path;
	path.SetToFormat("%s/%s", kTerminalDir, name);
	FILE* file = fopen(path.String(), "r");
	if (file == NULL)
		return false;
	char buffer[256];
	size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	buffer[bytes] = '\0';
	content = buffer;
	content.RemoveAll("\n");
	return true;
}


static bool
wait_for_file(const char* name, int seconds, BString& content)
{
	for (int i = 0; i < seconds * 10; i++) {
		if (read_file(name, content))
			return true;
		snooze(100000);
	}
	return false;
}


static void
quit_terminals()
{
	BList teams;
	be_roster->GetAppList(kTerminalSignature, &teams);
	for (int32 i = 0; i < teams.CountItems(); i++)
		kill_team((team_id)(addr_t)teams.ItemAt(i));
}


static void
test_terminal()
{
	const char* clip = "renga-terminal-paste";
	BString command;
	command.SetToFormat("rm -rf %s && mkdir -p %s", kTerminalDir,
		kTerminalDir);
	system(command.String());

	// The shell waits in "sleep" for Ctrl+C (the trap records SIGINT and
	// the script goes on), then reads the line pasted with Ctrl+Shift+V.
	BString script;
	script.SetToFormat(
		"trap 'echo interrupted > %s/sigint' INT\n"
		"echo ready > %s/ready\n"
		"sleep 40\n"
		"echo reading > %s/reading\n"
		"read line\n"
		"echo \"$line\" > %s/line\n",
		kTerminalDir, kTerminalDir, kTerminalDir, kTerminalDir);
	BString scriptPath;
	scriptPath.SetToFormat("%s/script.sh", kTerminalDir);
	FILE* file = fopen(scriptPath.String(), "w");
	if (file != NULL) {
		fputs(script.String(), file);
		fclose(file);
	}

	const char* args[] = { "-t", "shortcut_keys", "/bin/sh",
		scriptPath.String(), NULL };
	team_id team = -1;
	status_t status = be_roster->Launch(kTerminalSignature, 4,
		const_cast<char**>(args), &team);

	BString content;
	bool ready = status == B_OK && wait_for_file("ready", 20, content);
	// Give the window a moment to become active once its shell is up.
	snooze(1500000);
	if (ready)
		send_keys("ctrl-c");
	bool interrupted = ready && wait_for_file("sigint", 10, content);
	report("terminal_ctrl_c", interrupted, "%s",
		status != B_OK ? strerror(status)
		: !ready ? "Terminal's shell never started"
		: interrupted ? "Ctrl+C interrupted the shell's sleep (SIGINT)"
		: "no SIGINT within 10 s of Ctrl+C");

	bool reading = interrupted && wait_for_file("reading", 10, content);
	bool clipped = set_clipboard(clip);
	if (reading) {
		snooze(500000);
		send_keys("ctrl-shift-v");
		snooze(1500000);
		send_keys("ret");
	}
	bool gotLine = reading && wait_for_file("line", 10, content);
	report("terminal_ctrl_shift_v", reading && clipped && gotLine
			&& content == clip,
		"%s", !reading ? "shell not reading (needs terminal_ctrl_c)"
			: !gotLine ? "no line read within 10 s"
			: (BString("read \"") << content << "\" (want \"" << clip
				<< "\")").String());

	quit_terminals();
	command.SetToFormat("rm -rf %s", kTerminalDir);
	system(command.String());
}


class TestApplication : public BApplication {
public:
	TestApplication(status_t* status)
		:
		BApplication("application/x-vnd.Renga-ce-shortcut-keys", status)
	{
	}

	virtual void ReadyToRun()
	{
		// The checks wait for windows, which need this looper running.
		thread_id thread = spawn_thread(&_Run, "typing checks",
			B_NORMAL_PRIORITY, this);
		resume_thread(thread);
	}

private:
	static status_t _Run(void* data)
	{
		test_text_view();
		test_terminal();
		((BApplication*)data)->PostMessage(B_QUIT_REQUESTED);
		return B_OK;
	}
};


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	BPrivate::shortcut_key_style style;
	test_keymap(style);

	// The typing checks need app_server and input_server (windows and
	// keyboard input), which may come up after the runner starts. They do
	// not need Tracker: this used to wait for Tracker, but a Renga image
	// boots into FirstBootPrompt (data/launch/user, target first_boot) and
	// Tracker never runs there, so every renga-raw run waited the full 90 s
	// here in silence. That is exactly the harness's stall timeout, so some
	// runs (slower boots, typically the first after a rebuild) were taken
	// for a hang and stopped in the kernel debugger. Wait for input_server,
	// as the pointer and media_ui suites do, and say so while waiting.
	for (int i = 0; i < 300; i++) {
		if (be_roster->IsRunning("application/x-vnd.Be-input_server"))
			break;
		if (i > 0 && i % 30 == 0) {
			printf("CE-INFO shortcut_keys: waiting for input_server (%d s)\n",
				i);
			fflush(stdout);
		}
		snooze(1000000);
	}
	// Where the desktop does start (images without FirstBootPrompt), let
	// Tracker and Deskbar come up first, so that they do not take the
	// keyboard focus from the test window in the middle of a check.
	if (access("/boot/system/bin/FirstBootPrompt", F_OK) != 0) {
		for (int i = 0; i < 180; i++) {
			if (be_roster->IsRunning("application/x-vnd.Be-TRAK"))
				break;
			if (i > 0 && i % 30 == 0) {
				printf("CE-INFO shortcut_keys: waiting for Tracker (%d s)\n",
					i);
				fflush(stdout);
			}
			snooze(1000000);
		}
	}
	status_t status = B_ERROR;
	TestApplication* app = new TestApplication(&status);
	if (status != B_OK)
		report("desktop", false, "no BApplication: %s", strerror(status));
	else
		app->Run();
	delete app;

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
