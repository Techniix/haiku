/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "RenguinView.h"

#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <Roster.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextControl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "RenguinView"


static const char* kRenguinModule
	= "/boot/system/add-ons/kernel/personalities/renguin";
static const char* kLinuxRoot = "/boot/system/non-packaged/compat/linux";

static const uint32 kMsgShell = 'rshl';
static const uint32 kMsgSetup = 'rset';
static const uint32 kMsgUpgrade = 'rupg';
static const uint32 kMsgTrace = 'rtrc';
static const uint32 kMsgDeskbar = 'rdsk';


static std::string
read_text(const char* path)
{
	std::string text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[4096];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, length);
	fclose(file);
	return text;
}


static BPath
trace_settings_path()
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	path.Append("kernel/drivers/renguin");
	return path;
}


//!	Runs \a command in a Terminal, so its progress and questions are seen.
static void
in_terminal(const char* title, const char* command)
{
	const char* args[] = { "-t", title, "/bin/sh", "-c", command, NULL };
	be_roster->Launch("application/x-vnd.Haiku-Terminal", 5, (char**)args);
}


RenguinView::RenguinView()
	:
	BGroupView("renguin", B_VERTICAL)
{
	fLayer = new BStringView("layer", "");
	fRoot = new BStringView("root", "");
	fRelease = new BStringView("release", "");
	fPackages = new BStringView("packages", "");
	fShell = new BButton("openShell", B_TRANSLATE("Open the Linux shell"),
		new BMessage(kMsgShell));
	fSetup = new BButton("getRoot", B_TRANSLATE("Download a Linux root"),
		new BMessage(kMsgSetup));
	fUpgrade = new BButton("upgrade", B_TRANSLATE("Update Linux packages"),
		new BMessage(kMsgUpgrade));
	fDeskbar = new BButton("deskbarEntries", B_TRANSLATE("Update Deskbar "
		"entries"), new BMessage(kMsgDeskbar));
	fTrace = new BCheckBox("trace", B_TRANSLATE("Trace a Linux program's "
		"processes (for bug reports; written to the system log)"),
		new BMessage(kMsgTrace));
	fTracePattern = new BTextControl("tracePattern",
		B_TRANSLATE("Program:"), "", new BMessage(kMsgTrace));
	fTraceBuffer = new BCheckBox("traceBuffer", B_TRANSLATE("Keep the trace "
		"in memory until the program ends (does not slow it down)"),
		new BMessage(kMsgTrace));

	BLayoutBuilder::Group<>(this)
		.Add(fLayer)
		.Add(fRoot)
		.Add(fRelease)
		.Add(fPackages)
		.AddGroup(B_HORIZONTAL)
			.Add(fShell)
			.Add(fSetup)
			.Add(fUpgrade)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL)
			.Add(new BStringView("deskbarNote", B_TRANSLATE("Linux programs "
				"appear in the Deskbar under Applications, Linux.")))
			.Add(fDeskbar)
			.AddGlue()
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(new BStringView("diagnostics", B_TRANSLATE("Diagnostics")))
		.Add(fTrace)
		.AddGroup(B_VERTICAL, 0)
			.SetInsets(B_USE_DEFAULT_SPACING, 0, 0, 0)
			.Add(fTracePattern)
			.Add(fTraceBuffer)
		.End()
		.AddGlue();
}


void
RenguinView::AttachedToWindow()
{
	BGroupView::AttachedToWindow();
	BControl* controls[] = { fShell, fSetup, fUpgrade, fDeskbar, fTrace,
		fTracePattern, fTraceBuffer };
	for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
		controls[i]->SetTarget(this);
	Refresh();
}


void
RenguinView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgShell:
			be_roster->Launch("application/x-vnd.Renga-LinuxShell");
			break;
		case kMsgSetup:
			in_terminal("Linux root", "linux --setup; echo; read -p 'Press "
				"Return to close. '");
			break;
		case kMsgUpgrade:
			in_terminal("Linux packages", "linux apk update && linux apk "
				"upgrade; echo; read -p 'Press Return to close. '");
			break;
		case kMsgDeskbar:
			system("/bin/linux-apps sync &");
			break;
		case kMsgTrace:
			_SaveTrace();
			Refresh();
			break;
		default:
			BGroupView::MessageReceived(message);
	}
}


