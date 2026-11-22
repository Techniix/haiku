/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

//!	Renga's D-Bus session bus; see SessionBus.h.


#include "SessionBus.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include <Autolock.h>
#include <OS.h>


namespace DBus {


/*!	The user ID Linux programs see under Renguin as its normal user
	(Renguin's process.cpp, kLinuxUserID; switch "linux_user"). Their
	sockets carry Renga's own user (SO_PEERCRED), so EXTERNAL
	authentication with "1000" from a peer of the bus's own user is
	accepted (batch 12: Linux Chromium's calls timed out, its libdbus was
	rejected). Renga has one user, so this lets no one in who was not.
*/
static const uid_t kRenguinUserID = 1000;

static const char* kBusName = "org.freedesktop.DBus";
static const size_t kMaxConnections = 256;
static const size_t kMaxPendingOutput = 32 * 1024 * 1024;


// #pragma mark - match rules


static bool
parse_rule_value(const char*& text, BString& key, BString& value)
{
	while (*text == ' ' || *text == ',')
		text++;
	if (*text == '\0')
		return false;
	const char* equals = strchr(text, '=');
	if (equals == NULL)
		return false;
	key.SetTo(text, equals - text);
	key.Trim();
	text = equals + 1;
	value = "";
	if (*text == '\'') {
		text++;
		while (*text != '\0' && *text != '\'')
			value << *text++;
		if (*text != '\'')
			return false;
		text++;
	} else {
		while (*text != '\0' && *text != ',')
			value << *text++;
	}
	return true;
}


bool
MatchRule::Parse(const char* rule)
{
	text = rule;
	eavesdrop = false;
	const char* at = rule;
	BString key, value;
	while (parse_rule_value(at, key, value)) {
		if (key == "type")
			type = value;
		else if (key == "sender")
			sender = value;
		else if (key == "interface")
			interface = value;
		else if (key == "member")
			member = value;
		else if (key == "path")
			path = value;
		else if (key == "path_namespace")
			pathNamespace = value;
		else if (key == "destination")
			destination = value;
		else if (key == "arg0")
			arg0 = value;
		else if (key == "eavesdrop")
			eavesdrop = value == "true";
		// other keys (argN, argNpath, arg0namespace) are accepted and
		// not checked: such a rule matches more, never less
	}
	while (*at == ' ' || *at == ',')
		at++;
	return *at == '\0';
}


bool
MatchRule::Matches(const Message& message) const
{
	static const char* kTypes[] = { "", "method_call", "method_return",
		"error", "signal" };
	if (!type.IsEmpty() && (message.type > SIGNAL
			|| type != kTypes[message.type]))
		return false;
	if (!sender.IsEmpty() && sender != message.sender)
		return false;
	if (!interface.IsEmpty() && interface != message.interface)
		return false;
	if (!member.IsEmpty() && member != message.member)
		return false;
	if (!path.IsEmpty() && path != message.path)
		return false;
	if (!pathNamespace.IsEmpty()) {
		if (pathNamespace != "/" && message.path != pathNamespace
			&& !(message.path.StartsWith(pathNamespace)
				&& message.path[pathNamespace.Length()] == '/'))
			return false;
	}
	if (!destination.IsEmpty() && destination != message.destination)
		return false;
	if (!arg0.IsEmpty()) {
		DValueList values;
		if (message.GetArguments(values) != B_OK || values.empty()
			|| values[0].s != arg0)
			return false;
	}
	return true;
}


// #pragma mark - Service


void
Service::Reply(const Message& call, const DValueList& values)
{
	if ((call.flags & NO_REPLY_EXPECTED) != 0)
		return;
	Message reply = call.Reply();
	reply.sender = call.destination;
	reply.SetArguments(values);
	fBus->Send(reply);
}


void
Service::Error(const Message& call, const char* name, const char* text)
{
	if ((call.flags & NO_REPLY_EXPECTED) != 0)
		return;
	Message reply = call.ErrorReply(name, text);
	reply.sender = call.destination;
	fBus->Send(reply);
}


void
Service::EmitSignal(const char* sender, const char* path,
	const char* interface, const char* member, const DValueList& values)
{
	Message signal = Message::Signal(path, interface, member);
	signal.sender = sender;
	signal.SetArguments(values);
	fBus->Send(signal);
}


// #pragma mark - SessionBus


SessionBus::SessionBus()
	:
	fListenFD(-1),
	fQuit(false),
	fNextUnique(1),
	fSerial(1),
	fQueueLock("dbus queue")
{
	fWakeUp[0] = fWakeUp[1] = -1;
	uint8 random[16];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || read(fd, random, sizeof(random)) != sizeof(random)) {
		bigtime_t now = system_time();
		memcpy(random, &now, sizeof(now));
		memcpy(random + 8, &now, sizeof(now));
	}
	if (fd >= 0)
		close(fd);
	for (size_t i = 0; i < sizeof(random); i++)
		fGUID << BString().SetToFormat("%02x", random[i]);
}


