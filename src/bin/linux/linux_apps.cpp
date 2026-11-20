/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	linux-apps (Renga, batch 5; Linux programs that feel native): Deskbar
	entries for the Linux programs installed in Renguin's Linux root, made
	from their freedesktop .desktop files.

	For every usr/share/applications/<id>.desktop of the Linux root that
	is an Application, not hidden (NoDisplay, Hidden) and not meant for
	other desktops only (OnlyShowIn), an entry
	~/config/settings/deskbar/menu/Applications/Linux/<Name> is written: a
	small shell script that runs the Exec line through "linux" (in a
	Terminal for Terminal=true programs), with the program's name and its
	icon (the theme's PNG at 32, 48, 64 or 128 pixels, or pixmaps/,
	scaled to Renga's 16 and 32 pixel icons) on the file, and the
	"renga:linux" attribute naming the .desktop file. Entries whose
	.desktop file is gone are removed; entries without the attribute are
	never touched.

	  linux-apps sync      update the entries (the login job and Renguin
	                       Manager's button run this)
	  linux-apps list      what sync would write: "<id>\t<name>\t<exec>"

	Field codes in Exec (%f %F %u %U %i %c %k) are dropped: the Deskbar
	starts the program without files.
*/


#include <Application.h>
#include <Bitmap.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <TranslationUtils.h>
#include <View.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>


static const char* kLinuxRoot = "/boot/system/non-packaged/compat/linux";
static const char* kAttribute = "renga:linux";


struct DesktopEntry {
	std::string	id;
	std::string	path;
	std::string	name;
	std::string	exec;
	std::string	icon;
	bool		terminal;
};


static std::string
trim(const std::string& text)
{
	size_t start = text.find_first_not_of(" \t\r\n");
	size_t end = text.find_last_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	return text.substr(start, end - start + 1);
}


/*!	Reads the [Desktop Entry] group of \a path. Returns false if it is not
	a visible application.
*/
static bool
read_entry(const char* path, DesktopEntry& entry)
{
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return false;
	std::map<std::string, std::string> keys;
	bool inGroup = false;
	char line[4096];
	while (fgets(line, sizeof(line), file) != NULL) {
		std::string text = trim(line);
		if (text.empty() || text[0] == '#')
			continue;
		if (text[0] == '[') {
			inGroup = text == "[Desktop Entry]";
			continue;
		}
		size_t equals = text.find('=');
		if (!inGroup || equals == std::string::npos)
			continue;
		std::string key = trim(text.substr(0, equals));
		// localised keys (Name[de]) are not used: Renga's own language
		// mapping comes later
		if (key.find('[') != std::string::npos)
			continue;
		keys[key] = trim(text.substr(equals + 1));
	}
	fclose(file);

	if (keys["Type"] != "Application" || keys["NoDisplay"] == "true"
		|| keys["Hidden"] == "true" || keys["Name"].empty()
		|| keys["Exec"].empty())
		return false;
	if (!keys["OnlyShowIn"].empty())
		return false;

	entry.path = path;
	entry.name = keys["Name"];
	entry.icon = keys["Icon"];
	entry.terminal = keys["Terminal"] == "true";

	// drop the field codes, keep "%%" as "%"
	std::string exec;
	const std::string& raw = keys["Exec"];
	for (size_t i = 0; i < raw.size(); i++) {
		if (raw[i] == '%' && i + 1 < raw.size()) {
			if (raw[i + 1] == '%')
				exec += '%';
			i++;
			continue;
		}
		exec += raw[i];
	}
	entry.exec = trim(exec);
	return !entry.exec.empty();
}


static std::vector<DesktopEntry>
find_entries()
{
	std::vector<DesktopEntry> entries;
	std::string directory = std::string(kLinuxRoot)
		+ "/usr/share/applications";
	DIR* dir = opendir(directory.c_str());
	if (dir == NULL)
		return entries;
	while (dirent* item = readdir(dir)) {
		const char* name = item->d_name;
		size_t length = strlen(name);
		if (length < 9 || strcmp(name + length - 8, ".desktop") != 0)
			continue;
		DesktopEntry entry;
		entry.id = std::string(name, length - 8);
		if (read_entry((directory + "/" + name).c_str(), entry))
			entries.push_back(entry);
	}
	closedir(dir);
	return entries;
}


//!	The PNG for \a icon in the Linux root, or "".
static std::string
find_icon(const std::string& icon)
{
	if (icon.empty())
		return "";
	struct stat st;
	if (icon[0] == '/') {
		std::string path = std::string(kLinuxRoot) + icon;
		return stat(path.c_str(), &st) == 0 ? path : "";
	}
	const char* sizes[] = { "32x32", "48x48", "64x64", "128x128", "256x256",
		"24x24", "16x16" };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		std::string path = std::string(kLinuxRoot)
			+ "/usr/share/icons/hicolor/" + sizes[i] + "/apps/" + icon
			+ ".png";
		if (stat(path.c_str(), &st) == 0)
			return path;
	}
	std::string path = std::string(kLinuxRoot) + "/usr/share/pixmaps/"
		+ icon + ".png";
	return stat(path.c_str(), &st) == 0 ? path : "";
}


//!	\a source scaled to \a size, converted to CMAP8, set as that icon.
static void
set_icon(BNode& node, BBitmap* source, int32 size)
{
	BRect bounds(0, 0, size - 1, size - 1);
	BBitmap scaled(bounds, B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
	BView* view = new BView(bounds, "scale", 0, 0);
	scaled.AddChild(view);
	if (scaled.Lock()) {
		view->SetHighColor(0, 0, 0, 0);
		view->FillRect(bounds);
		view->SetDrawingMode(B_OP_ALPHA);
		view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		view->DrawBitmap(source, source->Bounds(), bounds,
			B_FILTER_BITMAP_BILINEAR);
		view->Sync();
		scaled.Unlock();
	}
	BBitmap cmap(bounds, 0, B_CMAP8);
	if (BIconUtils::ConvertToCMAP8(&scaled, &cmap) != B_OK)
		return;
	BNodeInfo info(&node);
	info.SetIcon(&cmap, size == 16 ? B_MINI_ICON : B_LARGE_ICON);
}


static std::string
safe_name(const std::string& name)
{
	std::string result;
	for (size_t i = 0; i < name.size(); i++)
		result += name[i] == '/' ? '-' : name[i];
	return result;
}


static std::string
quote(const std::string& text)
{
	std::string result = "'";
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\'')
			result += "'\\''";
		else
			result += text[i];
	}
	return result + "'";
}


