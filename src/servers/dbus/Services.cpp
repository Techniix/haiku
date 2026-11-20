/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	The services inside Renga's session bus (roadmap F-43 phase D), for
	Linux programs under Renguin and ported programs:

	- org.freedesktop.Notifications: Notify() shows a native notification
	  (notification_server); a click is the "default" action
	  (ActionInvoked), closing a notification is reported as
	  NotificationClosed. Switch: ~/config/settings/Renga/dbus
	  "notifications false" (Notifications preferences).
	- org.freedesktop.UPower: the batteries the ACPI battery driver sees
	  (/dev/power/acpi_battery), as UPower devices with a display device;
	  OnBattery. Machines without a battery (desktops, QEMU) have none,
	  like UPower on such machines.
	- org.freedesktop.portal.Desktop: Settings (org.freedesktop.appearance
	  color-scheme from the panel colour, accent-color, contrast) and
	  OpenURI (the system's preferred application for the URI or file).
	  FileChooser, Screenshot and ScreenCast are not offered yet.
	- org.freedesktop.secrets: the Secret Service, kept in a "Secret
	  Service" keyring of Haiku's key store (keystore_server), so secrets
	  are held where the system keeps its other passwords. One collection,
	  "login" (alias "default"), always unlocked; "plain" sessions only
	  (the bus is a local socket readable by the user alone).
*/


#include "Services.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <File.h>
#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <Key.h>
#include <KeyStore.h>
#include <Notification.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <Url.h>

#include <power_managment.h>


namespace DBus {


static const char* kPropertiesInterface = "org.freedesktop.DBus.Properties";


//!	A setting in ~/config/settings/Renga/dbus ("<name> false" turns it off).
static bool
setting_enabled(const char* name)
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append("Renga/dbus") != B_OK)
		return true;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return true;
	char line[256];
	bool enabled = true;
	size_t nameLength = strlen(name);
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, name, nameLength) == 0
			&& isspace(line[nameLength])
			&& strstr(line + nameLength, "false") != NULL)
			enabled = false;
	}
	fclose(file);
	return enabled;
}


static DValue
string_array(const char* const* strings)
{
	DValue array = DValue::Array("s");
	for (; *strings != NULL; strings++)
		array.Add(DValue::String(*strings));
	return array;
}


static DValue
dict_entry(const char* key, const DValue& value)
{
	return DValue::DictEntry(DValue::String(key), DValue::Variant(value));
}


static BString
introspection(const char* interfaces)
{
	BString xml;
	xml << "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
		"Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/"
		"1.0/introspect.dtd\">\n<node>\n" << interfaces << "</node>\n";
	return xml;
}


// #pragma mark - notifications


NotificationsService::NotificationsService(SessionBus* bus)
	:
	Service(bus),
	fNextID(1)
{
}


const char* const*
NotificationsService::Names() const
{
	static const char* const kNames[] = { "org.freedesktop.Notifications",
		NULL };
	return kNames;
}


//!	Notification bodies may carry a small HTML subset; shown as text.
static BString
strip_markup(const BString& text)
{
	BString result;
	bool inTag = false;
	for (int32 i = 0; i < text.Length(); i++) {
		char c = text[i];
		if (c == '<')
			inTag = true;
		else if (c == '>')
			inTag = false;
		else if (!inTag)
			result << c;
	}
	result.ReplaceAll("&lt;", "<");
	result.ReplaceAll("&gt;", ">");
	result.ReplaceAll("&quot;", "\"");
	result.ReplaceAll("&apos;", "'");
	result.ReplaceAll("&amp;", "&");
	return result;
}


