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

	Everything it adds is removed again.
*/


#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <File.h>
#include <FindDirectory.h>
#include <Node.h>
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
