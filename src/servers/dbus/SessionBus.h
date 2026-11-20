/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SESSION_BUS_H
#define SESSION_BUS_H


#include "DBusMessage.h"

#include <Locker.h>

#include <map>
#include <string>
#include <vector>


namespace DBus {


class Service;


struct MatchRule {
	BString		text;
	BString		type;
	BString		sender;
	BString		interface;
	BString		member;
	BString		path;
	BString		pathNamespace;
	BString		destination;
	BString		arg0;
	bool		eavesdrop;

	bool		Parse(const char* rule);
	bool		Matches(const Message& message) const;
};


struct Connection {
	int						fd;
	BString					uniqueName;
	bool					authenticated;
	bool					helloDone;
	bool					unixFDs;
	std::vector<uint8>		input;
	std::vector<int>		inputFDs;
	std::vector<uint8>		output;
	std::vector<int>		outputFDs;
		// descriptors still to send with the first byte of output
	std::vector<MatchRule>	rules;
	uid_t					uid;
	pid_t					pid;
};


/*!	The session bus: a D-Bus message bus on a Unix socket (by default
	$XDG_RUNTIME_DIR/bus), with the bus's own org.freedesktop.DBus
	interface (names, matching, NameOwnerChanged), routing of calls,
	replies and signals, Unix descriptor passing, and services that live
	inside the bus process (Service, Services.cpp) under well-known names.
*/
class SessionBus {
public:
							SessionBus();
							~SessionBus();

			status_t		Listen(const char* socketPath);
			void			Run();
				// until Quit(); the bus thread
			void			Quit();

			void			AddService(Service* service);

			// for services (any thread)
			void			Send(const Message& message);
				// from the bus itself (sender org.freedesktop.DBus or a
				// service's name), serial assigned
			uint32			NextSerial();

private:
			void			_Accept();
			bool			_Read(Connection* connection);
			bool			_Flush(Connection* connection);
			bool			_Authenticate(Connection* connection);
			void			_Dispatch(Connection* from, Message& message);
			void			_Route(Message& message, Connection* from);
			void			_Deliver(Connection* to, const Message& message);
			void			_BusMethod(Connection* from,
								const Message& message);
			void			_SetOwner(const BString& name,
								const BString& owner);
			void			_Drop(Connection* connection);
			Connection*		_Find(const BString& name);
			Service*		_FindService(const BString& name);
			void			_DrainQueue();

			int				fListenFD;
			int				fWakeUp[2];
			volatile bool	fQuit;
			BString			fGUID;
			uint32			fNextUnique;
			uint32			fSerial;
			std::vector<Connection*>			fConnections;
			std::map<std::string, std::string>	fOwners;
				// well-known name -> unique name
			std::vector<Service*>				fServices;
			BLocker			fQueueLock;
			std::vector<Message>				fQueue;
				// from services, delivered by the bus thread
};


/*!	A service inside the bus process: owns well-known names and answers
	the calls sent to them. Called on the bus thread; replies and signals
	go out with SessionBus::Send() (any thread).
*/
class Service {
public:
							Service(SessionBus* bus) : fBus(bus) {}
	virtual					~Service() {}

	virtual	const char* const* Names() const = 0;
	virtual	void			HandleCall(const Message& call) = 0;

protected:
			void			Reply(const Message& call,
								const DValueList& values);
			void			Error(const Message& call, const char* name,
								const char* text);
			void			EmitSignal(const char* sender, const char* path,
								const char* interface, const char* member,
								const DValueList& values);

			SessionBus*		fBus;
};


}	// namespace DBus


#endif	// SESSION_BUS_H