void
NotificationsService::HandleCall(const Message& call)
{
	DValueList arguments;
	if (call.GetArguments(arguments) != B_OK) {
		Error(call, "org.freedesktop.DBus.Error.InvalidArgs", "Bad arguments");
		return;
	}
	const BString& member = call.member;
	if (call.interface == "org.freedesktop.DBus.Introspectable") {
		DValueList result;
		result.push_back(DValue::String(introspection(
			" <interface name=\"org.freedesktop.Notifications\">\n"
			"  <method name=\"Notify\"><arg type=\"s\"/><arg type=\"u\"/>"
			"<arg type=\"s\"/><arg type=\"s\"/><arg type=\"s\"/>"
			"<arg type=\"as\"/><arg type=\"a{sv}\"/><arg type=\"i\"/>"
			"<arg type=\"u\" direction=\"out\"/></method>\n"
			"  <method name=\"CloseNotification\"><arg type=\"u\"/>"
			"</method>\n"
			"  <method name=\"GetCapabilities\"><arg type=\"as\" "
			"direction=\"out\"/></method>\n"
			"  <method name=\"GetServerInformation\"><arg type=\"s\" "
			"direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
			"<arg type=\"s\" direction=\"out\"/><arg type=\"s\" "
			"direction=\"out\"/></method>\n"
			"  <signal name=\"NotificationClosed\"><arg type=\"u\"/>"
			"<arg type=\"u\"/></signal>\n"
			"  <signal name=\"ActionInvoked\"><arg type=\"u\"/>"
			"<arg type=\"s\"/></signal>\n"
			" </interface>\n").String()));
		Reply(call, result);
		return;
	}

	DValueList result;
	if (member == "GetCapabilities") {
		static const char* const kCapabilities[] = { "body", "actions",
			"persistence", NULL };
		result.push_back(string_array(kCapabilities));
	} else if (member == "GetServerInformation") {
		result.push_back(DValue::String("Renga"));
		result.push_back(DValue::String("Techniix"));
		result.push_back(DValue::String("1.0"));
		result.push_back(DValue::String("1.2"));
	} else if (member == "Notify" && arguments.size() == 8) {
		uint32 id = arguments[1].u != 0 ? (uint32)arguments[1].u
			: fNextID++;
		const DValue* urgency = arguments[6].Find("urgency");
		bool urgent = urgency != NULL && urgency->Unwrapped().u >= 2;
		int32 timeout = arguments[7].i;
		const DValue& actions = arguments[5];
		bool hasDefault = false;
		for (size_t i = 0; i + 1 < actions.items.size(); i += 2) {
			if (actions.items[i].s == "default")
				hasDefault = true;
		}
		if (setting_enabled("notifications")) {
			BNotification notification(urgent ? B_IMPORTANT_NOTIFICATION
				: B_INFORMATION_NOTIFICATION);
			notification.SetGroup(arguments[0].s.IsEmpty()
				? BString("Linux programs") : arguments[0].s);
			notification.SetTitle(strip_markup(arguments[3].s));
			notification.SetContent(strip_markup(arguments[4].s));
			BString messageID;
			messageID << "dbus:" << id;
			notification.SetMessageID(messageID);
			// a click comes back to the bus as the "default" action
			notification.SetOnClickApp(kSessionBusSignature);
			BString idArgument;
			idArgument << id;
			notification.AddOnClickArg("--notification-clicked");
			notification.AddOnClickArg(idArgument);
			notification.AddOnClickArg(call.sender);
			notification.Send(timeout > 0 ? (bigtime_t)timeout * 1000 : -1);
		}
		fOwners[id] = call.sender.String();
		fDefaultAction[id] = hasDefault;
		result.push_back(DValue::UInt32(id));
	} else if (member == "CloseNotification" && arguments.size() == 1) {
		uint32 id = arguments[0].u;
		DValueList values;
		values.push_back(DValue::UInt32(id));
		values.push_back(DValue::UInt32(3));	// closed by a call
		EmitSignal("org.freedesktop.Notifications",
			"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
			"NotificationClosed", values);
		fOwners.erase(id);
	} else {
		Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
			"Unknown method");
		return;
	}
	Reply(call, result);
}


