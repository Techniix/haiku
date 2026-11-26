#pragma once

#include <Handler.h>
#include <Messenger.h>


class ServerHandler: public BHandler {
public:
	enum {
		closureSendMsg = 1,
		dispatchMsg = 'wldp',	// Renga: the socket event loop has work
		checkScreensMsg = 'wlsc',	// Renga: screens added, removed, changed
		pushClientsMsg = 'wlcl',	// Renga: send the client list to Deskbar
	};

	ServerHandler();
	virtual ~ServerHandler() = default;

	void MessageReceived(BMessage *msg) final;

private:
	void DispatchEvents();
};


extern ServerHandler gServerHandler;
extern BMessenger gServerMessenger;


// Renga: sends the events queued for every client (socket mode). Called
// with the server looper locked, after requests are dispatched and when a
// window thread leaves its WaylandEnv.
void WaylandFlushClients();
