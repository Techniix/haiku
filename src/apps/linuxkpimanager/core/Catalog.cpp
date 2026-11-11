/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Driver manifests and install state.
 */


#include "LinuxKPIManage.h"

#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <SymLink.h>
#include <driver_settings.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


namespace LKPI {


static const char* kStateNames[] = {
	"works", "partial", "experimental", "untested", "missing", "unknown"
};
static const char* kCoverageNames[] = {
	"implemented", "partial", "stub", "missing", "unknown"
};
static const char* kSeverityNames[] = { "info", "warning", "error" };


const char*
FeatureStateName(FeatureState state)
{
	return kStateNames[state <= kStateUnknown ? state : kStateUnknown];
}


const char*
CoverageStateName(CoverageState state)
{
	return kCoverageNames[state <= kCoverageUnknown ? state : kCoverageUnknown];
}


const char*
SeverityName(Severity severity)
{
	return kSeverityNames[severity <= kSeverityError ? severity
		: kSeverityError];
}


static FeatureState
parse_feature_state(const char* s)
{
	for (int32 i = 0; i < kStateUnknown; i++) {
		if (strcmp(s, kStateNames[i]) == 0)
			return (FeatureState)i;
	}
	return kStateUnknown;
}


// #pragma mark - manifest helpers


static const char*
param_value(const driver_parameter& parent, const char* name,
	int index = 0)
{
	for (int i = 0; i < parent.parameter_count; i++) {
		const driver_parameter& p = parent.parameters[i];
		if (strcmp(p.name, name) == 0 && p.value_count > index)
			return p.values[index];
	}
	return NULL;
}


static bool
parse_int64(const char* s, int64& out)
{
	if (s == NULL || *s == '\0')
		return false;
	char* end;
	errno = 0;
	long long v = strtoll(s, &end, 0);
	if (errno != 0 || *end != '\0')
		return false;
	out = v;
	return true;
}


static bool
parse_hex16(const char* s, uint16& out)
{
	if (s == NULL)
		return false;
	char* end;
	unsigned long v = strtoul(s, &end, 16);
	if (*end != '\0' || v > 0xffff)
		return false;
	out = (uint16)v;
	return true;
}


// #pragma mark - TunableSpec


bool
TunableSpec::Validate(const char* value, BString& error) const
{
	if (risk == kRiskUnsafe) {
		error = "This setting cannot be changed from here.";
		return false;
	}
	if (type == "bool") {
		static const char* kOk[] = { "0", "1", "true", "false", "yes", "no",
			"on", "off", NULL };
		for (int i = 0; kOk[i] != NULL; i++) {
			if (strcmp(value, kOk[i]) == 0)
				return true;
		}
		error = "Expected on or off (1 or 0).";
		return false;
	}
	if (type == "int") {
		int64 v;
		if (!parse_int64(value, v)) {
			error = "Expected a whole number.";
			return false;
		}
		if (v < minimum || v > maximum) {
			error.SetToFormat("Expected a number from %" B_PRId64 " to %"
				B_PRId64 ".", minimum, maximum);
			return false;
		}
		return true;
	}
	error = "Unknown setting type in the driver manifest.";
	return false;
}


// #pragma mark - Driver


Driver::Driver()
	:
	nonPackaged(false),
	enabledAtBoot(false),
	published(false)
{
}


const DeviceId*
Driver::Matches(uint16 vendor, uint16 device) const
{
	for (int32 i = 0; i < devices.CountItems(); i++) {
		const DeviceId* id = devices.ItemAt(i);
		if (id->vendor == vendor && id->device == device)
			return id;
	}
	return NULL;
}


const TunableSpec*
Driver::FindTunable(const char* key) const
{
	for (int32 i = 0; i < tunables.CountItems(); i++) {
		if (tunables.ItemAt(i)->key == key)
			return tunables.ItemAt(i);
	}
	return NULL;
}


status_t
Driver::MissingFirmware(const DeviceId& id, BStringList& missing) const
{
	static const directory_which kDirs[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, B_SYSTEM_DATA_DIRECTORY
	};
	missing.MakeEmpty();
	for (int32 i = 0; i < id.firmware.CountStrings(); i++) {
		bool found = false;
		for (size_t d = 0; d < B_COUNT_OF(kDirs) && !found; d++) {
			BPath path;
			if (find_directory(kDirs[d], &path) != B_OK)
				continue;
			path.Append("firmware");
			path.Append(firmwareDir.String());
			path.Append(id.firmware.StringAt(i).String());
			found = BEntry(path.Path()).Exists();
		}
		if (!found)
			missing.Add(id.firmware.StringAt(i));
	}
	return B_OK;
}


// #pragma mark - DriverCatalog


DriverCatalog::DriverCatalog()
{
}


status_t
DriverCatalog::Load()
{
	static const directory_which kDirs[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	fDrivers.MakeEmpty();
	fManifestDirs.MakeEmpty();
	for (size_t i = 0; i < B_COUNT_OF(kDirs); i++) {
		BPath path;
		if (find_directory(kDirs[i], &path) != B_OK)
			continue;
		path.Append("linuxkpi/drivers");
		fManifestDirs.Add(path.Path());

		BDirectory dir(path.Path());
		if (dir.InitCheck() != B_OK)
			continue;
		BEntry entry;
		while (dir.GetNextEntry(&entry) == B_OK) {
			BPath file;
			if (entry.GetPath(&file) != B_OK || !entry.IsFile())
				continue;
			// first directory wins: user overrides system
			if (FindDriver(file.Leaf()) != NULL)
				continue;
			_LoadManifest(file.Path());
		}
	}
	return RefreshState();
}


status_t
DriverCatalog::_LoadManifest(const char* path)
{
	void* handle = load_driver_settings(path);
	if (handle == NULL)
		return B_ERROR;
	const driver_settings* settings = get_driver_settings(handle);
	if (settings == NULL) {
		unload_driver_settings(handle);
		return B_ERROR;
	}

	driver_parameter root = {};
	root.parameter_count = settings->parameter_count;
	root.parameters = settings->parameters;

	Driver* driver = new Driver;
	const char* v;
	driver->name = (v = param_value(root, "name")) ? v : BPath(path).Leaf();
	if ((v = param_value(root, "description")) != NULL)
		driver->description = v;
	if ((v = param_value(root, "version")) != NULL)
		driver->version = v;
	if ((v = param_value(root, "origin")) != NULL)
		driver->origin = v;
	if ((v = param_value(root, "license")) != NULL)
		driver->license = v;
	if ((v = param_value(root, "class")) != NULL)
		driver->driverClass = v;
	if ((v = param_value(root, "publish")) != NULL)
		driver->publish = v;
	driver->firmwareDir = (v = param_value(root, "firmware_dir"))
		? v : driver->name.String();

	for (int i = 0; i < root.parameter_count; i++) {
		const driver_parameter& p = root.parameters[i];
		if (strcmp(p.name, "requires") == 0) {
			for (int j = 0; j < p.value_count; j++)
				driver->dependencies.Add(p.values[j]);
		} else if (strcmp(p.name, "device") == 0 && p.value_count >= 2) {
			DeviceId* id = new DeviceId;
			if (!parse_hex16(p.values[0], id->vendor)
				|| !parse_hex16(p.values[1], id->device)) {
				delete id;
				continue;
			}
			if (p.value_count >= 3)
				id->name = p.values[2];
			for (int k = 0; k < p.parameter_count; k++) {
				const driver_parameter& f = p.parameters[k];
				if (strcmp(f.name, "firmware") != 0)
					continue;
				for (int j = 0; j < f.value_count; j++)
					id->firmware.Add(f.values[j]);
			}
			driver->devices.AddItem(id);
		} else if (strcmp(p.name, "feature") == 0 && p.value_count >= 2) {
			Feature* feature = new Feature;
			feature->id = p.values[0];
			feature->state = parse_feature_state(p.values[1]);
			feature->label = p.value_count >= 3 ? p.values[2] : p.values[0];
			driver->features.AddItem(feature);
		} else if (strcmp(p.name, "tunable") == 0 && p.value_count >= 1) {
			TunableSpec* t = new TunableSpec;
			t->key = p.values[0];
			t->type = (v = param_value(p, "type")) ? v : "int";
			t->label = (v = param_value(p, "label")) ? v : p.values[0];
			if ((v = param_value(p, "description")) != NULL)
				t->description = v;
			v = param_value(p, "risk");
			t->risk = v == NULL || strcmp(v, "unsafe") == 0 ? kRiskUnsafe
				: strcmp(v, "safe") == 0 ? kRiskSafe : kRiskExperimental;
			if (!parse_int64(param_value(p, "min"), t->minimum))
				t->minimum = t->type == "bool" ? 0 : INT32_MIN;
			if (!parse_int64(param_value(p, "max"), t->maximum))
				t->maximum = t->type == "bool" ? 1 : INT32_MAX;
			if (!parse_int64(param_value(p, "default"), t->defaultValue))
				t->defaultValue = 0;
			driver->tunables.AddItem(t);
		}
	}
	unload_driver_settings(handle);

	fDrivers.AddItem(driver);
	return B_OK;
}


status_t
DriverCatalog::RefreshState()
{
	for (int32 i = 0; i < fDrivers.CountItems(); i++)
		_RefreshDriver(fDrivers.ItemAt(i));
	return B_OK;
}


void
DriverCatalog::_RefreshDriver(Driver* driver)
{
	struct Place {
		directory_which	which;
		bool			nonPackaged;
	};
	static const Place kPlaces[] = {
		{ B_USER_NONPACKAGED_ADDONS_DIRECTORY, true },
		{ B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY, true },
		{ B_SYSTEM_ADDONS_DIRECTORY, false }
	};

	driver->binaryPath = "";
	driver->linkPath = "";
	driver->nonPackaged = false;
	driver->enabledAtBoot = false;

	for (size_t i = 0; i < B_COUNT_OF(kPlaces); i++) {
		BPath base;
		if (find_directory(kPlaces[i].which, &base) != B_OK)
			continue;
		BPath binary(base.Path(), "kernel/drivers/bin");
		binary.Append(driver->name.String());
		if (!BEntry(binary.Path()).Exists())
			continue;

		driver->binaryPath = binary.Path();
		driver->nonPackaged = kPlaces[i].nonPackaged;
		if (!driver->publish.IsEmpty()) {
			BPath link(base.Path(), "kernel/drivers/dev");
			link.Append(driver->publish.String());
			driver->linkPath = link.Path();
		}
		break;
	}

	// A boot link in any of the directories publishes the driver, not only
	// the one next to the binary we found.
	for (size_t i = 0; i < B_COUNT_OF(kPlaces) && !driver->publish.IsEmpty();
			i++) {
		BPath base;
		if (find_directory(kPlaces[i].which, &base) != B_OK)
			continue;
		BPath link(base.Path(), "kernel/drivers/dev");
		link.Append(driver->publish.String());
		struct stat st;
		if (lstat(link.Path(), &st) == 0) {
			driver->enabledAtBoot = true;
			break;
		}
	}

	driver->published = false;
	if (!driver->publish.IsEmpty()) {
		BString dev("/dev/");
		dev << driver->publish;
		driver->published = BEntry(dev.String()).Exists();
	}
	KernelQuery::InterfacesFor(*driver, driver->interfaces);
}


Driver*
DriverCatalog::FindDriver(const char* name) const
{
	for (int32 i = 0; i < fDrivers.CountItems(); i++) {
		if (fDrivers.ItemAt(i)->name == name)
			return fDrivers.ItemAt(i);
	}
	return NULL;
}


Driver*
DriverCatalog::FindDriverFor(uint16 vendor, uint16 device,
	const DeviceId** _id) const
{
	for (int32 i = 0; i < fDrivers.CountItems(); i++) {
		const DeviceId* id = fDrivers.ItemAt(i)->Matches(vendor, device);
		if (id != NULL) {
			if (_id != NULL)
				*_id = id;
			return fDrivers.ItemAt(i);
		}
	}
	return NULL;
}


status_t
DriverCatalog::SetEnabledAtBoot(Driver* driver, bool enabled, BString& error)
{
	if (driver == NULL || !driver->Installed()) {
		error = "The driver is not installed.";
		return B_BAD_VALUE;
	}
	if (!driver->nonPackaged) {
		// A packaged driver's link lives in a read-only package; blocking it
		// needs the package system's BlockedEntries (docs/MANAGER.md).
		error = "This driver comes from a package and cannot be switched "
			"off here.";
		return B_NOT_ALLOWED;
	}
	if (driver->linkPath.IsEmpty()) {
		error = "The driver manifest does not say where it publishes.";
		return B_BAD_VALUE;
	}

	// Only ever touch the link we computed ourselves, inside the add-ons
	// dev directory, and only if it is a symlink (or absent).
	struct stat st;
	bool exists = lstat(driver->linkPath.String(), &st) == 0;
	if (exists && !S_ISLNK(st.st_mode)) {
		error.SetToFormat("%s is not a link; leaving it alone.",
			driver->linkPath.String());
		return B_NOT_ALLOWED;
	}

	if (!enabled) {
		if (exists && unlink(driver->linkPath.String()) != 0) {
			error.SetToFormat("Could not remove %s: %s",
				driver->linkPath.String(), strerror(errno));
			return errno;
		}
	} else if (!exists) {
		BPath linkPath(driver->linkPath.String());
		BPath parent;
		linkPath.GetParent(&parent);
		create_directory(parent.Path(), 0755);

		// relative target, as the testing guide does it:
		// dev/net/<name> -> ../../bin/<name>
		BString target;
		int32 depth = 1;
		for (int32 i = 0; i < driver->publish.Length(); i++) {
			if (driver->publish[i] == '/')
				depth++;
		}
		for (int32 i = 0; i < depth; i++)
			target << "../";
		target << "bin/" << driver->name;
		if (symlink(target.String(), driver->linkPath.String()) != 0) {
			error.SetToFormat("Could not create %s: %s",
				driver->linkPath.String(), strerror(errno));
			return errno;
		}
	}
	sync();
	_RefreshDriver(driver);
	return B_OK;
}


}	// namespace LKPI
