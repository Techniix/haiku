/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renga's D-Bus session bus (session_bus, roadmap F-43 phase D), spoken to
	over its socket as a D-Bus client library would:

	- auth_hello: EXTERNAL authentication with the user ID, Unix descriptor
	  negotiation, Hello() with a unique name and NameAcquired;
	- names: RequestName, GetNameOwner, NameHasOwner, ListNames, and
	  NameOwnerChanged to a match rule when the owner disconnects;
	- routing: a method call from one client to a name another owns, with
	  the sender filled in by the bus, and its reply back;
	- fd_passing: a pipe end sent along with a message arrives usable;
	- unknown_name: a call to a name nobody owns gets ServiceUnknown;
	- notifications: GetServerInformation and Notify (an ID back);
	- upower: EnumerateDevices, GetDisplayDevice and OnBattery;
	- portal_settings: org.freedesktop.appearance color-scheme;
	- secrets: OpenSession("plain"), CreateItem with attributes, SearchItems,
	  GetSecrets returning the secret, Delete.

	Only on images with the bus (/boot/system/servers/session_bus); the test
	starts it when no session runs it (harness boots have no desktop).
*/


#include "DBusMessage.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>


using namespace DBus;


#define SUITE "dbus"

static const char* kBus = "/boot/system/servers/session_bus";
static const char* kSocket = "/tmp/xdg-runtime/bus";
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[512];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


class Client {
public:
	Client() : fFD(-1), fSerial(1) {}
	~Client() { Close(); }

	void Close()
	{
		if (fFD >= 0)
			close(fFD);
		fFD = -1;
	}

	bool Connect(BString& error)
	{
		fFD = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un address;
		memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		strlcpy(address.sun_path, kSocket, sizeof(address.sun_path));
		if (connect(fFD, (struct sockaddr*)&address, sizeof(address)) != 0) {
			error.SetToFormat("connect: %s", strerror(errno));
			return false;
		}
		BString uid;
		uid << (int)getuid();
		BString hex;
		for (int32 i = 0; i < uid.Length(); i++)
			hex << BString().SetToFormat("%02x", (uint8)uid[i]);
		BString hello;
		hello << '\0' << "AUTH EXTERNAL " << hex << "\r\n";
		write(fFD, "\0", 1);
		write(fFD, hello.String() + 1, hello.Length() - 1);
		BString line = _Line();
		if (!line.StartsWith("OK ")) {
			error << "auth: [" << line << "]";
			return false;
		}
		write(fFD, "NEGOTIATE_UNIX_FD\r\n", 19);
		line = _Line();
		if (line != "AGREE_UNIX_FD") {
			error << "unix fd: [" << line << "]";
			return false;
		}
		write(fFD, "BEGIN\r\n", 7);
		Message reply;
		if (!Call(Message::MethodCall("org.freedesktop.DBus",
				"/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello"),
				reply) || reply.type != METHOD_RETURN) {
			error = "Hello failed";
			return false;
		}
		DValueList values;
		reply.GetArguments(values);
		if (values.size() == 1)
			fUnique = values[0].s;
		return !fUnique.IsEmpty();
	}

	const BString& Unique() const { return fUnique; }

	bool Send(Message& message)
	{
		message.serial = fSerial++;
		std::vector<uint8> bytes;
		message.Serialize(bytes);
		struct iovec vector = { bytes.data(), bytes.size() };
		struct msghdr header;
		memset(&header, 0, sizeof(header));
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		char control[CMSG_SPACE(sizeof(int) * 4)];
		if (!message.fds.empty()) {
			header.msg_control = control;
			header.msg_controllen = CMSG_SPACE(sizeof(int)
				* message.fds.size());
			struct cmsghdr* cmsg = CMSG_FIRSTHDR(&header);
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN(sizeof(int) * message.fds.size());
			memcpy(CMSG_DATA(cmsg), message.fds.data(),
				sizeof(int) * message.fds.size());
		}
		return sendmsg(fFD, &header, 0) == (ssize_t)bytes.size();
	}

