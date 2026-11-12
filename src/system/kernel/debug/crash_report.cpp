/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Crash reports (roadmap F-51).

	When the kernel debugger is entered (a panic, a double fault, the
	emergency key or a kernel_debugger() call), a short report is appended to
	the syslog ring buffer: the reason and message, the revision, the CPUs and
	memory, a backtrace of the thread each CPU was running, and the list of
	loaded kernel images.

	Nothing is written to a disk here. The syslog buffer is the boot loader's
	"debug syslog" (KDEBUG_ENABLE_DEBUG_SYSLOG): a ring buffer at a fixed
	physical address that the next boot loader finds again by its signature
	after a warm reboot and hands to the kernel, which saves it as
	/var/log/previous_syslog. This is the design of Linux' ramoops (pstore
	in reserved RAM): the debugger only writes to memory, with interrupts off
	and no locks or allocations, so writing the report cannot hang the
	debugger or damage a file system. The cost is that the report survives a
	warm reboot (the debugger's "reboot" command, the reset button on most
	machines) but not a power cycle. crash_collector turns the report into a
	file in /boot/system/var/log/crash on the next boot.

	The report goes to the syslog buffer only, not to the serial port or the
	screen, through a debug output filter, so the screen keeps showing the
	entry message and the stack trace and does not page through the report.
*/


#include "crash_report.h"

#include <stdio.h>
#include <string.h>

#include <new>

#include <OS.h>

#include <cpu.h>
#include <debug.h>
#include <ksystem_info.h>
#include <smp.h>
#include <thread.h>
#include <vm/vm_page.h>

#include <crash_report_format.h>

#include "debug_commands.h"
#include "debug_output_filter.h"


namespace {


class SyslogOnlyOutputFilter : public DebugOutputFilter {
public:
	virtual void PrintString(const char* string)
	{
		debug_crash_report_syslog_write(string, strlen(string));
	}

	virtual void Print(const char* format, va_list args)
	{
		vsnprintf(fBuffer, sizeof(fBuffer), format, args);
		PrintString(fBuffer);
	}

private:
	char	fBuffer[1024];
};


}	// namespace


// Constructed in place on first use: the kernel does not run the
// constructors of every global object before the debugger may be entered.
static union {
	char	buffer[sizeof(SyslogOnlyOutputFilter)];
	void*	alignment;
} sSyslogOnlyFilterStorage;
static SyslogOnlyOutputFilter* sSyslogOnlyFilter = NULL;
static char sMessageBuffer[512];

// legacy_drivers.cpp: the driver whose init or uninit hook is running, if
// any (Renga: the report names the add-on of a panic in a driver's load or
// unload, e.g. a reload by the kernel daemon during boot).
extern "C" bool legacy_driver_current_operation(const char** _operation,
	const char** _path);
static char sCommandBuffer[64];
static bool sWriting = false;


/*!	Runs a debugger command with its output going to the current filter
	(the syslog only). evaluate_debug_command() would send it to the
	default filter: the serial port and the screen, where the blue screen's
	paging would wait for a key.
*/
static void
run_command(const char* name, const char* argument)
{
	bool ambiguous;
	debugger_command* command = find_debugger_command(name, false, ambiguous);
	if (command == NULL) {
		kprintf("(no debugger command \"%s\")\n", name);
		return;
	}
	strlcpy(sCommandBuffer, argument != NULL ? argument : "",
		sizeof(sCommandBuffer));
	char* argv[3] = { (char*)name, sCommandBuffer, NULL };
	invoke_debugger_command(command, argument != NULL ? 2 : 1, argv);
}


static const char*
crash_reason(const char* messagePrefix, const char* message)
{
	if (messagePrefix != NULL && strstr(messagePrefix, "PANIC") != NULL)
		return CRASH_REPORT_REASON_PANIC;
	if (messagePrefix != NULL && strstr(messagePrefix, "Double Fault") != NULL)
		return CRASH_REPORT_REASON_DOUBLE_FAULT;
	// kernel_debugger() passes its message as the prefix
	const char* text = messagePrefix != NULL ? messagePrefix : message;
	if (text != NULL && strncmp(text, "USER: ", 6) == 0)
		return CRASH_REPORT_REASON_USER;
	if (text != NULL && strstr(text, "Keyboard Requested Halt") != NULL)
		return CRASH_REPORT_REASON_KEYBOARD;
	return CRASH_REPORT_REASON_DEBUGGER;
}