SessionBus::~SessionBus()
{
	for (size_t i = 0; i < fConnections.size(); i++) {
		close(fConnections[i]->fd);
		delete fConnections[i];
	}
	if (fListenFD >= 0)
		close(fListenFD);
	if (fWakeUp[0] >= 0) {
		close(fWakeUp[0]);
		close(fWakeUp[1]);
	}
	for (size_t i = 0; i < fServices.size(); i++)
		delete fServices[i];
}


status_t
SessionBus::Listen(const char* socketPath)
{
	if (pipe(fWakeUp) != 0)
		return errno;
	fcntl(fWakeUp[0], F_SETFL, O_NONBLOCK);
	fcntl(fWakeUp[1], F_SETFL, O_NONBLOCK);

	fListenFD = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fListenFD < 0)
		return errno;
	fcntl(fListenFD, F_SETFD, FD_CLOEXEC);

	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlcpy(address.sun_path, socketPath, sizeof(address.sun_path))
			>= sizeof(address.sun_path))
		return B_NAME_TOO_LONG;
	unlink(socketPath);
	if (bind(fListenFD, (struct sockaddr*)&address, sizeof(address)) != 0)
		return errno;
	chmod(socketPath, 0700);
	if (listen(fListenFD, 32) != 0)
		return errno;
	fcntl(fListenFD, F_SETFL, O_NONBLOCK);
	return B_OK;
}


void
SessionBus::AddService(Service* service)
{
	fServices.push_back(service);
	for (const char* const* name = service->Names(); *name != NULL; name++)
		fOwners[*name] = kBusName;
			// owned by the bus process itself
}


uint32
SessionBus::NextSerial()
{
	return (uint32)atomic_add((int32*)&fSerial, 1);
}


void
SessionBus::Send(const Message& message)
{
	{
		BAutolock locker(fQueueLock);
		fQueue.push_back(message);
	}
	char byte = 0;
	write(fWakeUp[1], &byte, 1);
}


void
SessionBus::Quit()
{
	fQuit = true;
	char byte = 0;
	write(fWakeUp[1], &byte, 1);
}


void
SessionBus::Run()
{
	while (!fQuit) {
		std::vector<struct pollfd> fds;
		struct pollfd entry;
		entry.fd = fListenFD;
		entry.events = POLLIN;
		entry.revents = 0;
		fds.push_back(entry);
		entry.fd = fWakeUp[0];
		fds.push_back(entry);
		for (size_t i = 0; i < fConnections.size(); i++) {
			entry.fd = fConnections[i]->fd;
			entry.events = POLLIN
				| (fConnections[i]->output.empty() ? 0 : POLLOUT);
			fds.push_back(entry);
		}
		if (poll(fds.data(), fds.size(), -1) < 0 && errno != EINTR)
			break;

		if ((fds[1].revents & POLLIN) != 0) {
			char buffer[64];
			while (read(fWakeUp[0], buffer, sizeof(buffer)) > 0)
				;
		}
		_DrainQueue();

		// the connections polled; new ones come after this loop
		std::vector<Connection*> polled(fConnections.begin(),
			fConnections.begin() + (fds.size() - 2));
		for (size_t i = 0; i < polled.size(); i++) {
			Connection* connection = polled[i];
			short revents = fds[i + 2].revents;
			bool alive = true;
			if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0)
				alive = _Read(connection);
			if (alive && (revents & POLLOUT) != 0)
				alive = _Flush(connection);
			if (!alive)
				_Drop(connection);
		}
		_DrainQueue();

		if ((fds[0].revents & POLLIN) != 0)
			_Accept();
	}
}


