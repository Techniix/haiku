/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DBUS_SERVICES_H
#define DBUS_SERVICES_H


#include "SessionBus.h"

#include <Locker.h>
#include <StringList.h>
#include <OS.h>

#include <map>
#include <string>


static const char* const kSessionBusSignature
	= "application/x-vnd.Renga-SessionBus";


namespace DBus {


class NotificationsService : public Service {
public:
							NotificationsService(SessionBus* bus);
	virtual	const char* const* Names() const;
	virtual	void			HandleCall(const Message& call);

			void			Clicked(uint32 id);
				// from the notification's click (bus application)

private:
			uint32			fNextID;
			std::map<uint32, std::string> fOwners;
			std::map<uint32, bool> fDefaultAction;
};


class UPowerService : public Service {
public:
							UPowerService(SessionBus* bus);
	virtual	const char* const* Names() const;
	virtual	void			HandleCall(const Message& call);
};


class PortalService : public Service {
public:
							PortalService(SessionBus* bus);
	virtual	const char* const* Names() const;
	virtual	void			HandleCall(const Message& call);

private:
			uint32			fNextRequest;
};


/*!	The Secret Service's calls go to the key store (keystore_server), which
	may take its time (or ask the user); they run on a thread of their own,
	so the bus never waits for it.

	Per-application approval (2026-11-20): the caller is identified by the
	team behind its connection (SO_PEERCRED) and that team's program file.
	The first time a program uses the collection, the user is asked: Deny,
	Allow this time (until the program quits) or Always allow (stored in the
	key store itself, in the same keyring as the secrets, so only what may
	read the secrets can change it). Without an answer within two minutes
	the request is denied. Callers that cannot be identified are denied.
*/
class SecretService : public Service {
public:
							SecretService(SessionBus* bus);
	virtual					~SecretService();
	virtual	const char* const* Names() const;
	virtual	void			HandleCall(const Message& call);

			// "Always allow" list, for the management window (any thread)
			status_t		GetAllowedPrograms(BStringList& programs);
			status_t		RemoveAllowedProgram(const BString& program);

private:
	struct QueuedCall {
		Message			call;
		team_id			team;
	};

	static	status_t		_Worker(void* data);
			void			_Handle(const Message& call);
			bool			_Approved(const Message& call, team_id team);
			int32			_Ask(const BString& program);
			status_t		_LoadAllowed();
			status_t		_StoreAllowed();

			uint32			fNextSession;
			BLocker			fLock;
			std::vector<QueuedCall> fQueue;
			sem_id			fWakeUp;
			thread_id		fThread;
			volatile bool	fQuit;
			bool			fKeyringReady;

			// under fLock
			BStringList		fAllowed;
			bool			fAllowedLoaded;
			std::map<std::string, bool> fDecisions;
				// "team:path" -> allowed, for this run of a program
};


}	// namespace DBus


#endif	// DBUS_SERVICES_H