void
crash_report_write(const char* messagePrefix, const char* message,
	va_list args, int32 cpu)
{
	// A hand-over to another CPU enters the loop again without a message;
	// that is the same entry. A fault while writing the report re-enters
	// the debugger: do not write a second report from inside the first.
	if ((messagePrefix == NULL && message == NULL) || sWriting
		|| !debug_crash_report_syslog_available()) {
		return;
	}
	sWriting = true;

	// The message, without the commands a panic() message may carry after
	// "@!".
	sMessageBuffer[0] = '\0';
	if (message != NULL) {
		va_list copy;
		va_copy(copy, args);
		vsnprintf(sMessageBuffer, sizeof(sMessageBuffer), message, copy);
		va_end(copy);
		if (char* commands = strstr(sMessageBuffer, "@!"))
			*commands = '\0';
	}
	// One line: the collector reads the message from a single line.
	for (char* c = sMessageBuffer; *c != '\0'; c++) {
		if (*c == '\n' || *c == '\r')
			*c = ' ';
	}

	if (sSyslogOnlyFilter == NULL) {
		sSyslogOnlyFilter = new(&sSyslogOnlyFilterStorage)
			SyslogOnlyOutputFilter;
	}
	DebugOutputFilter* previousFilter
		= set_debug_output_filter(sSyslogOnlyFilter);

	kprintf("\n%s\n", CRASH_REPORT_BEGIN);
	kprintf("format: %d\n", CRASH_REPORT_FORMAT_VERSION);
	kprintf("reason: %s\n", crash_reason(messagePrefix, message));
	kprintf("message: %s%s\n", messagePrefix != NULL ? messagePrefix : "",
		sMessageBuffer);
	kprintf("revision: %s\n", get_haiku_revision());
	kprintf("time: %lu\n", real_time_clock());
	kprintf("uptime_us: %" B_PRId64 "\n", system_time());
	kprintf("cpu_count: %" B_PRId32 "\n", smp_get_num_cpus());
#if defined(__i386__) || defined(__x86_64__)
	kprintf("cpu_model: %.48s\n", gCPU[0].arch.model_name);
#endif
	kprintf("memory_bytes: %" B_PRIu64 "\n",
		(uint64)vm_page_num_pages() * B_PAGE_SIZE);
	kprintf("free_memory_bytes: %" B_PRIu64 "\n",
		(uint64)vm_page_num_free_pages() * B_PAGE_SIZE);
	kprintf("debugger_cpu: %" B_PRId32 "\n", cpu);
	const char* operation;
	const char* driverPath;
	if (legacy_driver_current_operation(&operation, &driverPath)
		&& debug_is_kernel_memory_accessible((addr_t)driverPath, 1,
			B_KERNEL_READ_AREA)) {
		kprintf("%s %.256s (%s)\n", CRASH_REPORT_DRIVER, driverPath,
			operation);
	}

	// The thread each CPU was running. The other CPUs are held in
	// debug_trap_cpu_in_kdl(), which saved their registers, so "bt <id>"
	// traces them from there.
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		Thread* thread = gCPU[i].running_thread;
		if (thread == NULL || !debug_is_kernel_memory_accessible(
				(addr_t)thread, sizeof(Thread), B_KERNEL_READ_AREA)) {
			kprintf("%s %" B_PRId32 ": no thread\n", CRASH_REPORT_CPU, i);
			continue;
		}
		kprintf("%s %" B_PRId32 ": thread %" B_PRId32 " \"%.64s\"\n",
			CRASH_REPORT_CPU, i, thread->id, thread->name);
		if (i == cpu)
			run_command("bt", NULL);
		else {
			char id[16];
			snprintf(id, sizeof(id), "%" B_PRId32, thread->id);
			run_command("bt", id);
		}
	}

	kprintf("%s\n", CRASH_REPORT_IMAGES);
	run_command("image", NULL);

	kprintf("%s\n", CRASH_REPORT_END);

	set_debug_output_filter(previousFilter);
	kprintf("A crash report was kept in memory for the next boot "
		"(after a reboot, not a power-off).\n");

	sWriting = false;
}
