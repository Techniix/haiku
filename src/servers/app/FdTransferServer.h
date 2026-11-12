/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef FD_TRANSFER_SERVER_H
#define FD_TRANSFER_SERVER_H


#include <Locker.h>
#include <OS.h>

#include <map>


/*!	The receiving end of BPrivate::app_server_send_fd()
	(headers/private/app/ServerFdTransfer.h; Renga F-58, prerequisite of
	the Quasar driver's R-1): a thread serving the local SOCK_SEQPACKET
	socket APP_SERVER_FD_SOCKET. Every request carries one descriptor; it
	is kept under a new token, with the sending team from SO_PEERCRED, until
	that team's ServerApp takes it, the team's ServerApp goes, or
	kFdTransferLifetime passes.
*/
class FdTransferServer {
public:
								FdTransferServer();
								~FdTransferServer();

			status_t			Start();

	// The descriptor \a team sent under \a token, now owned by the caller;
	// -1 if there is none (wrong team, taken already, expired).
			int					Take(team_id team, int32 token);
	// Closes what \a team sent and nobody took.
			void				TeamQuit(team_id team);

	static	FdTransferServer*	Default();

private:
	struct Entry {
		team_id		team;
		int			fd;
		bigtime_t	received;
	};

	static	status_t			_ThreadEntry(void* data);
			void				_Run();
			bool				_Receive(int client);
			void				_Expire();

			BLocker				fLock;
			std::map<int32, Entry> fEntries;
			int32				fNextToken;
			int					fListener;
			thread_id			fThread;
};


#endif	// FD_TRANSFER_SERVER_H
