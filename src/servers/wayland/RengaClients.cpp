/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renga (batch 12, hardware item 6): the compositor's clients as Deskbar
	entries.

	Every Wayland window is a BWindow of this team, and the team is a
	background server, so running Linux programs had no Deskbar entry at
	all. This file keeps the toplevels of every client and answers the
	Deskbar's kMsgListWaylandClients (headers/private/shared/
	wayland_clients.h) with one "client" entry per connected program that
	shows at least one toplevel: its name, the signature and icon of its
	Deskbar menu entry (written by linux-apps from the program's .desktop
	file) and the client tokens of its windows (the BWindow's handler
	token, which app_server reports as window_info.client_token). The
	Deskbar shows one entry per program and lists in its window menu only
	the windows with those tokens. Changes (a first window shown, the last
	one gone, a new app_id) are pushed to the Deskbar once per dispatch.

	Name, signature and icon: the program's app_id (xdg_toplevel.
	set_app_id, by convention the .desktop file's id) is looked up among
	the Deskbar entries of ~/config/settings/deskbar/menu/Applications/
	Linux; each carries "renga:linux", the path of its .desktop file. A
	match gives the entry's name (the .desktop Name) and its file, whose
	icon the Deskbar uses. Without a match the app_id, else the client
	process's name, is the label. The signature is the one linux-apps gives
	the program (application/x-vnd.renguin-<id>), so the Deskbar's "launch
	another" (middle click) starts its "Open with" launcher when there is
	one.

	The Renguin Manager's switch "deskbar_entries" (on by default) turns
	this off: the list is then empty and the Deskbar shows no Linux
	programs, as before batch 12.
*/


#include "RengaClients.h"

#include "HaikuXdgToplevel.h"
#include "WaylandServer.h"

#include <wayland-server-core.h>

#include <AppMisc.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Messenger.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <String.h>
#include <Window.h>
#include <driver_settings.h>
#include <wayland_clients.h>

#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>


static const char* kDeskbarSignature = "application/x-vnd.Be-TSKB";
static const char* kLinuxAttribute = "renga:linux";


struct ToplevelEntry {
	HaikuXdgToplevel*	toplevel;
	std::string			appId;
	bool				shown;
};

static std::vector<ToplevelEntry> sToplevels;
static bool sPushPending = false;


static ToplevelEntry*
find_entry(HaikuXdgToplevel* toplevel)
{
	for (ToplevelEntry& entry : sToplevels) {
		if (entry.toplevel == toplevel)
			return &entry;
	}
	return NULL;
}


//!	The Renguin switch (driver settings, as linux-apps reads its own).
static bool
clients_enabled()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return true;
	path.Append("Renguin/settings");
	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return true;
	bool value = get_driver_boolean_parameter(handle,
		RENGA_WAYLAND_CLIENTS_SWITCH, true, true);
	unload_driver_settings(handle);
	return value;
}


//!	As linux-apps' program_signature(): the signature of program \a id.
static std::string
program_signature(const std::string& id)
{
	std::string signature = RENGA_WAYLAND_CLIENT_SIGNATURE_PREFIX;
	for (size_t i = 0; i < id.size() && signature.size() < B_MIME_TYPE_LENGTH
			- 1; i++) {
		char c = id[i];
		signature += (isalnum((unsigned char)c) || c == '.' || c == '-'
			|| c == '_') ? (char)tolower((unsigned char)c) : '-';
	}
	return signature;
}


struct MenuEntry {
	std::string	id;		// the .desktop file's id
	std::string	name;	// the entry's file name: the program's Name
	std::string	path;
};


//!	The Deskbar entries linux-apps wrote (read once per list).
static std::vector<MenuEntry>
read_menu_entries()
{
	std::vector<MenuEntry> entries;
	BPath menu;
	if (find_directory(B_USER_DESKBAR_DIRECTORY, &menu) != B_OK)
		return entries;
	menu.Append("Applications/Linux");
	BDirectory directory(menu.Path());
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		BNode node(&entry);
		BString desktop;
		if (node.ReadAttrString(kLinuxAttribute, &desktop) != B_OK)
			continue;
		int32 slash = desktop.FindLast('/');
		BString id(desktop.String() + slash + 1);
		if (id.EndsWith(".desktop"))
			id.Truncate(id.Length() - 8);
		BPath path;
		entry.GetPath(&path);
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		entries.push_back({ id.String(), name, path.Path() });
	}
	return entries;
}


