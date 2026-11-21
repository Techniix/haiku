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
	  linux-apps theme     write the GTK 3/4 and Qt colours from Renga's
	                       appearance (see write_theme())
	  linux-apps <file>... run AppImages (Tracker opens them with this)

	Batch 10 (F-27, Linux programs integration), each switchable in the
	Renguin Manager (~/config/settings/Renguin/settings):
	  - Open with ("open_with", on): every entry is also an application
	    Tracker knows: an application signature per program
	    (application/x-vnd.renguin-<id>), the .desktop file's MimeType list
	    as its supported types in the MIME database, and B_ARGV_ONLY, so
	    "Open with" lists Linux programs by type and hands them the files
	    on the command line. File field codes (%f %F %u %U) become the
	    files; the others are dropped.
	  - The Renguin badge ("badge", on): a leaf with a penguin
	    (RenguinBadge.h, HVIF) drawn into the lower right of the entry's
	    icon, so Linux programs are told apart in the Deskbar and Tracker.
	  - AppImages ("appimage", on): application/x-appimage (sniffed by the
	    "AI" magic after the ELF header) opens with this program, which
	    marks the file executable and runs it through "linux" with
	    APPIMAGE_EXTRACT_AND_RUN=1: the AppImage runtime unpacks itself into
	    /tmp instead of mounting itself with FUSE, which Renguin does not
	    have.
*/


#include <Alert.h>
#include <AppDefs.h>
#include <Roster.h>
#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <InterfaceDefs.h>
#include <MimeType.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <TranslationUtils.h>
#include <View.h>

#include <driver_settings.h>

#include <ctype.h>
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


#include "RenguinBadge.h"


static const char* kLinuxRoot = "/boot/system/non-packaged/compat/linux";
static const char* kAttribute = "renga:linux";
static const char* kSignaturePrefix = "application/x-vnd.renguin-";


/*!	A switch of ~/config/settings/Renguin/settings (driver settings, written
	by the Renguin Manager).
*/
static bool
renguin_setting(const char* name, bool defaultValue)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return defaultValue;
	path.Append("Renguin/settings");
	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return defaultValue;
	bool value = get_driver_boolean_parameter(handle, name, defaultValue,
		true);
	unload_driver_settings(handle);
	return value;
}


struct DesktopEntry {
	std::string	id;
	std::string	path;
	std::string	name;
	std::string	exec;
	std::string	icon;
	bool		terminal;
	bool		takesFiles;		// Exec had %f, %F, %u or %U
	std::vector<std::string> mimeTypes;
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
	entry.takesFiles = false;
	const std::string& types = keys["MimeType"];
	size_t start = 0;
	while (start < types.size()) {
		size_t end = types.find(';', start);
		if (end == std::string::npos)
			end = types.size();
		std::string type = trim(types.substr(start, end - start));
		if (!type.empty() && type.find('/') != std::string::npos)
			entry.mimeTypes.push_back(type);
		start = end + 1;
	}

