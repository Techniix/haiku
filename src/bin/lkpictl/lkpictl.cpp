/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * lkpictl: command-line front end of liblinuxkpi_manage, the same library
 * LinuxKPI Manager uses. Useful over ssh, in scripts and for bug reports.
 */


#include <stdio.h>
#include <string.h>

#include <File.h>

#include <lkpi_control.h>

#include "LinuxKPIManage.h"


using namespace LKPI;


static void
usage()
{
	fprintf(stderr,
		"usage: lkpictl <command> [arguments]\n"
		"  drivers                    list LinuxKPI drivers and their state\n"
		"  hardware                   list PCI devices a LinuxKPI driver supports\n"
		"  status <driver>            ask the running driver for its status\n"
		"  log [-a] [driver]          LinuxKPI log lines (-a: include syslog.old)\n"
		"  diagnose                   explain known problems in the log\n"
		"  coverage                   LinuxKPI API coverage\n"
		"  settings <driver>          list settings and their values\n"
		"  set <driver> <key> <value> change a setting (applies at next load)\n"
		"  unset <driver> <key>       return a setting to its default\n"
		"  enable|disable <driver>    load the driver at boot, or not\n"
		"  report [file]              write a diagnostic report\n");
}


static Driver*
need_driver(DriverCatalog& catalog, const char* name)
{
	Driver* driver = catalog.FindDriver(name);
	if (driver == NULL)
		fprintf(stderr, "lkpictl: no LinuxKPI driver named \"%s\"\n", name);
	return driver;
}


static int
cmd_drivers(DriverCatalog& catalog)
{
	printf("%-12s %-14s %-10s %-8s %-9s %s\n", "DRIVER", "VERSION",
		"INSTALLED", "AT BOOT", "RUNNING", "DESCRIPTION");
	for (int32 i = 0; i < catalog.CountDrivers(); i++) {
		Driver* d = catalog.DriverAt(i);
		printf("%-12s %-14s %-10s %-8s %-9s %s\n", d->name.String(),
			d->version.String(),
			!d->Installed() ? "no" : d->nonPackaged ? "yes" : "package",
			d->enabledAtBoot ? "yes" : "no",
			!d->interfaces.IsEmpty() ? "yes" : d->published ? "published"
				: "no",
			d->description.String());
	}
	return 0;
}


static int
cmd_hardware(DriverCatalog& catalog)
{
	BObjectList<PCIDevice, true> devices;
	status_t status = ScanPCIDevices(devices);
	if (status != B_OK) {
		fprintf(stderr, "lkpictl: device manager: %s\n", strerror(status));
		return 1;
	}
	int found = 0;
	for (int32 i = 0; i < devices.CountItems(); i++) {
		const PCIDevice* dev = devices.ItemAt(i);
		const DeviceId* id = NULL;
		Driver* d = catalog.FindDriverFor(dev->vendor, dev->device, &id);
		if (d == NULL)
			continue;
		found++;
		printf("%04x:%04x  %-28s %s  driver %s%s\n", dev->vendor, dev->device,
			id->name.String(), PCIClassName(dev->classBase, dev->classSub),
			d->name.String(), d->Installed() ? "" : " (not installed)");
		BStringList missing;
		d->MissingFirmware(*id, missing);
		for (int32 m = 0; m < missing.CountStrings(); m++)
			printf("           missing firmware: %s\n",
				missing.StringAt(m).String());
	}
	if (found == 0)
		printf("No device supported by a LinuxKPI driver was found.\n");
	return 0;
}


static int
cmd_status(DriverCatalog& catalog, const char* name)
{
	Driver* d = need_driver(catalog, name);
	if (d == NULL)
		return 1;
	if (d->interfaces.IsEmpty()) {
		printf("%s has no network interface right now, so it cannot be "
			"asked.\nCheck \"lkpictl log %s\".\n", name, name);
		return 1;
	}
	static const uint32 kOps[] = { LKPI_CTL_GET_INFO, LKPI_CTL_GET_STATS,
		LKPI_CTL_GET_TUNABLES };
	for (int32 n = 0; n < d->interfaces.CountStrings(); n++) {
		const char* iface = d->interfaces.StringAt(n).String();
		printf("%s:\n", iface);
		for (size_t o = 0; o < B_COUNT_OF(kOps); o++) {
			BString reply;
			status_t status = KernelQuery::Query(iface, kOps[o], reply);
			if (status == B_NOT_SUPPORTED) {
				printf("  this build of %s has no status interface\n", name);
				break;
			}
			if (status != B_OK) {
				printf("  query failed: %s\n", strerror(status));
				break;
			}
			BStringList lines;
			reply.Split("\n", true, lines);
			for (int32 l = 0; l < lines.CountStrings(); l++)
				printf("  %s\n", lines.StringAt(l).String());
		}
	}
	return 0;
}