void
RenguinView::Refresh()
{
	struct stat st;
	bool layer = stat(kRenguinModule, &st) == 0;
	BString shellPath(kLinuxRoot);
	shellPath << "/bin/sh";
	bool root = stat(shellPath.String(), &st) == 0;

	fLayer->SetText(layer
		? B_TRANSLATE("Linux programs: available (Renguin is installed)")
		: B_TRANSLATE("Linux programs: not available on this system"));
	BString text;
	if (root) {
		text.SetToFormat(B_TRANSLATE("Linux root: %s"), kLinuxRoot);
		fRoot->SetText(text);
		BString releasePath(kLinuxRoot);
		releasePath << "/etc/alpine-release";
		std::string release = read_text(releasePath.String());
		while (!release.empty() && (release.back() == '\n'
				|| release.back() == '\r'))
			release.pop_back();
		text.SetToFormat(B_TRANSLATE("Linux system: Alpine Linux %s"),
			release.empty() ? "?" : release.c_str());
		fRelease->SetText(text);

		BString databasePath(kLinuxRoot);
		databasePath << "/lib/apk/db/installed";
		std::string database = read_text(databasePath.String());
		int32 packages = 0;
		for (size_t i = 0; i + 2 < database.size(); i++) {
			if ((i == 0 || database[i - 1] == '\n') && database[i] == 'P'
				&& database[i + 1] == ':')
				packages++;
		}
		text.SetToFormat(B_TRANSLATE("Linux packages installed: %" B_PRId32),
			packages);
		fPackages->SetText(text);
	} else {
		fRoot->SetText(B_TRANSLATE("Linux root: none yet (Linux programs "
			"need one; it is downloaded once)"));
		fRelease->SetText("");
		fPackages->SetText("");
	}
	fShell->SetEnabled(layer && root);
	fSetup->SetEnabled(layer && !root);
	fUpgrade->SetEnabled(layer && root);
	fDeskbar->SetEnabled(layer && root);

	// the trace settings file (PORTING.md, "Diagnosing Linux programs")
	std::string settings = read_text(trace_settings_path().Path());
	size_t position = settings.find("trace_process \"");
	bool tracing = position != std::string::npos;
	if (tracing) {
		size_t start = position + 15;
		size_t end = settings.find('"', start);
		if (fTracePattern->Text() == NULL || fTracePattern->Text()[0] == '\0')
			fTracePattern->SetText(settings.substr(start, end - start).c_str());
	}
	fTrace->SetValue(tracing);
	fTraceBuffer->SetValue(settings.find("trace_buffer true")
		!= std::string::npos);
	fTracePattern->SetEnabled(layer);
	fTraceBuffer->SetEnabled(layer && tracing);
	fTrace->SetEnabled(layer);
}


/*!	Writes ~/config/settings/kernel/drivers/renguin: "trace_process
	<pattern>" and "trace_buffer true" while the trace is on; the file is
	removed when it is off (Renguin reads it at every exec).
*/
void
RenguinView::_SaveTrace()
{
	BPath path = trace_settings_path();
	const char* pattern = fTracePattern->Text();
	if (fTrace->Value() == 0 || pattern == NULL || pattern[0] == '\0') {
		unlink(path.Path());
		return;
	}
	BPath parent;
	path.GetParent(&parent);
	create_directory(parent.Path(), 0755);
	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return;
	BString escaped(pattern);
	escaped.CharacterEscape("\"\\", '\\');
	fprintf(file, "# Renguin Manager: the diagnostics trace\n"
		"trace_process \"%s\"\n", escaped.String());
	if (fTraceBuffer->Value() != 0)
		fputs("trace_buffer true\n", file);
	fclose(file);
}