void
NotificationsService::Clicked(uint32 id)
{
	DValueList values;
	values.push_back(DValue::UInt32(id));
	values.push_back(DValue::String("default"));
	EmitSignal("org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"ActionInvoked", values);
	DValueList closed;
	closed.push_back(DValue::UInt32(id));
	closed.push_back(DValue::UInt32(2));	// dismissed by the user
	EmitSignal("org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"NotificationClosed", closed);
}


// #pragma mark - UPower


UPowerService::UPowerService(SessionBus* bus)
	:
	Service(bus)
{
}


const char* const*
UPowerService::Names() const
{
	static const char* const kNames[] = { "org.freedesktop.UPower", NULL };
	return kNames;
}


struct battery_state {
	bool		present;
	bool		charging;
	bool		discharging;
	double		percentage;
	int64		timeToEmpty;
	BString		model;
	BString		path;
};


static void
find_batteries(const char* directory, std::vector<battery_state>& batteries)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return;
	while (struct dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		BString path;
		path << directory << "/" << entry->d_name;
		int fd = open(path.String(), O_RDONLY);
		if (fd < 0) {
			find_batteries(path.String(), batteries);
			continue;
		}
		uint32 magic = 0;
		acpi_battery_info info;
		acpi_extended_battery_info extended;
		if (ioctl(fd, IDENTIFY_DEVICE, &magic, sizeof(magic)) == 0
			&& magic == kMagicACPIBatteryID
			&& ioctl(fd, GET_BATTERY_INFO, &info, sizeof(info)) == 0
			&& ioctl(fd, GET_EXTENDED_BATTERY_INFO, &extended,
				sizeof(extended)) == 0) {
			battery_state state;
			state.present = extended.last_full_charge > 0
				&& extended.last_full_charge != (uint32)-1;
			state.charging = (info.state & BATTERY_CHARGING) != 0;
			state.discharging = (info.state & BATTERY_DISCHARGING) != 0;
			state.percentage = state.present ? 100.0 * info.capacity
				/ extended.last_full_charge : 0;
			if (state.percentage > 100)
				state.percentage = 100;
			state.timeToEmpty = state.discharging && info.current_rate > 0
				? (int64)info.capacity * 3600 / info.current_rate : 0;
			state.model = extended.model_number;
			state.path = path;
			batteries.push_back(state);
		}
		close(fd);
	}
	closedir(dir);
}


static DValue
device_properties(const battery_state* battery)
{
	// Type 2 battery; State 1 charging, 2 discharging, 4 fully charged
	DValue properties = DValue::Array("{sv}");
	bool present = battery != NULL && battery->present;
	uint32 state = !present ? 0 : battery->charging ? 1
		: battery->discharging ? 2 : 4;
	properties.Add(dict_entry("NativePath", DValue::String(
		battery != NULL ? battery->path.String() : "")));
	properties.Add(dict_entry("Vendor", DValue::String("")));
	properties.Add(dict_entry("Model", DValue::String(
		battery != NULL ? battery->model.String() : "")));
	properties.Add(dict_entry("Type", DValue::UInt32(present ? 2 : 0)));
	properties.Add(dict_entry("PowerSupply", DValue::Bool(present)));
	properties.Add(dict_entry("Online", DValue::Bool(false)));
	properties.Add(dict_entry("IsPresent", DValue::Bool(present)));
	properties.Add(dict_entry("IsRechargeable", DValue::Bool(present)));
	properties.Add(dict_entry("State", DValue::UInt32(state)));
	properties.Add(dict_entry("Percentage", DValue::Double(
		present ? battery->percentage : 0)));
	properties.Add(dict_entry("TimeToEmpty", DValue::Int64(
		present ? battery->timeToEmpty : 0)));
	properties.Add(dict_entry("TimeToFull", DValue::Int64(0)));
	properties.Add(dict_entry("IconName", DValue::String(
		present ? "battery-good-symbolic" : "")));
	properties.Add(dict_entry("WarningLevel", DValue::UInt32(
		present && battery->percentage < 10 ? 3 : 1)));
	return properties;
}


void
UPowerService::HandleCall(const Message& call)
{
	DValueList arguments;
	call.GetArguments(arguments);
	std::vector<battery_state> batteries;
	find_batteries("/dev/power", batteries);

	const BString& path = call.path;
	const BString& member = call.member;
	DValueList result;

	if (call.interface == "org.freedesktop.DBus.Introspectable") {
		result.push_back(DValue::String(introspection(
			" <interface name=\"org.freedesktop.UPower\">\n"
			"  <method name=\"EnumerateDevices\"><arg type=\"ao\" "
			"direction=\"out\"/></method>\n"
			"  <method name=\"GetDisplayDevice\"><arg type=\"o\" "
			"direction=\"out\"/></method>\n"
			"  <property name=\"OnBattery\" type=\"b\" access=\"read\"/>\n"
			" </interface>\n").String()));
		Reply(call, result);
		return;
	}

	bool onBattery = false;
	for (size_t i = 0; i < batteries.size(); i++) {
		if (batteries[i].present && batteries[i].discharging)
			onBattery = true;
	}

	if (path == "/org/freedesktop/UPower") {
		if (member == "EnumerateDevices") {
			DValue devices = DValue::Array("o");
			for (size_t i = 0; i < batteries.size(); i++) {
				BString object;
				object << "/org/freedesktop/UPower/devices/battery_BAT" << i;
				devices.Add(DValue::ObjectPath(object));
			}
			result.push_back(devices);
		} else if (member == "GetDisplayDevice") {
			result.push_back(DValue::ObjectPath(
				"/org/freedesktop/UPower/devices/DisplayDevice"));
		} else if (member == "GetCriticalAction") {
			result.push_back(DValue::String("PowerOff"));
		} else if (call.interface == kPropertiesInterface) {
			DValue properties = DValue::Array("{sv}");
			properties.Add(dict_entry("DaemonVersion",
				DValue::String("1.90.0")));
			properties.Add(dict_entry("OnBattery", DValue::Bool(onBattery)));
			properties.Add(dict_entry("LidIsClosed", DValue::Bool(false)));
			properties.Add(dict_entry("LidIsPresent", DValue::Bool(false)));
			if (member == "GetAll")
				result.push_back(properties);
			else if (member == "Get" && arguments.size() == 2) {
				const DValue* value = properties.Find(arguments[1].s);
				if (value == NULL) {
					Error(call, "org.freedesktop.DBus.Error.InvalidArgs",
						"No such property");
					return;
				}
				result.push_back(*value);
			} else {
				Error(call, "org.freedesktop.DBus.Error.PropertyReadOnly",
					"Read-only");
				return;
			}
		} else {
			Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
				"Unknown method");
			return;
		}
		Reply(call, result);
		return;
	}

	const battery_state* battery = NULL;
	bool known = false;
	if (path == "/org/freedesktop/UPower/devices/DisplayDevice") {
		known = true;
		for (size_t i = 0; i < batteries.size() && battery == NULL; i++) {
			if (batteries[i].present)
				battery = &batteries[i];
		}
	} else if (path.StartsWith(
			"/org/freedesktop/UPower/devices/battery_BAT")) {
		size_t index = atoi(path.String()
			+ strlen("/org/freedesktop/UPower/devices/battery_BAT"));
		if (index < batteries.size()) {
			battery = &batteries[index];
			known = true;
		}
	}
	if (!known) {
		Error(call, "org.freedesktop.DBus.Error.UnknownObject",
			"No such object");
		return;
	}
	DValue properties = device_properties(battery);
	if (call.interface == kPropertiesInterface && member == "GetAll")
		result.push_back(properties);
	else if (call.interface == kPropertiesInterface && member == "Get"
		&& arguments.size() == 2) {
		const DValue* value = properties.Find(arguments[1].s);
		if (value == NULL) {
			Error(call, "org.freedesktop.DBus.Error.InvalidArgs",
				"No such property");
			return;
		}
		result.push_back(*value);
	} else if (member == "Refresh") {
		// nothing cached
	} else {
		Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
			"Unknown method");
		return;
	}
	Reply(call, result);
}


