#pragma once

/*	Renga (batch 12, hardware item 6): which program each of the
	compositor's windows belongs to, for the Deskbar (one entry per Wayland
	program instead of none for the compositor's team). The message format
	is in headers/private/shared/wayland_clients.h; RengaClients.cpp has the
	details. All functions run with the Wayland looper locked.
*/

class BMessage;
class HaikuXdgToplevel;


//! A toplevel was created (not listed until it is shown).
void RengaClientsToplevelAdded(HaikuXdgToplevel *toplevel);
//! Its window was shown for the first time: the program gets its entry.
void RengaClientsToplevelShown(HaikuXdgToplevel *toplevel);
//! xdg_toplevel.set_app_id.
void RengaClientsSetAppId(HaikuXdgToplevel *toplevel, const char *appId);
//! The toplevel is gone.
void RengaClientsToplevelRemoved(HaikuXdgToplevel *toplevel);

//! Fills \a message (kMsgWaylandClients) with the current list.
void RengaClientsList(BMessage *message);
//! Sends the list to the Deskbar now.
void RengaClientsPush();
//! The list changed: sends it to the Deskbar once the current dispatch is
//! done (several changes in one dispatch make one message).
void RengaClientsChanged();
