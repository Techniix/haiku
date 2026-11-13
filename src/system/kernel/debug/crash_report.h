/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_DEBUG_CRASH_REPORT_H
#define _KERNEL_DEBUG_CRASH_REPORT_H


#include <stdarg.h>

#include <SupportDefs.h>


// The crash report the kernel debugger writes into the syslog buffer when
// it is entered (roadmap F-51). The format is shared with the userland
// collector (headers/private/shared/crash_report_format.h).


// Implemented in debug.cpp: appends text to the syslog ring buffer only
// (not to the serial port or the screen), without locking. Kernel debugger
// context only.
void debug_crash_report_syslog_write(const char* text, int32 length);
bool debug_crash_report_syslog_available();

// Implemented in crash_report.cpp. Called by kernel_debugger_loop() once per
// entry, after the entry message and the stack trace were printed. Returns
// whether a report was written (F-59: the debugger then says how to keep it).
bool crash_report_write(const char* messagePrefix, const char* message,
	va_list args, int32 cpu);


#endif	// _KERNEL_DEBUG_CRASH_REPORT_H