static int
sync(bool verbose)
{
	BPath menu;
	if (find_directory(B_USER_DESKBAR_DIRECTORY, &menu, true) != B_OK)
		return 1;
	menu.Append("Applications/Linux");
	create_directory(menu.Path(), 0755);

	std::vector<DesktopEntry> entries = find_entries();
	std::set<std::string> written;
	for (size_t i = 0; i < entries.size(); i++) {
		const DesktopEntry& entry = entries[i];
		std::string fileName = safe_name(entry.name);
		std::string path = std::string(menu.Path()) + "/" + fileName;

		// an entry the user made (no attribute) is never replaced
		BNode existing(path.c_str());
		char attribute[1024];
		if (existing.InitCheck() == B_OK && existing.ReadAttr(kAttribute,
				B_STRING_TYPE, 0, attribute, sizeof(attribute)) <= 0)
			continue;

		std::string script = "#!/bin/sh\n# Made by linux-apps from "
			+ entry.path + "\n";
		if (entry.terminal) {
			script += "exec /boot/system/apps/Terminal -t " + quote(entry.name)
				+ " /bin/linux " + entry.exec + "\n";
		} else
			script += "exec /bin/linux " + entry.exec + "\n";
		FILE* file = fopen(path.c_str(), "w");
		if (file == NULL)
			continue;
		fputs(script.c_str(), file);
		fclose(file);
		chmod(path.c_str(), 0755);

		BNode node(path.c_str());
		node.WriteAttr(kAttribute, B_STRING_TYPE, 0, entry.path.c_str(),
			entry.path.size() + 1);
		BNodeInfo(&node).SetType("text/x-source-code");
		std::string iconPath = find_icon(entry.icon);
		if (!iconPath.empty()) {
			BBitmap* bitmap = BTranslationUtils::GetBitmap(iconPath.c_str());
			if (bitmap != NULL) {
				set_icon(node, bitmap, 16);
				set_icon(node, bitmap, 32);
				delete bitmap;
			}
		}
		written.insert(fileName);
		if (verbose) {
			printf("%s\t%s\t%s\n", entry.id.c_str(), entry.name.c_str(),
				iconPath.empty() ? "(no icon)" : iconPath.c_str());
		}
	}

	// entries of programs that are gone
	BDirectory directory(menu.Path());
	BEntry item;
	while (directory.GetNextEntry(&item) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		item.GetName(name);
		BNode node(&item);
		char attribute[1024];
		if (node.ReadAttr(kAttribute, B_STRING_TYPE, 0, attribute,
				sizeof(attribute)) > 0 && written.count(name) == 0) {
			item.Remove();
			if (verbose)
				printf("removed\t%s\n", name);
		}
	}
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "list") == 0) {
		std::vector<DesktopEntry> entries = find_entries();
		for (size_t i = 0; i < entries.size(); i++) {
			printf("%s\t%s\t%s\n", entries[i].id.c_str(),
				entries[i].name.c_str(), entries[i].exec.c_str());
		}
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "sync") == 0) {
		// scaling the icons draws, which needs app_server
		BApplication app("application/x-vnd.Renga-linux-apps");
		return sync(argc == 3 && strcmp(argv[2], "-v") == 0);
	}

	fprintf(stderr, "usage: linux-apps sync [-v] | list\n");
	return 1;
}
