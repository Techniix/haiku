/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * liblinuxkpi_manage: everything LinuxKPI Manager and lkpictl know about
 * LinuxKPI drivers. The GUI and the CLI only call this library; this library
 * is the only code that talks to the kernel, the file system and the logs.
 *
 * Sources of information (docs/MANAGER.md, "Data sources"):
 *	driver manifests	data/linuxkpi/drivers/<name>	(what a driver is)
 *	coverage		data/linuxkpi/coverage		(API status)
 *	diagnostics rules	data/linuxkpi/diagnostics	(known messages)
 *	device manager		generic syscall, as listdev	(hardware present)
 *	file system		add-ons/kernel/drivers/{bin,dev}	(installed/enabled)
 *	network roster		BNetworkRoster			(interfaces)
 *	running driver		LKPI_CTL_* socket ioctls	(read-only status)
 *	syslog			/var/log/syslog			(messages)
 *	settings		~/config/settings/kernel/drivers/<name>
 */
#ifndef LINUXKPI_MANAGE_H
#define LINUXKPI_MANAGE_H


#include <ObjectList.h>
#include <String.h>
#include <StringList.h>
#include <SupportDefs.h>


namespace LKPI {


enum FeatureState {
	kStateWorks = 0,
	kStatePartial,
	kStateExperimental,
	kStateUntested,
	kStateMissing,
	kStateUnknown
};

enum CoverageState {
	kCoverageImplemented = 0,
	kCoveragePartial,
	kCoverageStub,
	kCoverageMissing,
	kCoverageUnknown
};

enum Severity {
	kSeverityInfo = 0,
	kSeverityWarning,
	kSeverityError
};

enum TunableRisk {
	kRiskSafe = 0,
	kRiskExperimental,
	kRiskUnsafe			// never offered for editing
};


const char*	FeatureStateName(FeatureState state);
const char*	CoverageStateName(CoverageState state);
const char*	SeverityName(Severity severity);


struct DeviceId {
	uint16			vendor;
	uint16			device;
	BString			name;
	BStringList		firmware;
};

struct Feature {
	BString			id;
	BString			label;
	FeatureState	state;
};

struct TunableSpec {
	BString			key;
	BString			type;		// "bool" or "int"
	BString			label;
	BString			description;
	TunableRisk		risk;
	int64			minimum;
	int64			maximum;
	int64			defaultValue;

	bool			Validate(const char* value, BString& error) const;
};


class Driver {
public:
	// manifest
	BString			name;
	BString			description;
	BString			version;
	BString			origin;
	BString			license;
	BString			driverClass;
	BString			publish;		// e.g. "net/mt7921e"
	BString			firmwareDir;
	BStringList		dependencies;
	BObjectList<DeviceId, true>		devices;
	BObjectList<Feature, true>		features;
	BObjectList<TunableSpec, true>	tunables;

	// install state (DriverCatalog::Refresh())
	BString			binaryPath;		// empty: not installed
	BString			linkPath;		// boot-time link, if any
	bool			nonPackaged;
	bool			enabledAtBoot;	// dev/<publish> link exists
	bool			published;		// /dev/<publish> exists now
	BStringList		interfaces;		// "/dev/net/mt7921e/0", ...

	Driver();

	bool			Installed() const { return !binaryPath.IsEmpty(); }
	const DeviceId*	Matches(uint16 vendor, uint16 device) const;
	const TunableSpec* FindTunable(const char* key) const;
	// Firmware files listed for "id" that are not installed.
	status_t		MissingFirmware(const DeviceId& id,
						BStringList& missing) const;
};


class DriverCatalog {
public:
					DriverCatalog();

	// Loads every manifest and the install state of each driver.
	status_t		Load();
	status_t		RefreshState();

	int32			CountDrivers() const { return fDrivers.CountItems(); }
	Driver*			DriverAt(int32 index) const
						{ return fDrivers.ItemAt(index); }
	Driver*			FindDriver(const char* name) const;
	Driver*			FindDriverFor(uint16 vendor, uint16 device,
						const DeviceId** id = NULL) const;