	//! The next message, within \a timeout.
	bool Receive(Message& message, bigtime_t timeout = 5000000)
	{
		bigtime_t deadline = system_time() + timeout;
		while (true) {
			ssize_t needed = Message::NeededSize(fInput.data(),
				fInput.size());
			if (needed > 0 && (size_t)needed <= fInput.size()) {
				if (message.Parse(fInput.data(), needed) != B_OK)
					return false;
				fInput.erase(fInput.begin(), fInput.begin() + needed);
				size_t count = std::min((size_t)message.unixFDCount,
					fFDs.size());
				message.fds.assign(fFDs.begin(), fFDs.begin() + count);
				fFDs.erase(fFDs.begin(), fFDs.begin() + count);
				return true;
			}
			bigtime_t left = deadline - system_time();
			if (left <= 0)
				return false;
			struct pollfd entry = { fFD, POLLIN, 0 };
			if (poll(&entry, 1, left / 1000 + 1) <= 0)
				continue;
			uint8 buffer[16384];
			struct iovec vector = { buffer, sizeof(buffer) };
			char control[CMSG_SPACE(sizeof(int) * 8)];
			struct msghdr header;
			memset(&header, 0, sizeof(header));
			header.msg_iov = &vector;
			header.msg_iovlen = 1;
			header.msg_control = control;
			header.msg_controllen = sizeof(control);
			ssize_t bytes = recvmsg(fFD, &header, 0);
			if (bytes <= 0)
				return false;
			for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != NULL;
					cmsg = CMSG_NXTHDR(&header, cmsg)) {
				if (cmsg->cmsg_type != SCM_RIGHTS)
					continue;
				int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
				for (int i = 0; i < count; i++)
					fFDs.push_back(((int*)CMSG_DATA(cmsg))[i]);
			}
			fInput.insert(fInput.end(), buffer, buffer + bytes);
		}
	}

	//!	Sends \a call and waits for its reply (other messages are kept).
	bool Call(Message call, Message& reply)
	{
		reply = Message();
		if (!Send(call))
			return false;
		uint32 serial = call.serial;
		for (size_t i = 0; i < fPending.size(); i++) {
			if (fPending[i].replySerial == serial) {
				reply = fPending[i];
				fPending.erase(fPending.begin() + i);
				return true;
			}
		}
		Message message;
		while (Receive(message)) {
			if ((message.type == METHOD_RETURN || message.type == ERROR)
				&& message.replySerial == serial) {
				reply = message;
				return true;
			}
			fPending.push_back(message);
		}
		return false;
	}

	//!	A queued or new message for which \a match holds.
	template<typename Match>
	bool WaitFor(Message& result, Match match, bigtime_t timeout = 5000000)
	{
		for (size_t i = 0; i < fPending.size(); i++) {
			if (match(fPending[i])) {
				result = fPending[i];
				fPending.erase(fPending.begin() + i);
				return true;
			}
		}
		bigtime_t deadline = system_time() + timeout;
		Message message;
		while (system_time() < deadline
			&& Receive(message, deadline - system_time())) {
			if (match(message)) {
				result = message;
				return true;
			}
			fPending.push_back(message);
		}
		return false;
	}

private:
	BString _Line()
	{
		BString line;
		char c;
		bigtime_t deadline = system_time() + 5000000;
		while (system_time() < deadline) {
			struct pollfd entry = { fFD, POLLIN, 0 };
			if (poll(&entry, 1, 100) <= 0)
				continue;
			if (read(fFD, &c, 1) != 1)
				break;
			if (c == '\n')
				break;
			if (c != '\r')
				line << c;
		}
		return line;
	}

	int					fFD;
	uint32				fSerial;
	BString				fUnique;
	std::vector<uint8>	fInput;
	std::vector<int>	fFDs;
	std::vector<Message> fPending;
};


static Message
call(const char* destination, const char* path, const char* interface,
	const char* member, const DValueList& arguments = DValueList())
{
	Message message = Message::MethodCall(destination, path, interface,
		member);
	message.SetArguments(arguments);
	return message;
}


static bool
start_bus()
{
	if (access(kSocket, F_OK) == 0) {
		Client probe;
		BString error;
		if (probe.Connect(error))
			return true;
	}
	if (access(kBus, X_OK) != 0)
		return false;
	mkdir("/tmp/xdg-runtime", 0700);
	const char* arguments[] = { kBus, NULL };
	thread_id thread = load_image(1, arguments, (const char**)environ);
	if (thread < 0)
		return false;
	resume_thread(thread);
	for (int i = 0; i < 50; i++) {
		snooze(100000);
		Client probe;
		BString error;
		if (probe.Connect(error))
			return true;
	}
	return false;
}