void
SessionBus::_DrainQueue()
{
	std::vector<Message> queue;
	{
		BAutolock locker(fQueueLock);
		queue.swap(fQueue);
	}
	for (size_t i = 0; i < queue.size(); i++) {
		if (queue[i].serial == 0)
			queue[i].serial = NextSerial();
		_Route(queue[i], NULL);
	}
}


void
SessionBus::_Accept()
{
	while (true) {
		int fd = accept(fListenFD, NULL, NULL);
		if (fd < 0)
			return;
		if (fConnections.size() >= kMaxConnections) {
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		Connection* connection = new Connection;
		connection->fd = fd;
		connection->authenticated = false;
		connection->helloDone = false;
		connection->unixFDs = false;
		connection->uid = (uid_t)-1;
		connection->pid = -1;
		struct ucred credentials;
		socklen_t length = sizeof(credentials);
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length)
				== 0) {
			connection->uid = credentials.uid;
			connection->pid = credentials.pid;
		}
		fConnections.push_back(connection);
	}
}


bool
SessionBus::_Read(Connection* connection)
{
	while (true) {
		uint8 buffer[16384];
		struct iovec vector = { buffer, sizeof(buffer) };
		char control[CMSG_SPACE(sizeof(int) * 16)];
		struct msghdr header;
		memset(&header, 0, sizeof(header));
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		header.msg_control = control;
		header.msg_controllen = sizeof(control);
		ssize_t bytes = recvmsg(connection->fd, &header, 0);
		if (bytes < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
		if (bytes == 0)
			return false;
		for (struct cmsghdr* message = CMSG_FIRSTHDR(&header);
				message != NULL; message = CMSG_NXTHDR(&header, message)) {
			if (message->cmsg_level != SOL_SOCKET
				|| message->cmsg_type != SCM_RIGHTS)
				continue;
			int count = (message->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int* received = (int*)CMSG_DATA(message);
			for (int i = 0; i < count; i++) {
				fcntl(received[i], F_SETFD, FD_CLOEXEC);
				connection->inputFDs.push_back(received[i]);
			}
		}
		connection->input.insert(connection->input.end(), buffer,
			buffer + bytes);
		if (connection->input.size() > kMaxMessageSize + 4096)
			return false;

		if (!connection->authenticated) {
			if (!_Authenticate(connection))
				return false;
			if (!connection->authenticated)
				continue;
		}

		while (connection->authenticated && !connection->input.empty()) {
			ssize_t needed = Message::NeededSize(connection->input.data(),
				connection->input.size());
			if (needed < 0)
				return false;
			if (needed == 0 || (size_t)needed > connection->input.size())
				break;
			Message message;
			if (message.Parse(connection->input.data(), needed) != B_OK)
				return false;
			connection->input.erase(connection->input.begin(),
				connection->input.begin() + needed);
			if (message.unixFDCount > connection->inputFDs.size())
				return false;
			message.fds.assign(connection->inputFDs.begin(),
				connection->inputFDs.begin() + message.unixFDCount);
			connection->inputFDs.erase(connection->inputFDs.begin(),
				connection->inputFDs.begin() + message.unixFDCount);
			_Dispatch(connection, message);
			for (size_t i = 0; i < message.fds.size(); i++)
				close(message.fds[i]);
		}
	}
}


static void
queue_text(Connection* connection, const char* text)
{
	connection->output.insert(connection->output.end(), text,
		text + strlen(text));
}


/*!	The SASL exchange before the first message: EXTERNAL with the client's
	own user ID (checked against the socket's peer credentials) or, as
	Linux's dbus-daemon allows on the session bus, ANONYMOUS is refused;
	NEGOTIATE_UNIX_FD; BEGIN.
*/
bool
SessionBus::_Authenticate(Connection* connection)
{
	std::vector<uint8>& input = connection->input;
	size_t start = 0;
	if (!connection->helloDone) {
		// the credentials byte
		if (input.empty())
			return true;
		if (input[0] != 0)
			return false;
		start = 1;
		connection->helloDone = true;
	}
	while (true) {
		size_t end = start;
		while (end + 1 < input.size()
			&& !(input[end] == '\r' && input[end + 1] == '\n'))
			end++;
		if (end + 1 >= input.size()) {
			input.erase(input.begin(), input.begin() + start);
			return input.size() < 16384;
		}
		BString line((const char*)input.data() + start, end - start);
		start = end + 2;

		if (line.StartsWith("AUTH EXTERNAL")) {
			BString hex = line.String() + strlen("AUTH EXTERNAL");
			hex.Trim();
			BString decoded;
			for (int32 i = 0; i + 1 < hex.Length(); i += 2) {
				char digits[3] = { hex[i], hex[i + 1], '\0' };
				decoded << (char)strtol(digits, NULL, 16);
			}
			uid_t claimed = decoded.IsEmpty() ? connection->uid
				: (uid_t)strtoul(decoded.String(), NULL, 10);
			if (connection->uid != (uid_t)-1 && claimed == kRenguinUserID
				&& connection->uid == getuid()) {
				// a Linux program as Renguin's normal user (batch 11): it
				// sees uid 1000, the socket says Renga's own user
				connection->uid = claimed;
			}
			if (connection->uid != (uid_t)-1 && claimed == connection->uid) {
				BString ok;
				ok << "OK " << fGUID << "\r\n";
				queue_text(connection, ok.String());
			} else if (hex.IsEmpty() && connection->uid != (uid_t)-1) {
				// "AUTH EXTERNAL" without data: ask for it (empty data)
				queue_text(connection, "DATA\r\n");
			} else
				queue_text(connection, "REJECTED EXTERNAL\r\n");
		} else if (line == "DATA" || line.StartsWith("DATA ")) {
			BString ok;
			ok << "OK " << fGUID << "\r\n";
			queue_text(connection, ok.String());
		} else if (line.StartsWith("AUTH")) {
			queue_text(connection, "REJECTED EXTERNAL\r\n");
		} else if (line == "NEGOTIATE_UNIX_FD") {
			connection->unixFDs = true;
			queue_text(connection, "AGREE_UNIX_FD\r\n");
		} else if (line == "BEGIN") {
			connection->authenticated = true;
			input.erase(input.begin(), input.begin() + start);
			_Flush(connection);
			return true;
		} else if (line == "CANCEL" || line.StartsWith("ERROR")) {
			queue_text(connection, "REJECTED EXTERNAL\r\n");
		} else
			queue_text(connection, "ERROR \"unknown command\"\r\n");
		_Flush(connection);
	}
}


bool
SessionBus::_Flush(Connection* connection)
{
	while (!connection->output.empty()) {
		struct iovec vector = { connection->output.data(),
			connection->output.size() };
		struct msghdr header;
		memset(&header, 0, sizeof(header));
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		std::vector<char> control;
		if (!connection->outputFDs.empty()) {
			control.resize(CMSG_SPACE(sizeof(int)
				* connection->outputFDs.size()));
			header.msg_control = control.data();
			header.msg_controllen = control.size();
			struct cmsghdr* message = CMSG_FIRSTHDR(&header);
			message->cmsg_level = SOL_SOCKET;
			message->cmsg_type = SCM_RIGHTS;
			message->cmsg_len = CMSG_LEN(sizeof(int)
				* connection->outputFDs.size());
			memcpy(CMSG_DATA(message), connection->outputFDs.data(),
				sizeof(int) * connection->outputFDs.size());
		}
		ssize_t bytes = sendmsg(connection->fd, &header, MSG_NOSIGNAL);
		if (bytes < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
		for (size_t i = 0; i < connection->outputFDs.size(); i++)
			close(connection->outputFDs[i]);
		connection->outputFDs.clear();
		connection->output.erase(connection->output.begin(),
			connection->output.begin() + bytes);
	}
	return true;
}


void
SessionBus::_Deliver(Connection* to, const Message& message)
{
	if (!to->authenticated)
		return;
	if (!message.fds.empty() && !to->unixFDs)
		return;
	if (to->output.size() > kMaxPendingOutput)
		return;
	Message copy = message;
	if (copy.bigEndianBody) {
		DValueList values;
		if (copy.GetArguments(values) != B_OK)
			return;
		BString signature = copy.signature;
		copy.SetArguments(values);
	}
	std::vector<uint8> bytes;
	copy.fds.clear();
	copy.unixFDCount = message.fds.size();
	copy.Serialize(bytes);
	if (!message.fds.empty()) {
		// descriptors go with the first byte of this message: send what is
		// queued before it first
		if (!to->outputFDs.empty() || !to->output.empty())
			_Flush(to);
		if (!to->outputFDs.empty() || !to->output.empty())
			return;
			// (rare: a full socket; the message is dropped like on a
			// too-slow reader)
		for (size_t i = 0; i < message.fds.size(); i++)
			to->outputFDs.push_back(dup(message.fds[i]));
	}
	to->output.insert(to->output.end(), bytes.begin(), bytes.end());
	_Flush(to);
}


team_id
SessionBus::PeerTeam(const BString& name)
{
	Connection* connection = _Find(name);
	return connection != NULL ? (team_id)connection->pid : -1;
}


Connection*
SessionBus::_Find(const BString& name)
{
	BString unique = name;
	if (!name.StartsWith(":")) {
		std::map<std::string, std::string>::iterator owner
			= fOwners.find(name.String());
		if (owner == fOwners.end())
			return NULL;
		unique = owner->second.c_str();
	}
	for (size_t i = 0; i < fConnections.size(); i++) {
		if (fConnections[i]->uniqueName == unique)
			return fConnections[i];
	}
	return NULL;
}


Service*
SessionBus::_FindService(const BString& name)
{
	for (size_t i = 0; i < fServices.size(); i++) {
		for (const char* const* own = fServices[i]->Names(); *own != NULL;
				own++) {
			if (name == *own) {
				// unless a client took the name over
				std::map<std::string, std::string>::iterator owner
					= fOwners.find(name.String());
				if (owner != fOwners.end() && owner->second != kBusName)
					return NULL;
				return fServices[i];
			}
		}
	}
	return NULL;
}


void
SessionBus::_Dispatch(Connection* from, Message& message)
{
	message.sender = from->uniqueName;
	if (from->uniqueName.IsEmpty()) {
		// the first call must be Hello()
		if (message.type != METHOD_CALL || message.member != "Hello"
			|| message.destination != kBusName) {
			_Drop(from);
			return;
		}
	}
	_Route(message, from);
}


void
SessionBus::_Route(Message& message, Connection* from)
{
	if (from == NULL && message.sender.IsEmpty())
		message.sender = kBusName;

	if (message.type == METHOD_CALL && message.destination == kBusName) {
		if (from != NULL)
			_BusMethod(from, message);
		return;
	}

	// signals to everyone whose rules match (and the destination)
	if (message.type == SIGNAL || message.destination.IsEmpty()) {
		Connection* target = message.destination.IsEmpty() ? NULL
			: _Find(message.destination);
		std::vector<Connection*> connections(fConnections);
		for (size_t i = 0; i < connections.size(); i++) {
			Connection* connection = connections[i];
			if (connection == target)
				continue;
			for (size_t r = 0; r < connection->rules.size(); r++) {
				if (connection->rules[r].Matches(message)) {
					_Deliver(connection, message);
					break;
				}
			}
		}
		if (target != NULL)
			_Deliver(target, message);
		return;
	}

	Service* service = message.type == METHOD_CALL
		? _FindService(message.destination) : NULL;
	if (service != NULL) {
		service->HandleCall(message);
		return;
	}
	Connection* target = _Find(message.destination);
	if (target != NULL) {
		_Deliver(target, message);
		// eavesdroppers on calls are not supported
		return;
	}
	if (message.type == METHOD_CALL && from != NULL
		&& (message.flags & NO_REPLY_EXPECTED) == 0) {
		BString text;
		text << "The name " << message.destination
			<< " was not provided by any .service files";
		Message error = message.ErrorReply(
			"org.freedesktop.DBus.Error.ServiceUnknown", text.String());
		error.sender = kBusName;
		error.serial = NextSerial();
		_Deliver(from, error);
	}
	// replies to a client that has gone, or to a service: dropped
}


void
SessionBus::_SetOwner(const BString& name, const BString& owner)
{
	BString old;
	std::map<std::string, std::string>::iterator found
		= fOwners.find(name.String());
	if (found != fOwners.end())
		old = found->second.c_str();
	if (owner.IsEmpty())
		fOwners.erase(name.String());
	else
		fOwners[name.String()] = owner.String();

	DValueList values;
	values.push_back(DValue::String(name));
	values.push_back(DValue::String(old));
	values.push_back(DValue::String(owner));
	Message signal = Message::Signal("/org/freedesktop/DBus", kBusName,
		"NameOwnerChanged");
	signal.sender = kBusName;
	signal.serial = NextSerial();
	signal.SetArguments(values);
	_Route(signal, NULL);
}


void
SessionBus::_BusMethod(Connection* from, const Message& message)
{
	DValueList arguments;
	message.GetArguments(arguments);
	DValueList result;
	const char* errorName = NULL;
	BString errorText;
	const BString& member = message.member;
	const BString& interface = message.interface;

	if (interface == "org.freedesktop.DBus.Peer") {
		if (member == "GetMachineId") {
			BString id = fGUID;
			result.push_back(DValue::String(id));
		} else if (member != "Ping")
			errorName = "org.freedesktop.DBus.Error.UnknownMethod";
	} else if (interface == "org.freedesktop.DBus.Introspectable"
		&& member == "Introspect") {
		result.push_back(DValue::String(
			"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
			"Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/"
			"dbus/1.0/introspect.dtd\">\n<node>\n"
			" <interface name=\"org.freedesktop.DBus\">\n"
			"  <method name=\"Hello\"><arg direction=\"out\" type=\"s\"/>"
			"</method>\n </interface>\n</node>\n"));
	} else if (member == "Hello") {
		if (!from->uniqueName.IsEmpty()) {
			errorName = "org.freedesktop.DBus.Error.Failed";
			errorText = "Already handled an Hello message";
		} else {
			from->uniqueName.SetToFormat(":1.%u", (unsigned)fNextUnique++);
			result.push_back(DValue::String(from->uniqueName));
		}
	} else if (member == "RequestName" && arguments.size() == 2) {
		const BString& name = arguments[0].s;
		uint32 flags = arguments[1].u;
		if (name.StartsWith(":") || name == kBusName || name.IsEmpty()) {
			errorName = "org.freedesktop.DBus.Error.InvalidArgs";
			errorText = "Cannot request that name";
		} else {
			Connection* owner = _Find(name);
			bool serviceOwned = _FindService(name) != NULL;
			if (owner == from)
				result.push_back(DValue::UInt32(4));	// already owner
			else if ((owner == NULL && !serviceOwned)
				|| (serviceOwned && (flags & 0x2) != 0)) {
				// free, or a built-in service a client replaces
				_SetOwner(name, from->uniqueName);
				result.push_back(DValue::UInt32(1));	// primary owner
			} else
				result.push_back(DValue::UInt32(3));	// exists
		}
	} else if (member == "ReleaseName" && arguments.size() == 1) {
		Connection* owner = _Find(arguments[0].s);
		if (owner == from) {
			_SetOwner(arguments[0].s, "");
			result.push_back(DValue::UInt32(1));
		} else
			result.push_back(DValue::UInt32(owner == NULL ? 2 : 3));
	} else if (member == "GetNameOwner" && arguments.size() == 1) {
		const BString& name = arguments[0].s;
		Connection* owner = _Find(name);
		if (owner != NULL)
			result.push_back(DValue::String(owner->uniqueName));
		else if (_FindService(name) != NULL || name == kBusName)
			result.push_back(DValue::String(kBusName));
		else {
			errorName = "org.freedesktop.DBus.Error.NameHasNoOwner";
			errorText << "Could not get owner of name '" << name << "'";
		}
	} else if (member == "NameHasOwner" && arguments.size() == 1) {
		const BString& name = arguments[0].s;
		result.push_back(DValue::Bool(name == kBusName || _Find(name) != NULL
			|| _FindService(name) != NULL));
	} else if (member == "ListNames" || member == "ListActivatableNames") {
		DValue names = DValue::Array("s");
		names.Add(DValue::String(kBusName));
		if (member == "ListNames") {
			for (size_t i = 0; i < fConnections.size(); i++) {
				if (!fConnections[i]->uniqueName.IsEmpty())
					names.Add(DValue::String(fConnections[i]->uniqueName));
			}
		}
		for (std::map<std::string, std::string>::iterator it
				= fOwners.begin(); it != fOwners.end(); it++)
			names.Add(DValue::String(it->first.c_str()));
		result.push_back(names);
	} else if (member == "StartServiceByName" && arguments.size() == 2) {
		if (_Find(arguments[0].s) != NULL
			|| _FindService(arguments[0].s) != NULL)
			result.push_back(DValue::UInt32(2));	// already running
		else {
			errorName = "org.freedesktop.DBus.Error.ServiceUnknown";
			errorText << "The name " << arguments[0].s
				<< " was not provided by any .service files";
		}
	} else if (member == "AddMatch" && arguments.size() == 1) {
		MatchRule rule;
		if (!rule.Parse(arguments[0].s.String())
			|| from->rules.size() >= 512) {
			errorName = "org.freedesktop.DBus.Error.MatchRuleInvalid";
			errorText = "Invalid match rule";
		} else
			from->rules.push_back(rule);
	} else if (member == "RemoveMatch" && arguments.size() == 1) {
		bool removed = false;
		for (size_t i = 0; i < from->rules.size(); i++) {
			if (from->rules[i].text == arguments[0].s) {
				from->rules.erase(from->rules.begin() + i);
				removed = true;
				break;
			}
		}
		if (!removed) {
			errorName = "org.freedesktop.DBus.Error.MatchRuleNotFound";
			errorText = "The given match rule wasn't found";
		}
	} else if ((member == "GetConnectionUnixUser"
			|| member == "GetConnectionUnixProcessID")
		&& arguments.size() == 1) {
		Connection* owner = _Find(arguments[0].s);
		if (owner == NULL) {
			errorName = "org.freedesktop.DBus.Error.NameHasNoOwner";
			errorText = "No such name";
		} else {
			result.push_back(DValue::UInt32(member == "GetConnectionUnixUser"
				? (uint32)owner->uid : (uint32)owner->pid));
		}
	} else if (member == "GetId") {
		result.push_back(DValue::String(fGUID));
	} else if (member == "UpdateActivationEnvironment") {
		// nothing to start: accepted
	} else if (member == "GetConnectionCredentials" && arguments.size() == 1) {
		Connection* owner = _Find(arguments[0].s);
		if (owner == NULL) {
			errorName = "org.freedesktop.DBus.Error.NameHasNoOwner";
			errorText = "No such name";
		} else {
			DValue credentials = DValue::Array("{sv}");
			credentials.Add(DValue::DictEntry(DValue::String("UnixUserID"),
				DValue::Variant(DValue::UInt32(owner->uid))));
			credentials.Add(DValue::DictEntry(DValue::String("ProcessID"),
				DValue::Variant(DValue::UInt32(owner->pid))));
			result.push_back(credentials);
		}
	} else {
		errorName = "org.freedesktop.DBus.Error.UnknownMethod";
		errorText << "Unknown method " << member;
	}

	if ((message.flags & NO_REPLY_EXPECTED) != 0 && errorName == NULL)
		return;
	Message reply = errorName != NULL
		? message.ErrorReply(errorName, errorText.String()) : message.Reply();
	if (errorName == NULL)
		reply.SetArguments(result);
	reply.sender = kBusName;
	reply.destination = from->uniqueName;
	reply.serial = NextSerial();
	_Deliver(from, reply);

	if (member == "Hello" && errorName == NULL) {
		// NameAcquired for the unique name, then NameOwnerChanged
		DValueList values;
		values.push_back(DValue::String(from->uniqueName));
		Message acquired = Message::Signal("/org/freedesktop/DBus", kBusName,
			"NameAcquired");
		acquired.sender = kBusName;
		acquired.destination = from->uniqueName;
		acquired.serial = NextSerial();
		acquired.SetArguments(values);
		_Deliver(from, acquired);
		_SetOwner(from->uniqueName, from->uniqueName);
	}
}


void
SessionBus::_Drop(Connection* connection)
{
	for (size_t i = 0; i < fConnections.size(); i++) {
		if (fConnections[i] == connection) {
			fConnections.erase(fConnections.begin() + i);
			break;
		}
	}
	close(connection->fd);
	for (size_t i = 0; i < connection->inputFDs.size(); i++)
		close(connection->inputFDs[i]);
	for (size_t i = 0; i < connection->outputFDs.size(); i++)
		close(connection->outputFDs[i]);
	BString unique = connection->uniqueName;
	delete connection;
	if (unique.IsEmpty())
		return;

	std::vector<std::string> owned;
	for (std::map<std::string, std::string>::iterator it = fOwners.begin();
			it != fOwners.end(); it++) {
		if (it->second == unique.String() && it->first != unique.String())
			owned.push_back(it->first);
	}
	for (size_t i = 0; i < owned.size(); i++) {
		// built-in services take back their names
		bool builtIn = false;
		for (size_t s = 0; s < fServices.size() && !builtIn; s++) {
			for (const char* const* name = fServices[s]->Names();
					*name != NULL; name++) {
				if (owned[i] == *name)
					builtIn = true;
			}
		}
		_SetOwner(owned[i].c_str(), builtIn ? kBusName : "");
	}
	_SetOwner(unique, "");
}


}	// namespace DBus
