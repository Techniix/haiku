/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Syslog reading and classification against data/linuxkpi/diagnostics.
 */


#include "LinuxKPIManage.h"

#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <driver_settings.h>

#include <string.h>
#include <strings.h>


namespace LKPI {


static const off_t kMaxLogBytes = 8 * 1024 * 1024;


LogReader::LogReader()
{
}


status_t
LogReader::LoadRules()
{
	static const directory_which kDirs[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	fRules.MakeEmpty();
	for (size_t i = 0; i < B_COUNT_OF(kDirs); i++) {
		BPath path;
		if (find_directory(kDirs[i], &path) != B_OK)
			continue;
		path.Append("linuxkpi/diagnostics");
		void* handle = load_driver_settings(path.Path());
		if (handle == NULL)
			continue;
		const driver_settings* settings = get_driver_settings(handle);
		for (int p = 0; settings != NULL && p < settings->parameter_count;
				p++) {
			const driver_parameter& rule = settings->parameters[p];
			if (strcmp(rule.name, "rule") != 0)
				continue;
			DiagnosticRule* r = new DiagnosticRule;
			r->severity = kSeverityInfo;
			for (int k = 0; k < rule.parameter_count; k++) {
				const driver_parameter& f = rule.parameters[k];
				if (f.value_count < 1)
					continue;
				if (strcmp(f.name, "match") == 0)
					r->match = f.values[0];
				else if (strcmp(f.name, "title") == 0)
					r->title = f.values[0];
				else if (strcmp(f.name, "advice") == 0)
					r->advice = f.values[0];
				else if (strcmp(f.name, "severity") == 0) {
					r->severity = strcmp(f.values[0], "error") == 0
						? kSeverityError
						: strcmp(f.values[0], "warning") == 0
							? kSeverityWarning : kSeverityInfo;
				}
			}
			if (r->match.IsEmpty())
				delete r;
			else
				fRules.AddItem(r);
		}
		unload_driver_settings(handle);
		break;		// first file found wins, like the manifests
	}
	return B_OK;
}


bool
LogReader::IsLinuxKPILine(const char* line, const DriverCatalog& catalog,
	BString* _driver)
{
	// freebsd_network prefixes driver messages with "[<driver>]"
	// (device.c, driver_vprintf); LinuxKPI's own lines say linuxkpi/lkpi.
	for (int32 i = 0; i < catalog.CountDrivers(); i++) {
		BString tag("[");
		tag << catalog.DriverAt(i)->name << "]";
		if (strstr(line, tag.String()) != NULL) {
			if (_driver != NULL)
				*_driver = catalog.DriverAt(i)->name;
			return true;
		}
	}
	if (strcasestr(line, "linuxkpi") != NULL
		|| strstr(line, "lkpi") != NULL) {
		if (_driver != NULL)
			*_driver = "";
		return true;
	}
	return false;
}


void
LogReader::_Classify(LogLine* line)
{
	line->severity = kSeverityInfo;
	line->rule = -1;
	for (int32 i = 0; i < fRules.CountItems(); i++) {
		if (strcasestr(line->text.String(), fRules.ItemAt(i)->match.String())
				!= NULL) {
			line->rule = i;
			line->severity = fRules.ItemAt(i)->severity;
			return;
		}
	}
}


status_t
LogReader::Read(const DriverCatalog& catalog, bool includeOld)
{
	fLines.MakeEmpty();

	BPath logDir;
	status_t status = find_directory(B_SYSTEM_LOG_DIRECTORY, &logDir);
	if (status != B_OK)
		return status;

	const char* names[] = { "syslog.old", "syslog" };
	for (size_t n = includeOld ? 0 : 1; n < B_COUNT_OF(names); n++) {
		BPath path(logDir.Path(), names[n]);
		BFile file(path.Path(), B_READ_ONLY);
		if (file.InitCheck() != B_OK)
			continue;

		off_t size;
		if (file.GetSize(&size) != B_OK)
			continue;
		off_t start = size > kMaxLogBytes ? size - kMaxLogBytes : 0;

		BString text;
		char* buffer = text.LockBuffer(size - start + 1);
		ssize_t bytes = file.ReadAt(start, buffer, size - start);
		text.UnlockBuffer(bytes > 0 ? bytes : 0);

		BStringList lines;
		text.Split("\n", true, lines);
		for (int32 i = 0; i < lines.CountStrings(); i++) {
			BString driver;
			if (!IsLinuxKPILine(lines.StringAt(i).String(), catalog,
					&driver))
				continue;
			LogLine* line = new LogLine;
			line->text = lines.StringAt(i);
			line->driver = driver;
			_Classify(line);
			fLines.AddItem(line);
		}
	}
	return B_OK;
}


}	// namespace LKPI
