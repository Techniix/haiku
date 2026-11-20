/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renga Doctor (batch 5): one click collects a hardware report.

	The archive is System logs' "Save a report" (src/apps/systemlogs/
	ReportArchive.cpp: logs, crash reports, listdev, listusb, sysinfo,
	LinuxKPI drivers, screen, audio and input devices, network, boot state
	and restore points, packages), with an overview first (overview.txt):
	the system, the graphics driver, the LinuxKPI drivers, the boot state,
	the crash reports and the disk, each line marked "ok" or "check", so
	that whoever reads the report sees at once where to look.

	  RengaDoctor                      the window
	  RengaDoctor --collect [<path>]   write the report without a window
	                                   (default: ~/Desktop/renga-report-
	                                   <date>.zip)
	  RengaDoctor --overview           print the overview
*/


#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include <boot_state_file.h>

#include "ReportArchive.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "RengaDoctor"


static const char* kSignature = "application/x-vnd.Renga-Doctor";
static const uint32 kMsgCollect = 'cllc';
static const uint32 kMsgShowReport = 'shrp';
static const uint32 kMsgCollected = 'clld';


static std::string
command_output(const char* command)
{
	std::string output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[4096];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, length);
	pclose(pipe);
	return output;
}


static std::string
first_line(const std::string& text)
{
	size_t end = text.find('\n');
	return end == std::string::npos ? text : text.substr(0, end);
}


static void
line(std::string& overview, bool ok, const char* what, const std::string& text)
{
	char buffer[600];
	snprintf(buffer, sizeof(buffer), "%-6s %-18s %s\n", ok ? "ok" : "check",
		what, text.c_str());
	overview += buffer;
}


//!	The overview: one line per area, "ok" or "check".
static std::string
make_overview()
{
	std::string overview = "Renga Doctor overview\n";
	time_t now = time(NULL);
	char date[64];
	strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
	overview += std::string("Made: ") + date + "\n\n";

	line(overview, true, "System", first_line(command_output(
		"/bin/uname -a 2>&1")));

	// graphics: the accelerant and the mode
	BScreen screen;
	accelerant_device_info info;
	if (screen.IsValid() && screen.GetDeviceInfo(&info) == B_OK) {
		display_mode mode;
		screen.GetMode(&mode);
		char text[300];
		snprintf(text, sizeof(text), "%s (%s), %ux%u",
			info.name, info.chipset, mode.virtual_width,
			mode.virtual_height);
		bool generic = strstr(info.name, "VESA") != NULL
			|| strstr(info.name, "Framebuffer") != NULL
			|| strstr(info.name, "framebuffer") != NULL;
		line(overview, !generic, "Graphics", std::string(text)
			+ (generic ? " - a generic driver: no acceleration, no "
				"multi-monitor" : ""));
	} else
		line(overview, false, "Graphics", "no screen information");

	// LinuxKPI drivers: the devices one of them supports, and their state
	std::string linuxkpi = command_output("/bin/lkpictl hardware 2>/dev/null");
	// ("vvvv:dddd  <name> <class>  driver <name>..." per device)
	int devices = 0;
	size_t start = 0;
	while (start < linuxkpi.size()) {
		size_t end = linuxkpi.find('\n', start);
		if (end == std::string::npos)
			end = linuxkpi.size();
		if (end - start > 9 && linuxkpi[start + 4] == ':'
			&& linuxkpi[start] != ' ')
			devices++;
		start = end + 1;
	}
	if (devices == 0)
		line(overview, true, "Linux drivers", "no device that needs one");
	else {
		bool problem = linuxkpi.find("missing firmware") != std::string::npos
			|| linuxkpi.find("not installed") != std::string::npos;
		char text[200];
		snprintf(text, sizeof(text), "%d device(s) for Linux drivers%s "
			"(hardware/linuxkpi.txt)", devices,
			problem ? ", firmware or driver missing" : "");
		line(overview, !problem, "Linux drivers", text);
	}

	// boot state
	char statePath[B_PATH_NAME_LENGTH];
	BootState::file_path(statePath, sizeof(statePath));
	BootState::Record record;
	int slot = BootState::read_file(statePath, record);
	if (slot == -2)
		line(overview, true, "Boot", "no boot counting on this volume");
	else {
		char text[200];
		snprintf(text, sizeof(text), "%u start(s) counted, %u not "
			"confirmed, running %s", (unsigned)record.totalStarts,
			(unsigned)record.failedStarts,
			record.booting[0] != '\0' ? record.booting : "the latest state");
		line(overview, record.failedStarts <= 1, "Boot", text);
	}
	static char names[64][BootState::kNameLength];
	int points = BootState::list_restore_points(names, 64);
	char pointText[160];
	if (points > 0) {
		char label[160];
		BootState::restore_point_label(names[0], label, sizeof(label));
		snprintf(pointText, sizeof(pointText), "%d, newest: %s", points,
			label);
	} else
		strlcpy(pointText, "none yet", sizeof(pointText));
	line(overview, true, "Restore points", pointText);

	// crash reports
	int kernelCrashes = 0, appCrashes = 0;
	if (DIR* dir = opendir("/boot/system/var/log/crash")) {
		while (dirent* entry = readdir(dir)) {
			if (strncmp(entry->d_name, "kernel-", 7) == 0)
				kernelCrashes++;
			else if (strncmp(entry->d_name, "app-", 4) == 0)
				appCrashes++;
		}
		closedir(dir);
	}
	char crashText[120];
	snprintf(crashText, sizeof(crashText), "%d kernel, %d application "
		"(crash/)", kernelCrashes, appCrashes);
	line(overview, kernelCrashes == 0, "Crash reports", crashText);

	// disk
	std::string df = command_output("/bin/df /boot 2>&1");
	line(overview, true, "Boot volume", first_line(df.substr(
		df.find('\n') != std::string::npos ? df.find('\n') + 1 : 0)));

	overview += "\nThe rest of the archive has the details; README.txt says "
		"what is where.\n";
	return overview;
}