static void
test_bus()
{
	Client first, second;
	BString error;
	bool connected = first.Connect(error);
	Message acquired;
	bool gotAcquired = connected && first.WaitFor(acquired,
		[](const Message& m) { return m.type == SIGNAL
			&& m.member == "NameAcquired"; });
	report("auth_hello", connected && gotAcquired
			&& first.Unique().StartsWith(":1."),
		"unique name %s, NameAcquired %s %s", first.Unique().String(),
		gotAcquired ? "received" : "missing", error.String());
	if (!connected || !second.Connect(error)) {
		report("names", false, "second client: %s", error.String());
		return;
	}

	// names
	DValueList arguments;
	arguments.push_back(DValue::String("org.renga.Test"));
	arguments.push_back(DValue::UInt32(0));
	Message reply;
	first.Call(call("org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "RequestName", arguments), reply);
	DValueList values;
	reply.GetArguments(values);
	uint32 requested = values.size() == 1 ? values[0].u : 0;

	DValueList nameOnly;
	nameOnly.push_back(DValue::String("org.renga.Test"));
	second.Call(call("org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetNameOwner", nameOnly), reply);
	reply.GetArguments(values);
	BString owner = values.size() == 1 ? values[0].s : BString("?");
	second.Call(call("org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameHasOwner", nameOnly), reply);
	reply.GetArguments(values);
	bool hasOwner = values.size() == 1 && values[0].u != 0;
	second.Call(call("org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "ListNames"), reply);
	reply.GetArguments(values);
	bool listed = false;
	if (values.size() == 1) {
		for (size_t i = 0; i < values[0].items.size(); i++) {
			if (values[0].items[i].s == "org.renga.Test")
				listed = true;
		}
	}

	// routing
	DValueList ping;
	ping.push_back(DValue::String("ping"));
	Message request = call("org.renga.Test", "/org/renga/Test",
		"org.renga.Test", "Echo", ping);
	second.Send(request);
	Message received;
	bool gotCall = first.WaitFor(received, [](const Message& m) {
		return m.type == METHOD_CALL && m.member == "Echo"; });
	Message answer = received.Reply();
	DValueList pong;
	pong.push_back(DValue::String("pong"));
	answer.SetArguments(pong);
	first.Send(answer);
	uint32 requestSerial = request.serial;
	Message echoed;
	bool gotReply = second.WaitFor(echoed, [requestSerial](const Message& m) {
		return m.type == METHOD_RETURN && m.replySerial == requestSerial; });
	echoed.GetArguments(values);
	report("routing", gotCall && received.sender == second.Unique()
			&& gotReply && values.size() == 1 && values[0].s == "pong"
			&& echoed.sender == first.Unique(),
		"call %s (sender %s, want %s), reply %s (%s, sender %s)",
		gotCall ? "delivered" : "missing", received.sender.String(),
		second.Unique().String(), gotReply ? "delivered" : "missing",
		values.size() == 1 ? values[0].s.String() : "-",
		echoed.sender.String());

	// descriptor passing
	int pipeFDs[2];
	pipe(pipeFDs);
	Message withFD = call("org.renga.Test", "/org/renga/Test",
		"org.renga.Test", "TakeFD");
	DValueList fdArgument;
	DValue handle;
	handle.type = 'h';
	handle.u = 0;
	fdArgument.push_back(handle);
	withFD.SetArguments(fdArgument);
	withFD.fds.push_back(pipeFDs[0]);
	withFD.flags = NO_REPLY_EXPECTED;
	second.Send(withFD);
	close(pipeFDs[0]);
	write(pipeFDs[1], "renga", 5);
	close(pipeFDs[1]);
	Message fdMessage;
	bool gotFD = first.WaitFor(fdMessage, [](const Message& m) {
		return m.member == "TakeFD"; });
	char data[8] = "";
	ssize_t bytes = gotFD && fdMessage.fds.size() == 1
		? read(fdMessage.fds[0], data, 5) : -1;
	report("fd_passing", gotFD && bytes == 5 && memcmp(data, "renga", 5) == 0,
		"message %s, %d descriptors, read %d bytes [%.5s]",
		gotFD ? "delivered" : "missing", (int)fdMessage.fds.size(),
		(int)bytes, bytes > 0 ? data : "");
	for (size_t i = 0; i < fdMessage.fds.size(); i++)
		close(fdMessage.fds[i]);

	// NameOwnerChanged when the owner goes
	DValueList rule;
	rule.push_back(DValue::String("type='signal',interface="
		"'org.freedesktop.DBus',member='NameOwnerChanged',"
		"arg0='org.renga.Test'"));
	second.Call(call("org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "AddMatch", rule), reply);
	bool matchAdded = reply.type == METHOD_RETURN;
	first.Close();
	Message changed;
	bool gotChanged = second.WaitFor(changed, [](const Message& m) {
		return m.type == SIGNAL && m.member == "NameOwnerChanged"; });
	changed.GetArguments(values);
	report("names", requested == 1 && owner == first.Unique() && hasOwner
			&& listed && matchAdded && gotChanged && values.size() == 3
			&& values[0].s == "org.renga.Test" && values[2].s.IsEmpty(),
		"RequestName %u, owner %s, NameHasOwner %d, listed %d, AddMatch %d, "
		"NameOwnerChanged on disconnect %s", (unsigned)requested,
		owner.String(), hasOwner, listed, matchAdded,
		gotChanged ? "received" : "missing");

	// unknown names
	second.Call(call("org.renga.Nobody", "/", "org.renga.Nobody", "Hi"),
		reply);
	report("unknown_name", reply.type == ERROR && reply.errorName
			== "org.freedesktop.DBus.Error.ServiceUnknown",
		"reply type %d, error %s", reply.type, reply.errorName.String());
}


static void
test_services()
{
	Client client;
	BString error;
	if (!client.Connect(error)) {
		report("notifications", false, "connect: %s", error.String());
		return;
	}
	Message reply;
	DValueList values;

	// notifications
	client.Call(call("org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"GetServerInformation"), reply);
	reply.GetArguments(values);
	BString server = values.size() == 4 ? values[0].s : BString("-");
	DValueList notify;
	notify.push_back(DValue::String("dbus_test"));
	notify.push_back(DValue::UInt32(0));
	notify.push_back(DValue::String(""));
	notify.push_back(DValue::String("Renga test"));
	notify.push_back(DValue::String("A <b>D-Bus</b> notification"));
	notify.push_back(DValue::Array("s"));
	notify.push_back(DValue::Array("{sv}"));
	notify.push_back(DValue::Int32(2000));
	client.Call(call("org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"Notify", notify), reply);
	reply.GetArguments(values);
	uint32 id = values.size() == 1 ? values[0].u : 0;
	report("notifications", server == "Renga" && reply.type == METHOD_RETURN
			&& id > 0,
		"server \"%s\", Notify -> id %u (%s)", server.String(), (unsigned)id,
		reply.type == ERROR ? reply.errorName.String() : "ok");

	// UPower
	client.Call(call("org.freedesktop.UPower", "/org/freedesktop/UPower",
		"org.freedesktop.UPower", "EnumerateDevices"), reply);
	reply.GetArguments(values);
	bool enumerated = reply.type == METHOD_RETURN && values.size() == 1
		&& values[0].type == 'a';
	client.Call(call("org.freedesktop.UPower", "/org/freedesktop/UPower",
		"org.freedesktop.UPower", "GetDisplayDevice"), reply);
	reply.GetArguments(values);
	BString display = values.size() == 1 ? values[0].s : BString("-");
	DValueList property;
	property.push_back(DValue::String("org.freedesktop.UPower"));
	property.push_back(DValue::String("OnBattery"));
	client.Call(call("org.freedesktop.UPower", "/org/freedesktop/UPower",
		"org.freedesktop.DBus.Properties", "Get", property), reply);
	reply.GetArguments(values);
	bool onBatteryKnown = values.size() == 1
		&& values[0].Unwrapped().type == 'b';
	report("upower", enumerated && display.EndsWith("DisplayDevice")
			&& onBatteryKnown,
		"EnumerateDevices %s, display device %s, OnBattery %s",
		enumerated ? "ok" : "failed", display.String(),
		onBatteryKnown ? (values[0].Unwrapped().u ? "true" : "false")
			: "missing");

	// portal settings
	DValueList read;
	read.push_back(DValue::String("org.freedesktop.appearance"));
	read.push_back(DValue::String("color-scheme"));
	client.Call(call("org.freedesktop.portal.Desktop",
		"/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
		"ReadOne", read), reply);
	reply.GetArguments(values);
	uint32 scheme = values.size() == 1 ? values[0].Unwrapped().u : 99;
	report("portal_settings", reply.type == METHOD_RETURN
			&& (scheme == 1 || scheme == 2),
		"color-scheme %u (1 dark, 2 light)", (unsigned)scheme);

	// Secret Service
	DValueList open;
	open.push_back(DValue::String("plain"));
	open.push_back(DValue::Variant(DValue::String("")));
	client.Call(call("org.freedesktop.secrets", "/org/freedesktop/secrets",
		"org.freedesktop.Secret.Service", "OpenSession", open), reply);
	reply.GetArguments(values);
	BString session = reply.type == METHOD_RETURN && values.size() == 2
		? values[1].s : BString();

	DValue attributes = DValue::Array("{ss}");
	attributes.Add(DValue::DictEntry(DValue::String("application"),
		DValue::String("renga-dbus-test")));
	DValue properties = DValue::Array("{sv}");
	properties.Add(DValue::DictEntry(
		DValue::String("org.freedesktop.Secret.Item.Label"),
		DValue::Variant(DValue::String("Renga test secret"))));
	properties.Add(DValue::DictEntry(
		DValue::String("org.freedesktop.Secret.Item.Attributes"),
		DValue::Variant(attributes)));
	DValue secret = DValue::Struct();
	secret.Add(DValue::ObjectPath(session.IsEmpty() ? "/" : session.String()));
	secret.Add(DValue::Array("y"));
	DValue bytes = DValue::Array("y");
	for (const char* c = "hunter2"; *c != '\0'; c++)
		bytes.Add(DValue::Byte(*c));
	secret.Add(bytes);
	secret.Add(DValue::String("text/plain"));
	DValueList create;
	create.push_back(properties);
	create.push_back(secret);
	create.push_back(DValue::Bool(true));
	client.Call(call("org.freedesktop.secrets",
		"/org/freedesktop/secrets/collection/login",
		"org.freedesktop.Secret.Collection", "CreateItem", create), reply);
	reply.GetArguments(values);
	BString item = reply.type == METHOD_RETURN && values.size() == 2
		? values[0].s : BString();
	BString createError = reply.errorName;

	DValueList search;
	search.push_back(attributes);
	client.Call(call("org.freedesktop.secrets", "/org/freedesktop/secrets",
		"org.freedesktop.Secret.Service", "SearchItems", search), reply);
	reply.GetArguments(values);
	bool found = reply.type == METHOD_RETURN && values.size() == 2
		&& values[0].items.size() == 1 && values[0].items[0].s == item;

	DValueList get;
	DValue items = DValue::Array("o");
	items.Add(DValue::ObjectPath(item.IsEmpty() ? "/" : item.String()));
	get.push_back(items);
	get.push_back(DValue::ObjectPath(session.IsEmpty() ? "/"
		: session.String()));
	client.Call(call("org.freedesktop.secrets", "/org/freedesktop/secrets",
		"org.freedesktop.Secret.Service", "GetSecrets", get), reply);
	reply.GetArguments(values);
	BString value;
	if (reply.type == METHOD_RETURN && values.size() == 1
		&& values[0].items.size() == 1
		&& values[0].items[0].items.size() == 2) {
		const DValue& entry = values[0].items[0].items[1];
		if (entry.items.size() == 4 && entry.items[2].type == 'a') {
			for (size_t i = 0; i < entry.items[2].items.size(); i++)
				value << (char)entry.items[2].items[i].u;
		}
	}

	client.Call(call("org.freedesktop.secrets",
		item.IsEmpty() ? "/" : item.String(), "org.freedesktop.Secret.Item",
		"Delete"), reply);
	bool deleted = reply.type == METHOD_RETURN;
	client.Call(call("org.freedesktop.secrets", "/org/freedesktop/secrets",
		"org.freedesktop.Secret.Service", "SearchItems", search), reply);
	reply.GetArguments(values);
	bool gone = reply.type == METHOD_RETURN && values.size() == 2
		&& values[0].items.empty();
	report("secrets", !session.IsEmpty() && !item.IsEmpty() && found
			&& value == "hunter2" && deleted && gone,
		"session %s, item %s %s, found %d, secret [%s], deleted %d, gone %d",
		session.String(), item.String(), createError.String(), found,
		value.String(), deleted, gone);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);
	if (access(kBus, X_OK) != 0) {
		printf("CE-INFO dbus: no session bus in this image\n");
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}
	bool running = start_bus();
	report("bus_running", running, "%s", kSocket);
	if (running) {
		test_bus();
		test_services();
	}
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
