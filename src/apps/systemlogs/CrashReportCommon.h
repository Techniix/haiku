/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef CRASH_REPORT_COMMON_H
#define CRASH_REPORT_COMMON_H


/*!	What System logs (SystemLogs.cpp) and crash_report_notifier
	(CrashReportNotifier.cpp) share: reading a crash report and its fields.
*/


#include <stdio.h>
#include <sys/types.h>

#include <string>


static const char* kNotifierSignature
	= "application/x-vnd.Renga-CrashReportNotifier";


static inline bool
read_file(const char* path, std::string& contents, off_t from = 0)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	if (from > 0)
		fseeko(file, from, SEEK_SET);
	char buffer[65536];
	size_t bytesRead;
	while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0)
		contents.append(buffer, bytesRead);
	fclose(file);
	return true;
}


static inline std::string
report_field(const std::string& text, const char* name)
{
	std::string key = std::string("\n") + name + ": ";
	size_t start = text.find(key);
	if (start == std::string::npos)
		return "";
	start += key.size();
	size_t end = text.find('\n', start);
	return text.substr(start, end == std::string::npos ? std::string::npos
		: end - start);
}


#endif	// CRASH_REPORT_COMMON_H