	// the file field codes become the files ("$@" in the entry's script),
	// the others are dropped; "%%" is "%"
	std::string exec;
	const std::string& raw = keys["Exec"];
	for (size_t i = 0; i < raw.size(); i++) {
		if (raw[i] == '%' && i + 1 < raw.size()) {
			char code = raw[i + 1];
			if (code == '%')
				exec += '%';
			else if ((code == 'f' || code == 'F' || code == 'u'
					|| code == 'U') && !entry.takesFiles) {
				exec += "\"$@\"";
				entry.takesFiles = true;
			}
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


/*!	\a source (or nothing) scaled to \a size, with the Renguin badge in
	the lower right when \a badge (the whole icon when there is no source),
	converted to CMAP8, set as that icon.
*/
static void
set_icon(BNode& node, BBitmap* source, int32 size, bool badge)
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
		if (source != NULL) {
			view->DrawBitmap(source, source->Bounds(), bounds,
				B_FILTER_BITMAP_BILINEAR);
		}
		if (badge) {
			// the badge is vector art: rendered at the size it is shown,
			// so it stays sharp at every icon size
			int32 badgeSize = source != NULL ? size * 9 / 16 : size;
			BBitmap badgeBitmap(BRect(0, 0, badgeSize - 1, badgeSize - 1),
				B_RGBA32);
			if (BIconUtils::GetVectorIcon(kRenguinBadge,
					sizeof(kRenguinBadge), &badgeBitmap) == B_OK) {
				view->DrawBitmap(&badgeBitmap, BPoint(size - badgeSize,
					size - badgeSize));
			}
		}
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


//!	The application signature of the Linux program \a id.
static std::string
program_signature(const std::string& id)
{
	std::string signature = kSignaturePrefix;
	for (size_t i = 0; i < id.size() && signature.size() < B_MIME_TYPE_LENGTH
			- 1; i++) {
		char c = id[i];
		signature += (isalnum((unsigned char)c) || c == '.' || c == '-'
			|| c == '_') ? (char)tolower((unsigned char)c) : '-';
	}
	return signature;
}


/*!	Makes the entry at \a path an application Tracker's "Open with" offers
	for \a entry's types (or takes that away again, \a enable false).
*/
static void
register_program(const char* path, const DesktopEntry& entry, bool enable)
{
	BFile file(path, B_READ_WRITE);
	if (file.InitCheck() != B_OK)
		return;
	std::string signature = program_signature(entry.id);
	BMimeType type(signature.c_str());
	if (!enable || entry.mimeTypes.empty() || !entry.takesFiles) {
		file.RemoveAttr("BEOS:APP_SIG");
		file.RemoveAttr("BEOS:FILE_TYPES");
		file.RemoveAttr("BEOS:APP_FLAGS");
		if (type.IsInstalled())
			type.Delete();
		return;
	}
	if (!type.IsInstalled())
		type.Install();
	type.SetShortDescription(entry.name.c_str());
	entry_ref ref;
	if (get_ref_for_path(path, &ref) == B_OK)
		type.SetAppHint(&ref);

	BAppFileInfo info(&file);
	info.SetInfoLocation(B_USE_ATTRIBUTES);
	info.SetSignature(signature.c_str());
	info.SetAppFlags(B_MULTIPLE_LAUNCH | B_ARGV_ONLY);
	BMessage types;
	for (size_t i = 0; i < entry.mimeTypes.size(); i++)
		types.AddString("types", entry.mimeTypes[i].c_str());
	// syncAll: the MIME database learns that this program supports them
	info.SetSupportedTypes(&types, true, true);
}


static int
sync(bool verbose)
{
	bool badge = renguin_setting("badge", true);
	bool openWith = renguin_setting("open_with", true);

	BPath menu;
	if (find_directory(B_USER_DESKBAR_DIRECTORY, &menu, true) != B_OK)
		return 1;
	menu.Append("Applications/Linux");
	create_directory(menu.Path(), 0755);

	std::vector<DesktopEntry> entries = find_entries();
	std::set<std::string> written;
	for (size_t i = 0; i < entries.size(); i++) {
		DesktopEntry entry = entries[i];
		// the Renguin Manager's per-program settings
		BPath programSettings;
		bool hidden = false;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &programSettings)
				== B_OK) {
			programSettings.Append("Renguin/programs");
			programSettings.Append(entry.id.c_str());
			if (void* handle = load_driver_settings(programSettings.Path())) {
				hidden = get_driver_boolean_parameter(handle, "hidden", false,
					true);
				const char* options = get_driver_parameter(handle, "options",
					NULL, NULL);
				if (options != NULL && options[0] != '\0')
					entry.exec += std::string(" ") + options;
				unload_driver_settings(handle);
			}
		}
		if (hidden)
			continue;
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
		BBitmap* bitmap = iconPath.empty() ? NULL
			: BTranslationUtils::GetBitmap(iconPath.c_str());
		if (bitmap != NULL || badge) {
			set_icon(node, bitmap, 16, badge);
			set_icon(node, bitmap, 32, badge);
		} else {
			node.RemoveAttr("BEOS:M:STD_ICON");
			node.RemoveAttr("BEOS:L:STD_ICON");
		}
		delete bitmap;
		node.Unset();
		register_program(path.c_str(), entry, openWith);
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
			char signature[B_MIME_TYPE_LENGTH];
			if (node.ReadAttr("BEOS:APP_SIG", B_MIME_STRING_TYPE, 0, signature,
					sizeof(signature)) > 0) {
				signature[sizeof(signature) - 1] = '\0';
				BMimeType type(signature);
				if (strncmp(signature, kSignaturePrefix,
						strlen(kSignaturePrefix)) == 0 && type.IsInstalled()) {
					type.Delete();
				}
			}
			item.Remove();
			if (verbose)
				printf("removed\t%s\n", name);
		}
	}
	return 0;
}


