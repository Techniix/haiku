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
	  linux_apps.hidpi       (batch 11) the scale from Renga's font size,
	                         GDK_SCALE/GDK_DPI_SCALE/QT_SCALE_FACTOR from it
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

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <initializer_list>
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


//!	At most 256 bytes of \a path (in this process: a native cat polling the
//!	file in a loop was found spinning in full renga-raw runs).
static std::string
read_small(const char* path)
{
	char buffer[257];
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return "";
	ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (length <= 0)
		return "";
	return std::string(buffer, length);
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
		// read in-process: a polling native cat spun in full renga-raw
		// runs (batch 10)
		copied = read_small("/tmp/ce-openwith-copied")
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

	// AppImages. In full renga-raw runs (not alone) the test stalled here
	// twice with its thread running (batch 10, not yet understood); a
	// watchdog reports that as this check's failure and ends the test, so
	// the suites after it still run.
	static volatile bool sAppImageDone = false;
	pthread_t watchdog;
	pthread_create(&watchdog, NULL, [](void*) -> void* {
		for (int i = 0; i < 1200 && !sAppImageDone; i++)
			snooze(100000);
		if (!sAppImageDone) {
			printf("CE-TEST %s.appimage FAIL stalled for 120 s (opening the "
				"AppImage through the roster)\n", SUITE);
			printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures + 1);
			fflush(stdout);
			_exit(1);
		}
		return NULL;
	}, NULL);
	std::string fake = "/tmp/ce-test.AppImage";
	run(("cp /boot/system/non-packaged/data/renguin-tests/appimage-fake '"
		+ fake + "' && chmod -x '" + fake + "'").c_str());
	FILE* image = fopen(fake.c_str(), "r+");
	if (image != NULL) {
		fseek(image, 8, SEEK_SET);
		fwrite("AI\002", 1, 3, image);
		fclose(image);
	}
	// batch 11: as a downloaded AppImage gets it: the type from sniffing,
	// not the attribute cp copied from the test program (an ELF
	// executable's type, which made the roster launch the file itself)
	run(("rmattr BEOS:TYPE '" + fake + "' 2>/dev/null; mimeset -f '" + fake
		+ "'").c_str());
	char fileType[B_MIME_TYPE_LENGTH] = "";
	{
		BNode fakeNode(fake.c_str());
		BNodeInfo(&fakeNode).GetType(fileType);
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
		ran = read_small("/tmp/ce-appimage-ran");
	}
	std::string direct;
	if (ran.empty()) {
		// diagnosis: the same without the roster
		direct = run(("timeout 60 /bin/linux-apps '" + fake
			+ "' </dev/null >/dev/null 2>&1; echo rc=$?").c_str());
		// batch 11: what the program itself says, run as linux-apps runs it
		std::string said = run(("timeout 30 /bin/linux env "
			"APPIMAGE_EXTRACT_AND_RUN=1 APPIMAGE='" + fake + "' '" + fake
			+ "' </dev/null 2>&1 | head -c 200 | tr '\\n' ' '; echo "
			"rc=$?").c_str());
		direct += " linux env: " + said;
		for (int i = 0; i < 200 && ran.empty(); i++) {
			snooze(100000);
			ran = read_small("/tmp/ce-appimage-ran");
		}
		direct += " direct: " + ran;
		// one line: the harness keeps a check's first line only
		for (size_t i = 0; i < direct.size(); i++) {
			if (direct[i] == '\n')
				direct[i] = ' ';
		}
		ran.clear();
	}
	report("appimage", guessed.Type() != NULL
			&& strcmp(guessed.Type(), "application/x-appimage") == 0
			&& strcmp(fileType, "application/x-appimage") == 0
			&& strcmp(preferred, "application/x-vnd.Renga-linux-apps") == 0
			&& opened == B_OK && ran.find("extract=1 appimage=" + fake)
				!= std::string::npos,
		"type %s (file %s), preferred %s, open %s, ran [%s]%s",
		guessed.Type() != NULL ? guessed.Type() : "?", fileType, preferred,
		strerror(opened), ran.c_str(), direct.c_str());
	sAppImageDone = true;
	pthread_join(watchdog, NULL);
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

	// batch 11: HiDPI. The scale from the font size (12 px is 1.00), and
	// what "linux" makes of a scale of 2
	unlink(settings.c_str());
	run("/bin/linux-apps theme 2>&1");
	std::string scale = read_small((config + "/renguin/scale").c_str());
	write_text(config + "/renguin/scale", "2.00\n");
	std::string scaled = run("/bin/linux env 2>&1 | grep -E "
		"'^(GDK_SCALE|GDK_DPI_SCALE|QT_SCALE_FACTOR)=' | sort | tr '\\n' ' '");
	write_text(config + "/renguin/scale", "1.00\n");
	std::string unscaled = run("/bin/linux env 2>&1 | grep -c SCALE");
	write_text(settings, "hidpi false\n");
	run("/bin/linux-apps theme 2>&1");
	bool hidpiOff = access((config + "/renguin/scale").c_str(), F_OK) != 0;
	report("hidpi", !scale.empty() && scaled == "GDK_DPI_SCALE=1.000 "
			"GDK_SCALE=2 QT_SCALE_FACTOR=2.00 " && unscaled == "0\n" && hidpiOff,
		"scale file \"%s\"; at 2.00: [%s]; at 1.00: %s variables; switch "
		"off removes the file %d", scale.c_str(), scaled.c_str(),
		unscaled.c_str(), hidpiOff);
	// batch 11: a normal user, root on request
	auto oneLine = [](std::string text) {
		for (size_t i = 0; i < text.size(); i++) {
			if (text[i] == '\n')
				text[i] = '/';
		}
		return text;
	};
	std::string uid = run("/bin/linux id -u 2>&1");
	std::string fileOwner = run("/bin/linux stat -c %u /boot/home 2>&1");
	std::string rootUid = run("/bin/linux --root id -u 2>&1");
	std::string sudoUid = run("/bin/linux sudo id -u 2>&1");
	std::string passwd = run("grep '^user:' "
		"/boot/system/non-packaged/compat/linux/etc/passwd");
	write_text(settings, "linux_user false\n");
	std::string switchedOff = run("/bin/linux id -u 2>&1");
	unlink(settings.c_str());
	for (std::string* text : { &uid, &fileOwner, &rootUid, &sudoUid,
			&switchedOff }) {
		for (size_t i = 0; i < text->size(); i++) {
			if ((*text)[i] == '\n')
				(*text)[i] = ' ';
		}
	}
	report("linux_user", uid == "1000 " && fileOwner == "1000 "
			&& rootUid == "0 " && sudoUid == "0 "
			&& passwd.find("user:x:1000:1000:") != std::string::npos,
		"id -u %s, home's owner %s, --root %s, sudo %s, passwd entry %d "
		"(switch off in the user settings: %s)",
		uid.c_str(), fileOwner.c_str(), rootUid.c_str(), sudoUid.c_str(),
		passwd.find("user:x:1000:1000:") != std::string::npos,
		switchedOff.c_str());

	// batch 11: Chromium's flags under Renguin (no sandbox, zygote or GPU
	// process yet), for the root's Chromium launcher
	const char* chromiumDir
		= "/boot/system/non-packaged/compat/linux/usr/lib/chromium";
	bool madeDir = mkdir(chromiumDir, 0755) == 0;
	run((std::string("rm -f ") + home.Path()
		+ "/.config/renguin/chromium-noted").c_str());
	run("/bin/linux true 2>&1");
	std::string first = run("/bin/linux sh -c '. /etc/chromium/renguin.conf; "
		"echo $CHROMIUM_FLAGS' 2>&1");
	std::string second = run("/bin/linux sh -c '. /etc/chromium/renguin.conf;"
		" echo $CHROMIUM_FLAGS' 2>&1");
	write_text(settings, "chromium_flags false\n");
	run("/bin/linux true 2>&1");
	bool confGone = access(
		"/boot/system/non-packaged/compat/linux/etc/chromium/renguin.conf",
		F_OK) != 0;
	unlink(settings.c_str());
	if (madeDir) {
		rmdir(chromiumDir);
		run("/bin/linux true 2>&1");
	}
	bool flags = first.find("--no-sandbox --no-zygote --in-process-gpu "
		"--disable-gpu --ozone-platform=wayland") != std::string::npos;
	report("chromium_flags", flags
			&& first.find("Renguin: Chromium runs with") != std::string::npos
			&& second.find("Renguin: Chromium runs") == std::string::npos
			&& confGone,
		"flags %d, note once %d/%d, switch off removes the file %d [%s]",
		flags,
		first.find("Renguin: Chromium runs with") != std::string::npos,
		second.find("Renguin: Chromium runs") == std::string::npos,
		confGone, oneLine(first).c_str());

	run("/bin/linux-apps theme 2>&1");
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
