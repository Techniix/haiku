/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RENGA_WAYLAND_CLIENTS_H
#define _RENGA_WAYLAND_CLIENTS_H


/*!	Renga (batch 12, hardware item 6): the Wayland compositor's clients as
	Deskbar entries.

	Every window of a Wayland program, native or Linux under Renguin, is a
	window of the compositor's team, so the Deskbar knew only that team, a
	background server it does not list: Linux programs had no entry. The
	compositor now tells the Deskbar which program each of its windows
	belongs to:

	  kMsgListWaylandClients   to the compositor
	                           (application/x-vnd.Renga-WaylandCompositor);
	                           the reply is a kMsgWaylandClients message.
	  kMsgWaylandClients       the list: the compositor's reply, and pushed
	                           to the Deskbar (application/x-vnd.Be-TSKB)
	                           whenever a program shows its first window,
	                           closes its last one or changes its app_id.

	A kMsgWaylandClients message has
	  "enabled"  bool     the Renguin Manager's switch "deskbar_entries"
	                      (~/config/settings/Renguin/settings, on by
	                      default); off: no "client" entries
	  "team"     int32    the compositor's team
	  "client"   message  one per program with a shown toplevel window:
	    "name"       string  the Deskbar label: the Name of the program's
	                         Deskbar entry (linux-apps), else its app_id,
	                         else its process name
	    "signature"  string  application/x-vnd.renguin-<id>, the signature
	                         linux-apps gives the program (its "Open with"
	                         launcher); <id> is the .desktop file's id, else
	                         the app_id or process name
	    "icon_path"  string  the program's Deskbar entry
	                         (~/config/settings/deskbar/menu/Applications/
	                         Linux/<Name>), whose icon the Deskbar shows;
	                         "" when it has none
	    "app_id"     string  xdg_toplevel.set_app_id, may be ""
	    "pid"        int32   the client's process (socket credentials)
	    "token"      int32[] client tokens (window_info.client_token) of its
	                         toplevel windows; popups are not listed

	Clients of the same program (one signature) are separate "client"
	entries; the Deskbar merges them as it merges teams of one application.
	All of this is private: neither the message nor its fields are public
	API (BDeskbar is unchanged).
*/


enum {
	kMsgListWaylandClients	= 'RgLC',
	kMsgWaylandClients		= 'RgLc'
};

#define RENGA_WAYLAND_COMPOSITOR_SIGNATURE \
	"application/x-vnd.Renga-WaylandCompositor"
#define RENGA_WAYLAND_CLIENT_SIGNATURE_PREFIX "application/x-vnd.renguin-"
#define RENGA_WAYLAND_CLIENTS_SWITCH "deskbar_entries"


#endif	// _RENGA_WAYLAND_CLIENTS_H
