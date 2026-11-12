/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SERVER_FD_TRANSFER_H
#define _SERVER_FD_TRANSFER_H


/*!	Passing a file descriptor from an application to app_server (Renga,
	F-58: the prerequisite of the Quasar driver's request R-1; the GPU
	buffers of F-47 travel this way, see <add-ons/graphics/GpuPresent.h>).

	The app_server link (ports) cannot carry descriptors. Next to it
	app_server listens on a local SOCK_SEQPACKET socket
	(APP_SERVER_FD_SOCKET); app_server_send_fd() sends a descriptor there
	with SCM_RIGHTS and gets back a token. The application then names the
	descriptor by that token in its app_server messages, and app_server
	takes it (ServerApp::TakeClientFd()). app_server learns the sending
	team from the socket's SO_PEERCRED, so a token can only be taken by
	the ServerApp of the team that sent the descriptor, and only once.
	Descriptors not taken are closed when their team's ServerApp goes, and
	after kFdTransferLifetime.

	The caller keeps its own descriptor (close it if it is no longer
	needed); app_server gets a new one for the same open file.
*/


#include <OS.h>


#define APP_SERVER_FD_SOCKET_DIRECTORY	"/boot/system/var/run/app_server"
#define APP_SERVER_FD_SOCKET			APP_SERVER_FD_SOCKET_DIRECTORY "/fd"

namespace BPrivate {


// The request: this header plus exactly one descriptor (SCM_RIGHTS).
struct fd_transfer_request {
	uint32		magic;			// FD_TRANSFER_MAGIC
	uint32		version;		// FD_TRANSFER_VERSION
};

// The reply.
struct fd_transfer_reply {
	int32		status;			// B_OK, or why the descriptor was refused
	int32		token;			// > 0 on success
};

#define FD_TRANSFER_MAGIC		'RFdT'
#define FD_TRANSFER_VERSION		1

// How long app_server keeps a descriptor nobody took (microseconds).
static const bigtime_t kFdTransferLifetime = 60000000;


/*!	Hands \a fd to app_server; *_token names it in app_server messages.
	Thread safe. B_BAD_VALUE for a negative descriptor, B_NO_INIT if
	app_server does not offer the socket, an error of the socket calls
	otherwise.
*/
status_t app_server_send_fd(int fd, int32* _token);


}	// namespace BPrivate


#endif	// _SERVER_FD_TRANSFER_H