// #pragma mark - portal


PortalService::PortalService(SessionBus* bus)
	:
	Service(bus),
	fNextRequest(1)
{
}


const char* const*
PortalService::Names() const
{
	static const char* const kNames[] = { "org.freedesktop.portal.Desktop",
		NULL };
	return kNames;
}


static DValue
appearance_value(const BString& key, bool* found)
{
	*found = true;
	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	bool dark = (panel.red + panel.green + panel.blue) / 3 < 128;
	if (key == "color-scheme")
		return DValue::UInt32(dark ? 1 : 2);
			// 1 prefer dark, 2 prefer light
	if (key == "accent-color") {
		rgb_color accent = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
		DValue color = DValue::Struct();
		color.Add(DValue::Double(accent.red / 255.0))
			.Add(DValue::Double(accent.green / 255.0))
			.Add(DValue::Double(accent.blue / 255.0));
		return color;
	}
	if (key == "contrast")
		return DValue::UInt32(0);
	*found = false;
	return DValue();
}


void
PortalService::HandleCall(const Message& call)
{
	DValueList arguments;
	call.GetArguments(arguments);
	const BString& interface = call.interface;
	const BString& member = call.member;
	DValueList result;

	if (interface == "org.freedesktop.DBus.Introspectable") {
		result.push_back(DValue::String(introspection(
			" <interface name=\"org.freedesktop.portal.Settings\">\n"
			"  <method name=\"ReadAll\"><arg type=\"as\"/>"
			"<arg type=\"a{sa{sv}}\" direction=\"out\"/></method>\n"
			"  <method name=\"Read\"><arg type=\"s\"/><arg type=\"s\"/>"
			"<arg type=\"v\" direction=\"out\"/></method>\n"
			"  <method name=\"ReadOne\"><arg type=\"s\"/><arg type=\"s\"/>"
			"<arg type=\"v\" direction=\"out\"/></method>\n"
			"  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
			" </interface>\n"
			" <interface name=\"org.freedesktop.portal.OpenURI\">\n"
			"  <method name=\"OpenURI\"><arg type=\"s\"/><arg type=\"s\"/>"
			"<arg type=\"a{sv}\"/><arg type=\"o\" direction=\"out\"/>"
			"</method>\n"
			"  <property name=\"version\" type=\"u\" access=\"read\"/>\n"
			" </interface>\n").String()));
		Reply(call, result);
		return;
	}

	if (interface == kPropertiesInterface && arguments.size() >= 1) {
		const BString& target = arguments[0].s;
		uint32 version = target == "org.freedesktop.portal.Settings" ? 2
			: target == "org.freedesktop.portal.OpenURI" ? 4 : 0;
		if (version == 0) {
			Error(call, "org.freedesktop.DBus.Error.UnknownInterface",
				"No such interface");
			return;
		}
		if (member == "GetAll") {
			DValue properties = DValue::Array("{sv}");
			properties.Add(dict_entry("version", DValue::UInt32(version)));
			result.push_back(properties);
		} else if (member == "Get" && arguments.size() == 2
			&& arguments[1].s == "version")
			result.push_back(DValue::Variant(DValue::UInt32(version)));
		else {
			Error(call, "org.freedesktop.DBus.Error.InvalidArgs",
				"No such property");
			return;
		}
		Reply(call, result);
		return;
	}

	if (interface == "org.freedesktop.portal.Settings") {
		static const char* const kKeys[] = { "color-scheme", "accent-color",
			"contrast", NULL };
		if ((member == "Read" || member == "ReadOne")
			&& arguments.size() == 2) {
			bool found = false;
			DValue value = arguments[0].s == "org.freedesktop.appearance"
				? appearance_value(arguments[1].s, &found) : DValue();
			if (!found) {
				Error(call, "org.freedesktop.portal.Error.NotFound",
					"Requested setting not found");
				return;
			}
			// Read() wraps the value in one more variant, as specified
			result.push_back(member == "Read"
				? DValue::Variant(DValue::Variant(value))
				: DValue::Variant(value));
		} else if (member == "ReadAll" && arguments.size() == 1) {
			bool wanted = arguments[0].items.empty();
			for (size_t i = 0; i < arguments[0].items.size(); i++) {
				BString pattern = arguments[0].items[i].s;
				if (pattern == "org.freedesktop.appearance"
					|| pattern == "org.freedesktop.*" || pattern == "*"
					|| pattern.IsEmpty())
					wanted = true;
			}
			DValue all = DValue::Array("{sa{sv}}");
			if (wanted) {
				DValue values = DValue::Array("{sv}");
				for (const char* const* key = kKeys; *key != NULL; key++) {
					bool found;
					DValue value = appearance_value(*key, &found);
					values.Add(dict_entry(*key, value));
				}
				all.Add(DValue::DictEntry(
					DValue::String("org.freedesktop.appearance"), values));
			}
			result.push_back(all);
		} else {
			Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
				"Unknown method");
			return;
		}
		Reply(call, result);
		return;
	}

	if (interface == "org.freedesktop.portal.OpenURI"
		&& (member == "OpenURI" || member == "OpenFile"
			|| member == "OpenDirectory")) {
		BString request;
		BString sender = call.sender;
		sender.RemoveFirst(":");
		sender.ReplaceAll(".", "_");
		BString token;
		const DValue* handleToken = arguments.size() >= 3
			? arguments[arguments.size() - 1].Find("handle_token") : NULL;
		if (handleToken != NULL)
			token = handleToken->Unwrapped().s;
		else
			token << "renga" << fNextRequest++;
		request << "/org/freedesktop/portal/desktop/request/" << sender
			<< "/" << token;

		status_t status = B_BAD_VALUE;
		if (member == "OpenURI" && arguments.size() == 3) {
			BString uri = arguments[1].s;
			if (uri.StartsWith("file://")) {
				BUrl url(uri.String(), true);
				entry_ref ref;
				if (get_ref_for_path(url.Path().String(), &ref) == B_OK)
					status = be_roster->Launch(&ref);
			} else {
				const char* argv[] = { uri.String(), NULL };
				BString scheme(uri, uri.FindFirst(':'));
				BString mime;
				mime << "application/x-vnd.Be.URL." << scheme;
				status = be_roster->Launch(mime.String(), 1, (char**)argv);
				if (status == B_ALREADY_RUNNING)
					status = B_OK;
			}
		} else if (arguments.size() >= 2 && call.fds.size() > 0) {
			// OpenFile/OpenDirectory with a descriptor: its path
			char path[B_PATH_NAME_LENGTH];
			BString link;
			link << "/dev/fd/" << call.fds[0];
			ssize_t length = readlink(link.String(), path, sizeof(path) - 1);
			if (length > 0) {
				path[length] = '\0';
				entry_ref ref;
				if (get_ref_for_path(path, &ref) == B_OK)
					status = be_roster->Launch(&ref);
			}
		}
		if (status == B_ALREADY_RUNNING)
			status = B_OK;

		result.push_back(DValue::ObjectPath(request));
		Reply(call, result);
		DValueList response;
		response.push_back(DValue::UInt32(status == B_OK ? 0 : 2));
		response.push_back(DValue::Array("{sv}"));
		EmitSignal("org.freedesktop.portal.Desktop", request.String(),
			"org.freedesktop.portal.Request", "Response", response);
		return;
	}

	Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
		"This portal is not offered yet");
}


