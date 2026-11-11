/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * API coverage data and the plain-text diagnostic report.
 */


#include "LinuxKPIManage.h"

#include <FindDirectory.h>
#include <OS.h>
#include <Path.h>
#include <driver_settings.h>

#include <string.h>
#include <sys/utsname.h>

#include <lkpi_control.h>


namespace LKPI {


status_t
LoadCoverage(BObjectList<Coverage, true>& out, BString& version)
{
	static const directory_which kDirs[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	out.MakeEmpty();
	version = "";
	for (size_t i = 0; i < B_COUNT_OF(kDirs); i++) {
		BPath path;
		if (find_directory(kDirs[i], &path) != B_OK)
			continue;
		path.Append("linuxkpi/coverage");
		void* handle = load_driver_settings(path.Path());
		if (handle == NULL)
			continue;
		const driver_settings* settings = get_driver_settings(handle);
		for (int p = 0; settings != NULL && p < settings->parameter_count;
				p++) {
			const driver_parameter& param = settings->parameters[p];
			if (strcmp(param.name, "version") == 0 && param.value_count > 0) {
				version = param.values[0];
				continue;
			}
			if (strcmp(param.name, "area") != 0 || param.value_count < 3)
				continue;
			Coverage* c = new Coverage;
			c->group = param.values[0];
			c->name = param.values[1];
			c->state = kCoverageUnknown;
			for (int32 s = 0; s < kCoverageUnknown; s++) {
				if (strcmp(param.values[2],
						CoverageStateName((CoverageState)s)) == 0)
					c->state = (CoverageState)s;
			}
			if (param.value_count > 3)
				c->note = param.values[3];
			out.AddItem(c);
		}
		unload_driver_settings(handle);
		return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static void
section(BString& report, const char* title)
{
	report << "\n== " << title << " ==\n";
}


status_t
BuildReport(DriverCatalog& catalog, BString& report)
{
	report = "LinuxKPI diagnostic report\n";

	struct utsname uts;
	if (uname(&uts) == 0) {
		report << "System: " << uts.sysname << " " << uts.release << " "
			<< uts.version << " " << uts.machine << "\n";
	}
	system_info info;
	if (get_system_info(&info) == B_OK) {
		report << "CPUs: " << info.cpu_count << ", memory: "
			<< (info.max_pages * B_PAGE_SIZE / (1024 * 1024)) << " MiB\n";
	}

	catalog.RefreshState();

	section(report, "Drivers");
	for (int32 i = 0; i < catalog.CountDrivers(); i++) {
		Driver* d = catalog.DriverAt(i);
		report << d->name << " " << d->version << ": "
			<< (d->Installed() ? d->binaryPath.String() : "not installed")
			<< (d->nonPackaged ? " (non-packaged)" : "")
			<< ", enabled at boot: " << (d->enabledAtBoot ? "yes" : "no")
			<< ", published: " << (d->published ? "yes" : "no") << "\n";
		for (int32 n = 0; n < d->interfaces.CountStrings(); n++) {
			const char* name = d->interfaces.StringAt(n).String();
			report << "  interface " << name << "\n";
			static const uint32 kOps[] = { LKPI_CTL_GET_INFO,
				LKPI_CTL_GET_STATS, LKPI_CTL_GET_TUNABLES };
			for (size_t o = 0; o < B_COUNT_OF(kOps); o++) {
				BString reply;
				status_t status = KernelQuery::Query(name, kOps[o], reply);
				if (status != B_OK) {
					report << "    (query failed: " << strerror(status)
						<< ")\n";
					break;
				}
				BStringList lines;
				reply.Split("\n", true, lines);
				for (int32 l = 0; l < lines.CountStrings(); l++)
					report << "    " << lines.StringAt(l) << "\n";
			}
		}
		BStringList keys, values;
		if (SettingsStore::Read(*d, keys, values) == B_OK) {
			for (int32 k = 0; k < keys.CountStrings(); k++) {
				report << "  setting " << keys.StringAt(k) << " = "
					<< values.StringAt(k) << "\n";
			}
		}
	}

	section(report, "Matching hardware");
	BObjectList<PCIDevice, true> devices;
	if (ScanPCIDevices(devices) == B_OK) {
		for (int32 i = 0; i < devices.CountItems(); i++) {
			const PCIDevice* dev = devices.ItemAt(i);
			const DeviceId* id = NULL;
			Driver* d = catalog.FindDriverFor(dev->vendor, dev->device, &id);
			if (d == NULL)
				continue;
			BString line;
			line.SetToFormat("%04x:%04x %s -> %s", dev->vendor, dev->device,
				id->name.String(), d->name.String());
			report << line << "\n";
			BStringList missing;
			d->MissingFirmware(*id, missing);
			for (int32 m = 0; m < missing.CountStrings(); m++)
				report << "  missing firmware: " << missing.StringAt(m) << "\n";
		}
	} else
		report << "(device manager not available)\n";

	section(report, "Log (LinuxKPI lines, newest last)");
	LogReader logs;
	logs.LoadRules();
	logs.Read(catalog, true);
	int32 first = logs.CountLines() > 400 ? logs.CountLines() - 400 : 0;
	for (int32 i = first; i < logs.CountLines(); i++)
		report << logs.LineAt(i)->text << "\n";

	return B_OK;
}


}	// namespace LKPI