static std::string
default_path()
{
	BPath path;
	if (find_directory(B_DESKTOP_DIRECTORY, &path, true) != B_OK)
		path.SetTo("/boot/home/Desktop");
	time_t now = time(NULL);
	char name[64];
	strftime(name, sizeof(name), "renga-report-%Y-%m-%d_%H-%M-%S.zip",
		localtime(&now));
	path.Append(name);
	return path.Path();
}


static status_t
collect(const char* path, std::string& overview)
{
	overview = make_overview();
	BString summary;
	return save_report_archive(path, "overview.txt", overview.c_str(),
		&summary);
}


// #pragma mark - window


class DoctorWindow : public BWindow {
public:
	DoctorWindow()
		:
		BWindow(BRect(100, 100, 700, 480), B_TRANSLATE_SYSTEM_NAME(
			"Renga Doctor"), B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE
				| B_AUTO_UPDATE_SIZE_LIMITS)
	{
		BStringView* intro = new BStringView("intro", B_TRANSLATE(
			"Collects hardware, driver status, logs and crash reports into "
			"one report for a bug report."));
		fCollect = new BButton("collect", B_TRANSLATE("Collect a report"),
			new BMessage(kMsgCollect));
		fShow = new BButton("show", B_TRANSLATE("Show in Tracker"),
			new BMessage(kMsgShowReport));
		fShow->SetEnabled(false);
		fStatus = new BStringView("status", "");
		fOverview = new BTextView("overview");
		fOverview->MakeEditable(false);
		fOverview->SetFontAndColor(be_fixed_font);
		BScrollView* scroll = new BScrollView("overviewScroll", fOverview, 0,
			true, true);
		scroll->SetExplicitMinSize(BSize(520, 220));

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(intro)
			.AddGroup(B_HORIZONTAL)
				.Add(fCollect)
				.Add(fShow)
				.AddGlue()
			.End()
			.Add(fStatus)
			.Add(scroll);
		fOverview->SetText(make_overview().c_str());
		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgCollect:
			{
				fCollect->SetEnabled(false);
				fStatus->SetText(B_TRANSLATE("Collecting" B_UTF8_ELLIPSIS));
				thread_id thread = spawn_thread(_CollectThread, "collect",
					B_LOW_PRIORITY, this);
				resume_thread(thread);
				break;
			}
			case kMsgCollected:
			{
				fCollect->SetEnabled(true);
				status_t status = message->GetInt32("status", B_ERROR);
				if (status == B_OK) {
					fPath = message->GetString("path", "");
					BString text;
					text.SetToFormat(B_TRANSLATE("Saved: %s"),
						fPath.String());
					fStatus->SetText(text);
					fShow->SetEnabled(true);
					fOverview->SetText(message->GetString("overview", ""));
				} else {
					BString text;
					text.SetToFormat(B_TRANSLATE("The report could not be "
						"saved: %s"), strerror(status));
					fStatus->SetText(text);
				}
				break;
			}
			case kMsgShowReport:
			{
				BEntry entry(fPath.String());
				BEntry parent;
				entry_ref ref;
				if (entry.GetParent(&parent) == B_OK
					&& parent.GetRef(&ref) == B_OK) {
					BMessage open(B_REFS_RECEIVED);
					open.AddRef("refs", &ref);
					BMessenger("application/x-vnd.Be-TRAK").SendMessage(&open);
				}
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	static status_t _CollectThread(void* data)
	{
		DoctorWindow* window = (DoctorWindow*)data;
		std::string path = default_path();
		std::string overview;
		status_t status = collect(path.c_str(), overview);
		BMessage done(kMsgCollected);
		done.AddInt32("status", status);
		done.AddString("path", path.c_str());
		done.AddString("overview", overview.c_str());
		window->PostMessage(&done);
		return B_OK;
	}

	BButton*		fCollect;
	BButton*		fShow;
	BStringView*	fStatus;
	BTextView*		fOverview;
	BString			fPath;
};


int
main(int argc, char** argv)
{
	if (argc >= 2 && strcmp(argv[1], "--overview") == 0) {
		BApplication app("application/x-vnd.Renga-Doctor-cli");
		printf("%s", make_overview().c_str());
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "--collect") == 0) {
		BApplication app("application/x-vnd.Renga-Doctor-cli");
		std::string path = argc >= 3 ? argv[2] : default_path();
		std::string overview;
		status_t status = collect(path.c_str(), overview);
		if (status != B_OK) {
			fprintf(stderr, "RengaDoctor: %s: %s\n", path.c_str(),
				strerror(status));
			return 1;
		}
		printf("%s\n", path.c_str());
		return 0;
	}
	if (argc != 1) {
		fprintf(stderr, "usage: RengaDoctor [--collect [<archive>] | "
			"--overview]\n");
		return 1;
	}

	BApplication app(kSignature);
	(new DoctorWindow())->Show();
	app.Run();
	return 0;
}