// #pragma mark - Secret Service


static const char* kKeyring = "Secret Service";
static const char* kServicePath = "/org/freedesktop/secrets";
static const char* kCollectionPath
	= "/org/freedesktop/secrets/collection/login";
static const char* kItemPrefix = "/org/freedesktop/secrets/collection/login/";


SecretService::SecretService(SessionBus* bus)
	:
	Service(bus),
	fNextSession(1),
	fLock("secret service"),
	fQuit(false),
	fKeyringReady(false)
{
	fWakeUp = create_sem(0, "secret service");
	fThread = spawn_thread(_Worker, "secret service", B_NORMAL_PRIORITY,
		this);
	resume_thread(fThread);
}


SecretService::~SecretService()
{
	fQuit = true;
	release_sem(fWakeUp);
	status_t result;
	wait_for_thread(fThread, &result);
	delete_sem(fWakeUp);
}


void
SecretService::HandleCall(const Message& call)
{
	BAutolock locker(fLock);
	if (fQueue.size() >= 64) {
		// the key store does not answer: say so rather than pile up
		Error(call, "org.freedesktop.DBus.Error.LimitsExceeded",
			"The key store is busy");
		return;
	}
	fQueue.push_back(call);
	release_sem(fWakeUp);
}


/*static*/ status_t
SecretService::_Worker(void* data)
{
	SecretService* self = (SecretService*)data;
	while (!self->fQuit) {
		acquire_sem(self->fWakeUp);
		while (!self->fQuit) {
			Message call;
			{
				BAutolock locker(self->fLock);
				if (self->fQueue.empty())
					break;
				call = self->fQueue.front();
				self->fQueue.erase(self->fQueue.begin());
			}
			if (!self->fKeyringReady) {
				BKeyStore().AddKeyring(kKeyring);
					// B_NAME_IN_USE after the first time
				self->fKeyringReady = true;
			}
			self->_Handle(call);
		}
	}
	return B_OK;
}


