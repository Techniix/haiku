/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _CRASH_REPORT_FORMAT_H
#define _CRASH_REPORT_FORMAT_H


/*!	The crash reports of roadmap F-51, shared by the kernel debugger (which
	writes them into the syslog buffer, src/system/kernel/debug/
	crash_report.cpp), crash_collector (which finds them in the previous
	session's syslog and stores them) and the System logs application.

	In the syslog a report is a block of lines

		==== Renga crash report begin ====
		format: 1
		reason: panic | double fault | user request | keyboard | debugger
		message: <the debugger's entry message, one line>
		revision: <hrev>
		time: <seconds since 1970, the kernel's real time clock>
		uptime_us: ..., cpu_count: ..., cpu_model: ..., memory_bytes: ...,
		free_memory_bytes: ..., debugger_cpu: ...
		--- cpu <n>: thread <id> "<name>"
		<the debugger's "bt" output>
		...
		--- loaded kernel images
		<the debugger's "image" output>
		==== Renga crash report end ====

	Kernel crash files are /boot/system/var/log/crash/
	kernel-<YYYY-MM-DD_HH-MM-SS>.txt; application crash files (debug_server)
	app-<name>-<YYYY-MM-DD_HH-MM-SS>.txt. The collector marks a kernel crash
	file it has not shown yet with the attribute CRASH_REPORT_NEW_ATTRIBUTE;
	the notification removes it.
*/


#define CRASH_REPORT_BEGIN			"==== Renga crash report begin ===="
#define CRASH_REPORT_END			"==== Renga crash report end ===="
#define CRASH_REPORT_CPU			"--- cpu"
#define CRASH_REPORT_IMAGES			"--- loaded kernel images"
#define CRASH_REPORT_FORMAT_VERSION	1

#define CRASH_REPORT_REASON_PANIC			"panic"
#define CRASH_REPORT_REASON_DOUBLE_FAULT	"double fault"
#define CRASH_REPORT_REASON_USER			"user request"
#define CRASH_REPORT_REASON_KEYBOARD		"keyboard"
#define CRASH_REPORT_REASON_DEBUGGER		"debugger"

#define CRASH_REPORT_DIRECTORY		"/boot/system/var/log/crash"
#define CRASH_REPORT_PREVIOUS_SYSLOG	"/boot/system/var/log/previous_syslog"
#define CRASH_REPORT_NEW_ATTRIBUTE	"Renga:crash-report-new"
#define CRASH_REPORT_SYSLOG_TAIL	(64 * 1024)
	// how much of the syslog before the report the collector keeps


#endif	// _CRASH_REPORT_FORMAT_H