// #pragma mark - theme


static const char* kThemeMarker
	= "Written by Renga (linux-apps theme) from Renga's appearance";


static std::string
hex_color(rgb_color color)
{
	char text[16];
	snprintf(text, sizeof(text), "#%02x%02x%02x", color.red, color.green,
		color.blue);
	return text;
}


static std::string
qt_color(rgb_color color)
{
	char text[16];
	snprintf(text, sizeof(text), "#ff%02x%02x%02x", color.red, color.green,
		color.blue);
	return text;
}


static rgb_color
mix(rgb_color a, rgb_color b, float amount)
{
	return make_color((uint8)(a.red + (b.red - a.red) * amount),
		(uint8)(a.green + (b.green - a.green) * amount),
		(uint8)(a.blue + (b.blue - a.blue) * amount));
}


/*!	Writes \a text to \a path, unless the file is the user's own (exists
	without the marker line). With \a text NULL it removes a file this
	program wrote.
*/
static void
write_marked(const std::string& path, const char* text)
{
	FILE* existing = fopen(path.c_str(), "r");
	if (existing != NULL) {
		char line[512] = "";
		bool ours = false;
		for (int i = 0; i < 3 && fgets(line, sizeof(line), existing) != NULL;
				i++) {
			if (strstr(line, kThemeMarker) != NULL)
				ours = true;
		}
		fclose(existing);
		if (!ours)
			return;
	}
	if (text == NULL) {
		unlink(path.c_str());
		return;
	}
	std::string directory = path.substr(0, path.rfind('/'));
	create_directory(directory.c_str(), 0755);
	FILE* file = fopen(path.c_str(), "w");
	if (file == NULL)
		return;
	fputs(text, file);
	fclose(file);
}