const char* const*
SecretService::Names() const
{
	static const char* const kNames[] = { "org.freedesktop.secrets", NULL };
	return kNames;
}


struct secret_item {
	BString				id;
	BString				label;
	BString				contentType;
	std::vector<std::pair<BString, BString> > attributes;
	std::vector<uint8>	secret;
	bigtime_t			created;
};


static bool
item_from_key(const BKey& key, secret_item& item)
{
	BMessage archive;
	if (key.DataLength() == 0
		|| archive.Unflatten((const char*)key.Data()) != B_OK)
		return false;
	item.id = key.Identifier();
	item.label = archive.GetString("label", "");
	item.contentType = archive.GetString("content_type", "text/plain");
	item.created = archive.GetInt64("created", 0);
	const char* name;
	const char* value;
	for (int32 i = 0; archive.FindString("attribute_name", i, &name) == B_OK
			&& archive.FindString("attribute_value", i, &value) == B_OK; i++)
		item.attributes.push_back(std::make_pair(BString(name),
			BString(value)));
	const void* data;
	ssize_t size;
	if (archive.FindData("secret", B_RAW_TYPE, &data, &size) == B_OK)
		item.secret.assign((const uint8*)data, (const uint8*)data + size);
	return true;
}


static status_t
store_item(const secret_item& item)
{
	BMessage archive;
	archive.AddString("label", item.label);
	archive.AddString("content_type", item.contentType);
	archive.AddInt64("created", item.created);
	for (size_t i = 0; i < item.attributes.size(); i++) {
		archive.AddString("attribute_name", item.attributes[i].first);
		archive.AddString("attribute_value", item.attributes[i].second);
	}
	archive.AddData("secret", B_RAW_TYPE, item.secret.data(),
		item.secret.size());
	ssize_t size = archive.FlattenedSize();
	std::vector<char> buffer(size);
	status_t status = archive.Flatten(buffer.data(), size);
	if (status != B_OK)
		return status;
	BKeyStore store;
	BKey old;
	if (store.GetKey(kKeyring, B_KEY_TYPE_GENERIC, item.id.String(), old)
			== B_OK)
		store.RemoveKey(kKeyring, old);
	BKey key(B_KEY_PURPOSE_GENERIC, item.id.String(), "",
		(const uint8*)buffer.data(), size);
	return store.AddKey(kKeyring, key);
}


static void
all_items(std::vector<secret_item>& items)
{
	BKeyStore store;
	uint32 cookie = 0;
	BKey key;
	while (store.GetNextKey(kKeyring, cookie, key) == B_OK) {
		secret_item item;
		if (item_from_key(key, item))
			items.push_back(item);
	}
}


static bool
attributes_match(const secret_item& item, const DValue& wanted)
{
	for (size_t i = 0; i < wanted.items.size(); i++) {
		const BString& name = wanted.items[i].items[0].s;
		const BString& value = wanted.items[i].items[1].s;
		bool found = false;
		for (size_t a = 0; a < item.attributes.size() && !found; a++) {
			if (item.attributes[a].first == name
				&& item.attributes[a].second == value)
				found = true;
		}
		if (!found)
			return false;
	}
	return true;
}


static DValue
secret_struct(const char* session, const secret_item& item)
{
	DValue secret = DValue::Struct();
	secret.Add(DValue::ObjectPath(session));
	secret.Add(DValue::Array("y"));		// parameters: none for "plain"
	DValue value = DValue::Array("y");
	for (size_t i = 0; i < item.secret.size(); i++)
		value.Add(DValue::Byte(item.secret[i]));
	secret.Add(value);
	secret.Add(DValue::String(item.contentType));
	return secret;
}


static DValue
item_properties(const secret_item& item)
{
	DValue attributes = DValue::Array("{ss}");
	for (size_t i = 0; i < item.attributes.size(); i++) {
		attributes.Add(DValue::DictEntry(
			DValue::String(item.attributes[i].first),
			DValue::String(item.attributes[i].second)));
	}
	DValue properties = DValue::Array("{sv}");
	properties.Add(dict_entry("Label", DValue::String(item.label)));
	properties.Add(dict_entry("Attributes", attributes));
	properties.Add(dict_entry("Locked", DValue::Bool(false)));
	properties.Add(dict_entry("Created", DValue::UInt64(
		item.created / 1000000)));
	properties.Add(dict_entry("Modified", DValue::UInt64(
		item.created / 1000000)));
	return properties;
}


