/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	linux-apps (batch 5): Deskbar entries for Linux programs from their
	.desktop files. Puts two .desktop files (one visible with a PNG icon,
	one NoDisplay) into the Linux root, then:

	  linux_apps.list        "list" shows the visible one, without the
	                         field codes of its Exec line
	  linux_apps.entry       "sync" writes the Deskbar entry, a script
	                         running the program through "linux"
	  linux_apps.icon        the entry has a 32 and a 16 pixel icon
	  linux_apps.hidden      no entry for the NoDisplay one
	  linux_apps.user_kept   an entry the user made in the same folder is
	                         left alone
	  linux_apps.removed     after the .desktop file is gone, sync removes
	                         its entry
	  linux_apps.open_with   (batch 10) a program with a MimeType line is an
	                         application supporting that type (signature,
	                         supported types in the MIME database), and
	                         launching it with a file, as "Open with" does,
	                         hands the file to the Linux program
	  linux_apps.badge       the Renguin badge changes the entry's icon,
	                         and "badge false" takes it away again
	  linux_apps.appimage    a file with the AppImage magic is sniffed as
	                         application/x-appimage, whose preferred
	                         application is linux-apps; opening it runs it
	                         through linux with APPIMAGE_EXTRACT_AND_RUN=1
	  linux_apps.theme       "theme" writes GTK 3/4 settings and colours and
	                         qt5ct/qt6ct schemes into ~/.config with the
	                         marker line, and "theme false" removes them

	Everything it adds is removed again.
*/


#include <Application.h>
#include <Roster.h>
#include <MimeType.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <File.h>
#include <FindDirectory.h>
#include <Node.h>
#include <NodeInfo.h>
#include <fs_attr.h>
#include <Path.h>
#include <TranslatorRoster.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>


#define SUITE "linux_apps"

static const char* kRoot = "/boot/system/non-packaged/compat/linux";
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[500];
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


static std::string
run(const char* command, int* _status = NULL)
{
	std::string output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, bytes);
	int status = pclose(pipe);
	if (_status != NULL)
		*_status = status;
	return output;
}


static void
write_text(const std::string& path, const char* text)
{
	FILE* file = fopen(path.c_str(), "w");
	if (file != NULL) {
		fputs(text, file);
		fclose(file);
	}
}