/*!	GTK 3, GTK 4 and Qt 5/6 (qt5ct/qt6ct) colours and font from Renga's
	appearance, in the Linux home (Renga's home): settings.ini (dark
	preference, font), gtk.css (Adwaita's and libadwaita's named colours:
	window, view, selection, accent), and qt5ct/qt6ct colour schemes with
	their configuration. Files the user wrote (no marker line) are left
	alone. GTK 4 and Qt 6 programs also follow light/dark and the accent
	live through Renga's portal (org.freedesktop.appearance); this adds the
	exact colours. "theme false" in the Renguin settings removes the files.
*/
static int
write_theme()
{
	BPath home;
	if (find_directory(B_USER_DIRECTORY, &home) != B_OK)
		return 1;
	std::string config = std::string(home.Path()) + "/.config";
	const char* files[] = { "/gtk-3.0/settings.ini", "/gtk-3.0/gtk.css",
		"/gtk-4.0/settings.ini", "/gtk-4.0/gtk.css",
		"/qt5ct/colors/Renga.conf", "/qt5ct/qt5ct.conf",
		"/qt6ct/colors/Renga.conf", "/qt6ct/qt6ct.conf" };
	if (!renguin_setting("theme", true)) {
		for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
			write_marked(config + files[i], NULL);
		return 0;
	}

	rgb_color window = ui_color(B_PANEL_BACKGROUND_COLOR);
	rgb_color windowText = ui_color(B_PANEL_TEXT_COLOR);
	rgb_color view = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	rgb_color viewText = ui_color(B_DOCUMENT_TEXT_COLOR);
	rgb_color accent = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
	rgb_color selected = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
	rgb_color selectedText = ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR);
	rgb_color link = ui_color(B_LINK_TEXT_COLOR);
	rgb_color tooltip = ui_color(B_TOOL_TIP_BACKGROUND_COLOR);
	rgb_color tooltipText = ui_color(B_TOOL_TIP_TEXT_COLOR);
	bool dark = window.IsDark();
	rgb_color accentText = accent.IsDark() ? make_color(255, 255, 255)
		: make_color(0, 0, 0);

	font_family family;
	font_style style;
	be_plain_font->GetFamilyAndStyle(&family, &style);
	// Renga's sizes are pixels; GTK and Qt take points at 96 dpi
	int points = (int)(be_plain_font->Size() * 0.75f + 0.5f);
	if (points < 6)
		points = 9;

	char text[4096];
	snprintf(text, sizeof(text), "# %s\n[Settings]\n"
		"gtk-application-prefer-dark-theme=%d\n"
		"gtk-font-name=%s %d\n", kThemeMarker, dark ? 1 : 0, family, points);
	write_marked(config + "/gtk-3.0/settings.ini", text);
	write_marked(config + "/gtk-4.0/settings.ini", text);

	snprintf(text, sizeof(text), "/* %s */\n"
		"@define-color theme_bg_color %s;\n"
		"@define-color theme_fg_color %s;\n"
		"@define-color theme_base_color %s;\n"
		"@define-color theme_text_color %s;\n"
		"@define-color theme_selected_bg_color %s;\n"
		"@define-color theme_selected_fg_color %s;\n"
		"@define-color accent_color %s;\n"
		"@define-color accent_bg_color %s;\n"
		"@define-color accent_fg_color %s;\n"
		"@define-color window_bg_color %s;\n"
		"@define-color window_fg_color %s;\n"
		"@define-color view_bg_color %s;\n"
		"@define-color view_fg_color %s;\n"
		"@define-color headerbar_bg_color %s;\n"
		"@define-color headerbar_fg_color %s;\n"
		"@define-color link_color %s;\n",
		kThemeMarker, hex_color(window).c_str(),
		hex_color(windowText).c_str(), hex_color(view).c_str(),
		hex_color(viewText).c_str(), hex_color(selected).c_str(),
		hex_color(selectedText).c_str(), hex_color(accent).c_str(),
		hex_color(accent).c_str(), hex_color(accentText).c_str(),
		hex_color(window).c_str(), hex_color(windowText).c_str(),
		hex_color(view).c_str(), hex_color(viewText).c_str(),
		hex_color(mix(window, windowText, 0.06f)).c_str(),
		hex_color(windowText).c_str(), hex_color(link).c_str());
	write_marked(config + "/gtk-3.0/gtk.css", text);
	write_marked(config + "/gtk-4.0/gtk.css", text);

	// qt5ct/qt6ct: the QPalette roles in their order
	rgb_color light = mix(window, make_color(255, 255, 255), 0.5f);
	rgb_color midlight = mix(window, make_color(255, 255, 255), 0.25f);
	rgb_color darkShade = mix(window, make_color(0, 0, 0), 0.4f);
	rgb_color mid = mix(window, make_color(0, 0, 0), 0.2f);
	rgb_color shadow = mix(window, make_color(0, 0, 0), 0.7f);
	rgb_color alternate = mix(view, windowText, 0.04f);
	rgb_color placeholder = mix(view, viewText, 0.5f);
	rgb_color disabledText = mix(window, windowText, 0.45f);
	std::string active = qt_color(windowText) + ", " + qt_color(window)
		+ ", " + qt_color(light) + ", " + qt_color(midlight) + ", "
		+ qt_color(darkShade) + ", " + qt_color(mid) + ", "
		+ qt_color(viewText) + ", " + qt_color(make_color(255, 255, 255))
		+ ", " + qt_color(windowText) + ", " + qt_color(view) + ", "
		+ qt_color(window) + ", " + qt_color(shadow) + ", "
		+ qt_color(selected) + ", " + qt_color(selectedText) + ", "
		+ qt_color(link) + ", " + qt_color(link) + ", "
		+ qt_color(alternate) + ", " + qt_color(window) + ", "
		+ qt_color(tooltip) + ", " + qt_color(tooltipText) + ", "
		+ qt_color(placeholder);
	std::string disabled = qt_color(disabledText) + ", " + qt_color(window)
		+ ", " + qt_color(light) + ", " + qt_color(midlight) + ", "
		+ qt_color(darkShade) + ", " + qt_color(mid) + ", "
		+ qt_color(disabledText) + ", " + qt_color(make_color(255, 255, 255))
		+ ", " + qt_color(disabledText) + ", " + qt_color(window) + ", "
		+ qt_color(window) + ", " + qt_color(shadow) + ", "
		+ qt_color(mix(selected, window, 0.5f)) + ", "
		+ qt_color(selectedText) + ", " + qt_color(link) + ", "
		+ qt_color(link) + ", " + qt_color(alternate) + ", "
		+ qt_color(window) + ", " + qt_color(tooltip) + ", "
		+ qt_color(tooltipText) + ", " + qt_color(placeholder);
	std::string scheme = std::string("# ") + kThemeMarker
		+ "\n[ColorScheme]\nactive_colors=" + active
		+ "\ninactive_colors=" + active + "\ndisabled_colors=" + disabled
		+ "\n";
	const char* versions[] = { "qt5ct", "qt6ct" };
	for (int i = 0; i < 2; i++) {
		std::string directory = config + "/" + versions[i];
		write_marked(directory + "/colors/Renga.conf", scheme.c_str());
		snprintf(text, sizeof(text), "# %s\n[Appearance]\n"
			"color_scheme_path=%s/colors/Renga.conf\ncustom_palette=true\n"
			"style=Fusion\n\n[Fonts]\n"
			"fixed=\"%s,%d,-1,5,50,0,0,0,0,0\"\n"
			"general=\"%s,%d,-1,5,50,0,0,0,0,0\"\n", kThemeMarker,
			directory.c_str(), family, points, family, points);
		write_marked(directory + "/" + versions[i] + ".conf", text);
	}
	return 0;
}