void
SecretService::_Handle(const Message& call)
{
	DValueList arguments;
	call.GetArguments(arguments);
	const BString& interface = call.interface;
	const BString& member = call.member;
	const BString& path = call.path;
	DValueList result;

	if (interface == "org.freedesktop.DBus.Introspectable") {
		result.push_back(DValue::String(introspection(
			" <interface name=\"org.freedesktop.Secret.Service\">\n"
			"  <method name=\"OpenSession\"><arg type=\"s\"/>"
			"<arg type=\"v\"/><arg type=\"v\" direction=\"out\"/>"
			"<arg type=\"o\" direction=\"out\"/></method>\n"
			"  <method name=\"SearchItems\"><arg type=\"a{ss}\"/>"
			"<arg type=\"ao\" direction=\"out\"/><arg type=\"ao\" "
			"direction=\"out\"/></method>\n"
			"  <property name=\"Collections\" type=\"ao\" "
			"access=\"read\"/>\n"
			" </interface>\n").String()));
		Reply(call, result);
		return;
	}

	std::vector<secret_item> items;
	all_items(items);

	if (path == kServicePath && interface == "org.freedesktop.Secret.Service") {
		if (member == "OpenSession" && arguments.size() == 2) {
			if (arguments[0].s != "plain") {
				Error(call, "org.freedesktop.DBus.Error.NotSupported",
					"Only the plain algorithm is offered (local socket)");
				return;
			}
			BString session;
			session << "/org/freedesktop/secrets/session/" << fNextSession++;
			result.push_back(DValue::Variant(DValue::String("")));
			result.push_back(DValue::ObjectPath(session));
		} else if (member == "SearchItems" && arguments.size() == 1) {
			DValue unlocked = DValue::Array("o");
			for (size_t i = 0; i < items.size(); i++) {
				if (attributes_match(items[i], arguments[0])) {
					unlocked.Add(DValue::ObjectPath(BString(kItemPrefix)
						<< items[i].id));
				}
			}
			result.push_back(unlocked);
			result.push_back(DValue::Array("o"));
		} else if ((member == "Unlock" || member == "Lock")
			&& arguments.size() == 1) {
			result.push_back(member == "Unlock" ? arguments[0]
				: DValue::Array("o"));
			result.push_back(DValue::ObjectPath("/"));
		} else if (member == "GetSecrets" && arguments.size() == 2) {
			DValue secrets = DValue::Array("{o(oayays)}");
			for (size_t o = 0; o < arguments[0].items.size(); o++) {
				const BString& object = arguments[0].items[o].s;
				for (size_t i = 0; i < items.size(); i++) {
					if (object == BString(kItemPrefix) << items[i].id) {
						secrets.Add(DValue::DictEntry(
							DValue::ObjectPath(object),
							secret_struct(arguments[1].s, items[i])));
					}
				}
			}
			result.push_back(secrets);
		} else if (member == "ReadAlias" && arguments.size() == 1) {
			result.push_back(DValue::ObjectPath(
				arguments[0].s == "default" || arguments[0].s == "login"
					? kCollectionPath : "/"));
		} else if (member == "SetAlias") {
			// one collection: nothing to change
		} else if (member == "CreateCollection") {
			result.push_back(DValue::ObjectPath(kCollectionPath));
			result.push_back(DValue::ObjectPath("/"));
		} else {
			Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
				"Unknown method");
			return;
		}
		Reply(call, result);
		return;
	}

	if (interface == kPropertiesInterface && arguments.size() >= 1) {
		DValue properties = DValue::Array("{sv}");
		if (path == kServicePath) {
			DValue collections = DValue::Array("o");
			collections.Add(DValue::ObjectPath(kCollectionPath));
			properties.Add(dict_entry("Collections", collections));
		} else if (path == kCollectionPath) {
			DValue list = DValue::Array("o");
			for (size_t i = 0; i < items.size(); i++)
				list.Add(DValue::ObjectPath(BString(kItemPrefix)
					<< items[i].id));
			properties.Add(dict_entry("Items", list));
			properties.Add(dict_entry("Label", DValue::String("Login")));
			properties.Add(dict_entry("Locked", DValue::Bool(false)));
			properties.Add(dict_entry("Created", DValue::UInt64(0)));
			properties.Add(dict_entry("Modified", DValue::UInt64(0)));
		} else if (path.StartsWith(kItemPrefix)) {
			BString id = path.String() + strlen(kItemPrefix);
			bool found = false;
			for (size_t i = 0; i < items.size() && !found; i++) {
				if (items[i].id == id) {
					properties = item_properties(items[i]);
					found = true;
				}
			}
			if (!found) {
				Error(call, "org.freedesktop.Secret.Error.NoSuchObject",
					"No such item");
				return;
			}
		}
		if (member == "GetAll")
			result.push_back(properties);
		else if (member == "Get" && arguments.size() == 2) {
			const DValue* value = properties.Find(arguments[1].s);
			if (value == NULL) {
				Error(call, "org.freedesktop.DBus.Error.InvalidArgs",
					"No such property");
				return;
			}
			result.push_back(*value);
		} else if (member == "Set") {
			// labels and attributes of items
			if (path.StartsWith(kItemPrefix) && arguments.size() == 3) {
				BString id = path.String() + strlen(kItemPrefix);
				for (size_t i = 0; i < items.size(); i++) {
					if (items[i].id != id)
						continue;
					const DValue& value = arguments[2].Unwrapped();
					if (arguments[1].s == "Label")
						items[i].label = value.s;
					else if (arguments[1].s == "Attributes") {
						items[i].attributes.clear();
						for (size_t a = 0; a < value.items.size(); a++) {
							items[i].attributes.push_back(std::make_pair(
								value.items[a].items[0].s,
								value.items[a].items[1].s));
						}
					}
					store_item(items[i]);
				}
			}
		}
		Reply(call, result);
		return;
	}

	if (path == kCollectionPath
		&& interface == "org.freedesktop.Secret.Collection") {
		if (member == "SearchItems" && arguments.size() == 1) {
			DValue found = DValue::Array("o");
			for (size_t i = 0; i < items.size(); i++) {
				if (attributes_match(items[i], arguments[0]))
					found.Add(DValue::ObjectPath(BString(kItemPrefix)
						<< items[i].id));
			}
			result.push_back(found);
		} else if (member == "CreateItem" && arguments.size() == 3) {
			const DValue& properties = arguments[0];
			const DValue& secret = arguments[1];
			bool replace = arguments[2].u != 0;
			secret_item item;
			const DValue* label = properties.Find(
				"org.freedesktop.Secret.Item.Label");
			if (label != NULL)
				item.label = label->Unwrapped().s;
			const DValue* attributes = properties.Find(
				"org.freedesktop.Secret.Item.Attributes");
			DValue wanted = DValue::Array("{ss}");
			if (attributes != NULL) {
				const DValue& list = attributes->Unwrapped();
				for (size_t a = 0; a < list.items.size(); a++) {
					item.attributes.push_back(std::make_pair(
						list.items[a].items[0].s, list.items[a].items[1].s));
					wanted.Add(list.items[a]);
				}
			}
			if (secret.items.size() == 4) {
				for (size_t b = 0; b < secret.items[2].items.size(); b++)
					item.secret.push_back(secret.items[2].items[b].u);
				item.contentType = secret.items[3].s;
			}
			item.created = real_time_clock_usecs();
			item.id = "";
			if (replace) {
				for (size_t i = 0; i < items.size(); i++) {
					if (items[i].attributes.size() == item.attributes.size()
						&& attributes_match(items[i], wanted)) {
						item.id = items[i].id;
						item.created = items[i].created;
						break;
					}
				}
			}
			if (item.id.IsEmpty())
				item.id.SetToFormat("i%" B_PRIx64, (uint64)system_time());
			if (store_item(item) != B_OK) {
				Error(call, "org.freedesktop.DBus.Error.Failed",
					"The key store did not take the secret");
				return;
			}
			result.push_back(DValue::ObjectPath(BString(kItemPrefix)
				<< item.id));
			result.push_back(DValue::ObjectPath("/"));
		} else if (member == "Delete") {
			result.push_back(DValue::ObjectPath("/"));
		} else {
			Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
				"Unknown method");
			return;
		}
		Reply(call, result);
		return;
	}

	if (path.StartsWith(kItemPrefix)
		&& interface == "org.freedesktop.Secret.Item") {
		BString id = path.String() + strlen(kItemPrefix);
		secret_item* item = NULL;
		for (size_t i = 0; i < items.size(); i++) {
			if (items[i].id == id)
				item = &items[i];
		}
		if (item == NULL) {
			Error(call, "org.freedesktop.Secret.Error.NoSuchObject",
				"No such item");
			return;
		}
		if (member == "GetSecret" && arguments.size() == 1)
			result.push_back(secret_struct(arguments[0].s, *item));
		else if (member == "SetSecret" && arguments.size() == 1
			&& arguments[0].items.size() == 4) {
			item->secret.clear();
			for (size_t b = 0; b < arguments[0].items[2].items.size(); b++)
				item->secret.push_back(arguments[0].items[2].items[b].u);
			item->contentType = arguments[0].items[3].s;
			store_item(*item);
		} else if (member == "Delete") {
			BKeyStore store;
			BKey key;
			if (store.GetKey(kKeyring, B_KEY_TYPE_GENERIC, id.String(), key)
					== B_OK)
				store.RemoveKey(kKeyring, key);
			result.push_back(DValue::ObjectPath("/"));
		} else {
			Error(call, "org.freedesktop.DBus.Error.UnknownMethod",
				"Unknown method");
			return;
		}
		Reply(call, result);
		return;
	}

	if (path.StartsWith("/org/freedesktop/secrets/session/")
		&& member == "Close") {
		Reply(call, result);
		return;
	}

	Error(call, "org.freedesktop.DBus.Error.UnknownObject", "No such object");
}


}	// namespace DBus