/*!	The menu entry of the program with \a appId: the .desktop id equals it
	(case aside), else the id's last dotted part does (the entry
	"org.gnome.Calculator" for the app_id "calculator"; GTK programs that
	set no application id report their program name).
*/
static const MenuEntry*
match_menu_entry(const std::vector<MenuEntry>& entries, const std::string& appId)
{
	if (appId.empty())
		return NULL;
	for (const MenuEntry& entry : entries) {
		if (strcasecmp(entry.id.c_str(), appId.c_str()) == 0)
			return &entry;
	}
	for (const MenuEntry& entry : entries) {
		size_t dot = entry.id.rfind('.');
		if (dot != std::string::npos
			&& strcasecmp(entry.id.c_str() + dot + 1, appId.c_str()) == 0)
			return &entry;
	}
	return NULL;
}


//!	The client process's name (its first argument, without the path).
static std::string
process_name(pid_t pid)
{
	team_info info;
	if (pid <= 0 || get_team_info(pid, &info) != B_OK)
		return "";
	std::string args = info.args;
	size_t end = args.find(' ');
	if (end != std::string::npos)
		args.erase(end);
	size_t slash = args.rfind('/');
	if (slash != std::string::npos)
		args.erase(0, slash + 1);
	return args;
}


void
RengaClientsToplevelAdded(HaikuXdgToplevel* toplevel)
{
	if (find_entry(toplevel) == NULL)
		sToplevels.push_back({ toplevel, "", false });
}


void
RengaClientsToplevelShown(HaikuXdgToplevel* toplevel)
{
	ToplevelEntry* entry = find_entry(toplevel);
	if (entry == NULL || entry->shown)
		return;
	entry->shown = true;
	RengaClientsChanged();
}


void
RengaClientsSetAppId(HaikuXdgToplevel* toplevel, const char* appId)
{
	ToplevelEntry* entry = find_entry(toplevel);
	if (entry == NULL || appId == NULL || entry->appId == appId)
		return;
	entry->appId = appId;
	if (entry->shown)
		RengaClientsChanged();
}


void
RengaClientsToplevelRemoved(HaikuXdgToplevel* toplevel)
{
	for (size_t i = 0; i < sToplevels.size(); i++) {
		if (sToplevels[i].toplevel != toplevel)
			continue;
		bool shown = sToplevels[i].shown;
		sToplevels.erase(sToplevels.begin() + i);
		if (shown)
			RengaClientsChanged();
		return;
	}
}


void
RengaClientsList(BMessage* message)
{
	message->what = kMsgWaylandClients;
	message->AddInt32("team", BPrivate::current_team());
	bool enabled = clients_enabled();
	message->AddBool("enabled", enabled);
	if (!enabled)
		return;

	std::vector<MenuEntry> menuEntries = read_menu_entries();

	// one entry per client, in the order their first toplevel appeared
	std::vector<struct wl_client*> clients;
	for (const ToplevelEntry& entry : sToplevels) {
		if (!entry.shown || entry.toplevel->Window() == NULL)
			continue;
		struct wl_client* client = entry.toplevel->Client();
		bool known = false;
		for (struct wl_client* other : clients)
			known |= other == client;
		if (!known)
			clients.push_back(client);
	}

	for (struct wl_client* client : clients) {
		BMessage info;
		std::string appId;
		for (const ToplevelEntry& entry : sToplevels) {
			if (!entry.shown || entry.toplevel->Window() == NULL
				|| entry.toplevel->Client() != client)
				continue;
			if (appId.empty())
				appId = entry.appId;
			info.AddInt32("token",
				_get_object_token_(entry.toplevel->Window()));
		}
		pid_t pid = 0;
		wl_client_get_credentials(client, &pid, NULL, NULL);

		std::string name;
		std::string id;
		std::string iconPath;
		const MenuEntry* menuEntry = match_menu_entry(menuEntries, appId);
		if (menuEntry != NULL) {
			name = menuEntry->name;
			id = menuEntry->id;
			iconPath = menuEntry->path;
		} else {
			name = !appId.empty() ? appId : process_name(pid);
			id = name;
		}
		if (name.empty())
			name = id = "wayland-program";

		info.AddString("name", name.c_str());
		info.AddString("signature", program_signature(id).c_str());
		info.AddString("icon_path", iconPath.c_str());
		info.AddString("app_id", appId.c_str());
		info.AddInt32("pid", pid);
		message->AddMessage("client", &info);
	}
}


void
RengaClientsPush()
{
	sPushPending = false;
	BMessenger deskbar(kDeskbarSignature);
	if (!deskbar.IsValid())
		return;
	BMessage message;
	RengaClientsList(&message);
	// never wait for the Deskbar: this runs in the Wayland looper
	deskbar.SendMessage(&message, (BHandler*)NULL, 0);
}


void
RengaClientsChanged()
{
	if (sPushPending)
		return;
	sPushPending = true;
	BMessage message(ServerHandler::pushClientsMsg);
	if (gServerMessenger.SendMessage(&message) != B_OK)
		sPushPending = false;
}