static int
cmd_log(DriverCatalog& catalog, bool all, const char* driver, bool diagnose)
{
	LogReader logs;
	logs.LoadRules();
	logs.Read(catalog, all);
	int shown = 0;
	for (int32 i = 0; i < logs.CountLines(); i++) {
		const LogLine* line = logs.LineAt(i);
		// untagged lines ("linuxkpi: ...") belong to every driver
		if (driver != NULL && !line->driver.IsEmpty()
			&& line->driver != driver)
			continue;
		if (!diagnose) {
			printf("%s\n", line->text.String());
			continue;
		}
		if (line->rule < 0 || line->severity == kSeverityInfo)
			continue;
		const DiagnosticRule* rule = logs.RuleAt(line->rule);
		printf("%-7s %s\n        %s\n", SeverityName(line->severity),
			rule->title.String(), line->text.String());
		if (!rule->advice.IsEmpty())
			printf("        -> %s\n", rule->advice.String());
		shown++;
	}
	if (diagnose && shown == 0)
		printf("No known problems found in the log.\n");
	return 0;
}


static int
cmd_coverage()
{
	BObjectList<Coverage, true> areas;
	BString version;
	if (LoadCoverage(areas, version) != B_OK) {
		fprintf(stderr, "lkpictl: coverage data not installed\n");
		return 1;
	}
	int counts[kCoverageUnknown + 1] = {};
	printf("LinuxKPI %s\n", version.String());
	for (int32 i = 0; i < areas.CountItems(); i++) {
		const Coverage* c = areas.ItemAt(i);
		counts[c->state]++;
		printf("%-7s %-40s %-12s %s\n", c->group.String(), c->name.String(),
			CoverageStateName(c->state), c->note.String());
	}
	printf("\n%d implemented, %d partial, %d stub, %d missing\n",
		counts[kCoverageImplemented], counts[kCoveragePartial],
		counts[kCoverageStub], counts[kCoverageMissing]);
	return 0;
}


static int
cmd_settings(DriverCatalog& catalog, const char* name)
{
	Driver* d = need_driver(catalog, name);
	if (d == NULL)
		return 1;
	BStringList keys, values;
	SettingsStore::Read(*d, keys, values);
	BString path;
	SettingsStore::PathFor(*d, path);
	printf("File: %s\n", path.String());
	for (int32 i = 0; i < d->tunables.CountItems(); i++) {
		const TunableSpec* t = d->tunables.ItemAt(i);
		int32 index = keys.IndexOf(t->key);
		printf("%-44s %-8s (default %" B_PRId64 ", %s)  %s\n",
			t->key.String(),
			index >= 0 ? values.StringAt(index).String() : "-",
			t->defaultValue,
			t->risk == kRiskSafe ? "safe" : t->risk == kRiskExperimental
				? "experimental" : "locked",
			t->label.String());
	}
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}

	DriverCatalog catalog;
	catalog.Load();
	if (catalog.CountDrivers() == 0 && strcmp(argv[1], "coverage") != 0)
		fprintf(stderr, "lkpictl: no driver manifests found in %s\n",
			catalog.ManifestDirectories().Join(", ").String());

	const char* cmd = argv[1];
	if (strcmp(cmd, "drivers") == 0)
		return cmd_drivers(catalog);
	if (strcmp(cmd, "hardware") == 0)
		return cmd_hardware(catalog);
	if (strcmp(cmd, "status") == 0 && argc == 3)
		return cmd_status(catalog, argv[2]);
	if (strcmp(cmd, "log") == 0) {
		bool all = argc > 2 && strcmp(argv[2], "-a") == 0;
		const char* driver = argc > (all ? 3 : 2) ? argv[all ? 3 : 2] : NULL;
		return cmd_log(catalog, all, driver, false);
	}
	if (strcmp(cmd, "diagnose") == 0)
		return cmd_log(catalog, true, NULL, true);
	if (strcmp(cmd, "coverage") == 0)
		return cmd_coverage();
	if (strcmp(cmd, "settings") == 0 && argc == 3)
		return cmd_settings(catalog, argv[2]);
	if ((strcmp(cmd, "set") == 0 && argc == 5)
		|| (strcmp(cmd, "unset") == 0 && argc == 4)) {
		Driver* d = need_driver(catalog, argv[2]);
		if (d == NULL)
			return 1;
		BString error;
		status_t status = cmd[0] == 's'
			? SettingsStore::Set(*d, argv[3], argv[4], error)
			: SettingsStore::Unset(*d, argv[3], error);
		if (status != B_OK) {
			fprintf(stderr, "lkpictl: %s\n", error.String());
			return 1;
		}
		printf("Saved. %s reads it the next time it loads (restart).\n",
			d->name.String());
		return 0;
	}
	if ((strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0)
		&& argc == 3) {
		Driver* d = need_driver(catalog, argv[2]);
		if (d == NULL)
			return 1;
		BString error;
		if (catalog.SetEnabledAtBoot(d, cmd[0] == 'e', error) != B_OK) {
			fprintf(stderr, "lkpictl: %s\n", error.String());
			return 1;
		}
		printf("%s will %sload at the next restart.\n", d->name.String(),
			cmd[0] == 'e' ? "" : "not ");
		return 0;
	}
	if (strcmp(cmd, "report") == 0) {
		BString report;
		BuildReport(catalog, report);
		if (argc < 3) {
			fputs(report.String(), stdout);
			return 0;
		}
		BFile file(argv[2], B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (file.InitCheck() != B_OK
			|| file.Write(report.String(), report.Length())
				!= report.Length()) {
			fprintf(stderr, "lkpictl: cannot write %s\n", argv[2]);
			return 1;
		}
		printf("Report written to %s. Check it before sharing: it contains "
			"log lines,\nwhich can include network names and addresses.\n",
			argv[2]);
		return 0;
	}

	usage();
	return 1;
}