static bool
write_png(const std::string& path)
{
	BBitmap bitmap(BRect(0, 0, 47, 47), B_RGBA32);
	uint32* bits = (uint32*)bitmap.Bits();
	for (int32 i = 0; i < 48 * 48; i++)
		bits[i] = 0xff20a040;
	BBitmapStream stream(&bitmap);
	BFile file(path.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	bool ok = BTranslatorRoster::Default()->Translate(&stream, NULL, NULL,
		&file, B_PNG_FORMAT) == B_OK;
	BBitmap* detached;
	stream.DetachBitmap(&detached);
	return ok;
}


int
main()
{
	BApplication app("application/x-vnd.Renga-linux-apps-test");
	printf("CE-SUITE %s START\n", SUITE);
	struct stat st;
	if (access("/bin/linux-apps", X_OK) != 0
		|| stat((std::string(kRoot) + "/bin/sh").c_str(), &st) != 0) {
		printf("CE-INFO no linux-apps or Linux root, nothing to test\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	std::string applications = std::string(kRoot) + "/usr/share/applications";
	std::string pixmaps = std::string(kRoot) + "/usr/share/pixmaps";
	run(("mkdir -p " + applications + " " + pixmaps).c_str());
	std::string visible = applications + "/ce-test-viewer.desktop";
	std::string hidden = applications + "/ce-test-hidden.desktop";
	write_text(visible, "[Desktop Entry]\nType=Application\n"
		"Name=CE Test Viewer\nName[de]=CE Testbetrachter\n"
		"Exec=ce-test-viewer --open %F\nIcon=ce-test-viewer\n"
		"Terminal=false\n");
	write_text(hidden, "[Desktop Entry]\nType=Application\n"
		"Name=CE Hidden Helper\nExec=ce-hidden\nNoDisplay=true\n");
	bool png = write_png(pixmaps + "/ce-test-viewer.png");

	BPath menu;
	find_directory(B_USER_DESKBAR_DIRECTORY, &menu, true);
	menu.Append("Applications/Linux");
	run((std::string("mkdir -p '") + menu.Path() + "'").c_str());
	std::string userEntry = std::string(menu.Path()) + "/CE User Entry";
	write_text(userEntry, "#!/bin/sh\necho mine\n");

	std::string listing = run("/bin/linux-apps list 2>&1");
	report("list", listing.find("ce-test-viewer\tCE Test Viewer\t"
			"ce-test-viewer --open") != std::string::npos
		&& listing.find("ce-test-hidden") == std::string::npos
		&& listing.find("%F") == std::string::npos, "%s", listing.c_str());

	int status;
	std::string output = run("/bin/linux-apps sync -v 2>&1", &status);
	std::string entry = std::string(menu.Path()) + "/CE Test Viewer";
	std::string script;
	FILE* file = fopen(entry.c_str(), "r");
	if (file != NULL) {
		char buffer[1024];
		size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
		buffer[length] = '\0';
		script = buffer;
		fclose(file);
	}
	report("entry", status == 0 && script.find("exec /bin/linux "
			"ce-test-viewer --open") != std::string::npos
		&& access(entry.c_str(), X_OK) == 0, "%s", output.c_str());

	BNode node(entry.c_str());
	attr_info large, mini;
	bool icons = node.GetAttrInfo("BEOS:L:STD_ICON", &large) == B_OK
		&& node.GetAttrInfo("BEOS:M:STD_ICON", &mini) == B_OK
		&& large.size == 32 * 32 && mini.size == 16 * 16;
	report("icon", png && icons, "PNG written %s, icons %s",
		png ? "yes" : "no", icons ? "32 and 16" : "missing");
	report("hidden", access((std::string(menu.Path())
		+ "/CE Hidden Helper").c_str(), F_OK) != 0, "no entry for NoDisplay");

	// batch 10: Open with
	std::string copier = applications + "/ce-test-copier.desktop";
	write_text(copier, "[Desktop Entry]\nType=Application\n"
		"Name=CE Test Copier\nExec=cp %f /tmp/ce-openwith-copied\n"
		"MimeType=text/x-ce-renguin-test;\n");
	std::string registered = run("/bin/linux-apps sync -v 2>&1 | grep "
		"'^open_with'");
	std::string copierEntry = std::string(menu.Path()) + "/CE Test Copier";
	char signature[B_MIME_TYPE_LENGTH] = "";
	BPath launcherPath;
	find_directory(B_USER_NONPACKAGED_DATA_DIRECTORY, &launcherPath);
	launcherPath.Append("renguin/launchers/ce-test-copier");
	BNode launcherNode(launcherPath.Path());
	launcherNode.ReadAttr("BEOS:APP_SIG", B_MIME_STRING_TYPE, 0, signature,
		sizeof(signature) - 1);
	BMessage supporting;
	BMimeType("text/x-ce-renguin-test").GetSupportingApps(&supporting);
	bool supported = false;
	const char* supportingApp;
	for (int32 i = 0; supporting.FindString("applications", i,
			&supportingApp) == B_OK; i++) {
		if (strcmp(supportingApp, signature) == 0)
			supported = true;
	}
	write_text("/tmp/ce-openwith-source", "renguin open with\n");
	unlink("/tmp/ce-openwith-copied");
	entry_ref sourceRef;
	get_ref_for_path("/tmp/ce-openwith-source", &sourceRef);
	BMessage refs(B_REFS_RECEIVED);
	refs.AddRef("refs", &sourceRef);
	status_t launched = be_roster->Launch(signature, &refs);
	(void)copierEntry;
	bool copied = false;
	for (int i = 0; i < 300 && !copied; i++) {
		snooze(100000);
		copied = run("cat /tmp/ce-openwith-copied 2>/dev/null")
			== "renguin open with\n";
	}
	// Tracker's "Open with" starts the application with a B_REFS_RECEIVED
	// message; the application is the program's launcher
	char appType[B_MIME_TYPE_LENGTH] = "";
	entry_ref launcherRef;
	BMimeType(signature).GetAppHint(&launcherRef);
	BNodeInfo(&launcherNode).GetType(appType);
	report("open_with", strcmp(signature,
			"application/x-vnd.renguin-ce-test-copier") == 0 && supported
			&& strcmp(appType, "application/x-vnd.Be-elfexecutable") == 0
			&& launched == B_OK && copied,
		"signature \"%s\", type %s, supported %d, launch %s, copied %d "
		"[%s]", signature, appType, supported, strerror(launched), copied,
		registered.c_str());
	unlink(copier.c_str());
	unlink("/tmp/ce-openwith-source");
	unlink("/tmp/ce-openwith-copied");

	// the badge
	BPath settingsDir;
	find_directory(B_USER_SETTINGS_DIRECTORY, &settingsDir, true);
	settingsDir.Append("Renguin");
	run((std::string("mkdir -p '") + settingsDir.Path() + "'").c_str());
	std::string settings = std::string(settingsDir.Path()) + "/settings";
	std::string savedSettings = run(("cat '" + settings
		+ "' 2>/dev/null").c_str());
	uint8 badged[32 * 32], plain[32 * 32];
	memset(badged, 0, sizeof(badged));
	memset(plain, 0, sizeof(plain));
	BNode(entry.c_str()).ReadAttr("BEOS:L:STD_ICON", 'ICON', 0, badged,
		sizeof(badged));
	write_text(settings, "badge false\n");
	run("/bin/linux-apps sync 2>&1");
	BNode(entry.c_str()).ReadAttr("BEOS:L:STD_ICON", 'ICON', 0, plain,
		sizeof(plain));
	// the lower right quarter has the badge, the upper left not
	bool lowerDiffers = false, upperSame = true;
	for (int y = 0; y < 32; y++) {
		for (int x = 0; x < 32; x++) {
			bool same = badged[y * 32 + x] == plain[y * 32 + x];
			if (x >= 20 && y >= 20 && !same)
				lowerDiffers = true;
			if (x < 10 && y < 10 && !same)
				upperSame = false;
		}
	}
	report("badge", lowerDiffers && upperSame,
		"badge in the lower right %d, rest unchanged %d", lowerDiffers,
		upperSame);

	// AppImages
	std::string fake = "/tmp/ce-test.AppImage";
	run(("cp /boot/system/non-packaged/data/renguin-tests/appimage-fake '"
		+ fake + "' && chmod -x '" + fake + "'").c_str());
	FILE* image = fopen(fake.c_str(), "r+");
	if (image != NULL) {
		fseek(image, 8, SEEK_SET);
		fwrite("AI\002", 1, 3, image);
		fclose(image);
	}
	BMimeType guessed;
	BMimeType::GuessMimeType(fake.c_str(), &guessed);
	char preferred[B_MIME_TYPE_LENGTH] = "";
	BMimeType("application/x-appimage").GetPreferredApp(preferred);
	unlink("/tmp/ce-appimage-ran");
	entry_ref fakeRef;
	get_ref_for_path(fake.c_str(), &fakeRef);
	// a double-click in Tracker: the document is launched
	status_t opened = be_roster->Launch(&fakeRef);
	std::string ran;
	for (int i = 0; i < 300 && ran.empty(); i++) {
		snooze(100000);
		ran = run("cat /tmp/ce-appimage-ran 2>/dev/null");
	}
	std::string direct;
	if (ran.empty()) {
		// diagnosis: the same without the roster
		direct = run(("timeout 60 /bin/linux-apps '" + fake
			+ "' </dev/null >/dev/null 2>&1; echo rc=$?").c_str());
		for (int i = 0; i < 200 && ran.empty(); i++) {
			snooze(100000);
			ran = run("cat /tmp/ce-appimage-ran 2>/dev/null");
		}
		direct += " direct: " + ran;
		ran.clear();
	}
	report("appimage", guessed.Type() != NULL
			&& strcmp(guessed.Type(), "application/x-appimage") == 0
			&& strcmp(preferred, "application/x-vnd.Renga-linux-apps") == 0
			&& opened == B_OK && ran.find("extract=1 appimage=" + fake)
				!= std::string::npos,
		"type %s, preferred %s, open %s, ran [%s]%s",
		guessed.Type() != NULL ? guessed.Type() : "?", preferred,
		strerror(opened), ran.c_str(), direct.c_str());
	unlink(fake.c_str());
	unlink("/tmp/ce-appimage-ran");

	// the theme
	BPath home;
	find_directory(B_USER_DIRECTORY, &home);
	std::string config = std::string(home.Path()) + "/.config";
	unlink(settings.c_str());
	run("/bin/linux-apps theme 2>&1");
	std::string gtk3 = run(("cat '" + config + "/gtk-3.0/gtk.css' "
		"2>/dev/null").c_str());
	std::string ini = run(("cat '" + config + "/gtk-4.0/settings.ini' "
		"2>/dev/null").c_str());
	std::string qt = run(("cat '" + config + "/qt6ct/colors/Renga.conf' "
		"2>/dev/null").c_str());
	bool written = gtk3.find("Written by Renga") != std::string::npos
		&& gtk3.find("@define-color accent_bg_color #") != std::string::npos
		&& ini.find("gtk-application-prefer-dark-theme=") != std::string::npos
		&& qt.find("active_colors=#ff") != std::string::npos;
	write_text(settings, "theme false\n");
	run("/bin/linux-apps theme 2>&1");
	bool removed = access((config + "/gtk-3.0/gtk.css").c_str(), F_OK) != 0
		&& access((config + "/qt6ct/qt6ct.conf").c_str(), F_OK) != 0;
	report("theme", written && removed, "written %d, removed with the "
		"switch off %d", written, removed);
	if (savedSettings.empty())
		unlink(settings.c_str());
	else
		write_text(settings, savedSettings.c_str());

	unlink(visible.c_str());
	run("/bin/linux-apps sync 2>&1");
	report("removed", access(entry.c_str(), F_OK) != 0,
		"entry gone after its .desktop file");
	report("user_kept", access(userEntry.c_str(), F_OK) == 0,
		"the user's own entry stays");

	unlink(hidden.c_str());
	unlink((pixmaps + "/ce-test-viewer.png").c_str());
	unlink(userEntry.c_str());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