	// Boot-time enable/disable. Only for non-packaged drivers: the link in
	// non-packaged add-ons/kernel/drivers/dev/<publish>. Takes effect at the
	// next restart (docs/MANAGER.md explains why not immediately).
	status_t		SetEnabledAtBoot(Driver* driver, bool enabled,
						BString& error);

	// Directories that were searched, for reports.
	const BStringList& ManifestDirectories() const { return fManifestDirs; }

private:
	status_t		_LoadManifest(const char* path);
	void			_RefreshDriver(Driver* driver);

	BObjectList<Driver, true>	fDrivers;
	BStringList					fManifestDirs;
};


struct PCIDevice {
	uint16			vendor;
	uint16			device;
	// Subsystem IDs are not published by Haiku's PCI bus manager as
	// device-manager attributes (see listdev.cpp), so they are not here.
	uint8			classBase;
	uint8			classSub;
	uint8			classInterface;
};

// Walks the device manager tree (same syscalls as listdev).
status_t			ScanPCIDevices(BObjectList<PCIDevice, true>& devices);
const char*			PCIClassName(uint8 base, uint8 sub);


// Read-only status from a running driver (headers/private/linuxkpi).
class KernelQuery {
public:
	// Interfaces of the network roster whose device name starts with
	// "/dev/<publish>/".
	static status_t	InterfacesFor(const Driver& driver, BStringList& out);

	// op: LKPI_CTL_GET_INFO, _STATS or _TUNABLES. On success "reply" holds
	// "key=value" lines.
	static status_t	Query(const char* interface, uint32 op, BString& reply);
	static status_t	QueryValues(const char* interface, uint32 op,
						BStringList& keys, BStringList& values);
};


struct Coverage {
	BString			group;
	BString			name;
	CoverageState	state;
	BString			note;
};

status_t			LoadCoverage(BObjectList<Coverage, true>& out,
						BString& version);


struct LogLine {
	BString			text;
	BString			driver;		// "[mt7921e]" tag, if any
	Severity		severity;
	int32			rule;		// index into rules, -1 if none
};

struct DiagnosticRule {
	BString			match;
	Severity		severity;
	BString			title;
	BString			advice;
};

class LogReader {
public:
					LogReader();

	status_t		LoadRules();
	// Reads the syslog (and syslog.old when "includeOld") and keeps the lines
	// that belong to LinuxKPI drivers known to "catalog".
	status_t		Read(const DriverCatalog& catalog, bool includeOld = false);

	int32			CountLines() const { return fLines.CountItems(); }
	const LogLine*	LineAt(int32 index) const { return fLines.ItemAt(index); }
	int32			CountRules() const { return fRules.CountItems(); }
	const DiagnosticRule* RuleAt(int32 index) const
						{ return fRules.ItemAt(index); }

	static bool		IsLinuxKPILine(const char* line,
						const DriverCatalog& catalog, BString* driver);

private:
	void			_Classify(LogLine* line);

	BObjectList<LogLine, true>			fLines;
	BObjectList<DiagnosticRule, true>	fRules;
};


// The driver's settings file: ~/config/settings/kernel/drivers/<driver>.
// Only keys described by the driver's manifest can be written, and only with
// values the manifest's TunableSpec accepts.
class SettingsStore {
public:
	static status_t	PathFor(const Driver& driver, BString& path);
	static status_t	Read(const Driver& driver, BStringList& keys,
						BStringList& values);
	static status_t	Set(const Driver& driver, const char* key,
						const char* value, BString& error);
	static status_t	Unset(const Driver& driver, const char* key,
						BString& error);
};


// Plain-text diagnostic report (for attaching to bug reports).
status_t			BuildReport(DriverCatalog& catalog, BString& report);


}	// namespace LKPI


#endif	// LINUXKPI_MANAGE_H
