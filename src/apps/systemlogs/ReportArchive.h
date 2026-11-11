/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef REPORT_ARCHIVE_H
#define REPORT_ARCHIVE_H


#include <String.h>


/*!	Writes the "Save a report" archive: a ZIP file (deflate, zlib) with the
	syslog (and its rotated copy), the kernel's previous-session syslog, every
	crash report, and the output of listdev, listusb, sysinfo and uname.
	\a path is the archive to write; \a _summary gets what went into it.
*/
status_t save_report_archive(const char* path, BString* _summary);

/*!	The default place: ~/Desktop/system-report-<date>.zip. */
BString default_report_path();


#endif	// REPORT_ARCHIVE_H