// #pragma mark - AppImages


/*!	Runs the AppImage \a path through "linux": marks it executable (it was
	opened on purpose) and sets APPIMAGE_EXTRACT_AND_RUN, so its runtime
	unpacks it instead of mounting it with FUSE.
*/
static int
run_appimage(const char* path)
{
	if (!renguin_setting("appimage", true)) {
		BAlert* alert = new BAlert("AppImage", "AppImages are switched off "
			"in the Renguin Manager.", "OK");
		alert->Go();
		return 1;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "linux-apps: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if ((st.st_mode & S_IXUSR) == 0)
		chmod(path, st.st_mode | S_IXUSR);
	pid_t child = fork();
	if (child == 0) {
		setsid();
		std::string appimage = std::string("APPIMAGE=") + path;
		execl("/bin/linux", "linux", "env", "APPIMAGE_EXTRACT_AND_RUN=1",
			appimage.c_str(), path, (char*)NULL);
		_exit(127);
	}
	return child > 0 ? 0 : 1;
}


int
main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "theme") == 0) {
		// ui_color() and be_plain_font come from app_server
		BApplication app("application/x-vnd.Renga-linux-apps");
		return write_theme();
	}
	if (argc >= 2 && argv[1][0] == '/') {
		// files from Tracker (B_ARGV_ONLY): AppImages
		BApplication app("application/x-vnd.Renga-linux-apps");
		int result = 0;
		for (int i = 1; i < argc; i++)
			result |= run_appimage(argv[i]);
		return result;
	}
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

	fprintf(stderr, "usage: linux-apps sync [-v] | list | theme | "
		"<AppImage>...\n");
	return 1;
}
