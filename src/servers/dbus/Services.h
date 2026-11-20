/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DBUS_SERVICES_H
#define DBUS_SERVICES_H


#include "SessionBus.h"

#include <Locker.h>
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
*/
class SecretService : public Service {
public:
							SecretService(SessionBus* bus);
	virtual					~SecretService();
	virtual	const char* const* Names() const;
	virtual	void			HandleCall(const Message& call);

private:
	static	status_t		_Worker(void* data);
			void			_Handle(const Message& call);

			uint32			fNextSession;
			BLocker			fLock;
			std::vector<Message> fQueue;
			sem_id			fWakeUp;
			thread_id		fThread;
			volatile bool	fQuit;
			bool			fKeyringReady;
};


}	// namespace DBus


#endif	// DBUS_SERVICES_H
