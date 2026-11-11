/*
 * Copyright 2016-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "debug.h"

#include <string.h>

#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>
#include <util/ring_buffer.h>

#include "efi_platform.h"
#include "mmu.h"
#include "serial.h"


static char sBuffer[16384];
static uint32 sBufferPosition;

static ring_buffer* sDebugSyslogBuffer = NULL;
static bool sPostCleanup = false;

// Renga (F-51): the debug syslog at a fixed physical address, as the BIOS
// loader has it (bios_ia32/debug.cpp), so that the next boot finds the
// previous session's syslog (with the kernel debugger's crash report) again
// after a warm reboot. The pages are claimed with AllocatePages
// (AllocateAddress) as early as possible; if the firmware uses them, the
// buffer is allocated anywhere, as before, and is not kept.
static const char* const kDebugSyslogSignature = "Haiku syslog";
static const efi_physical_addr kPersistentSyslogBase = 63 * 1024 * 1024;
static const size_t kPersistentSyslogSize = 1024 * 1024;
static void* sPersistentSyslog = NULL;


static void
syslog_write(const char* buffer, size_t length)
{
	if (sPostCleanup && sDebugSyslogBuffer != NULL) {
		ring_buffer_write(sDebugSyslogBuffer, (const uint8*)buffer, length);
	} else if (sBufferPosition + length < sizeof(sBuffer)) {
		memcpy(sBuffer + sBufferPosition, buffer, length);
		sBufferPosition += length;
	}
}


static void
dprintf_args(const char *format, va_list args)
{
	char buffer[512];
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	if (length == 0)
		return;

	if (length >= (int)sizeof(buffer))
		length = sizeof(buffer) - 1;

	syslog_write(buffer, length);
	serial_puts(buffer, length);
}


extern "C" void
dprintf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	dprintf_args(format, args);
	va_end(args);
}


extern "C" void
panic(const char *format, ...)
{
	va_list args;

	platform_switch_to_text_mode();

	puts("*** PANIC ***");

	va_start(args, format);
	vprintf(format, args);
	va_end(args);

	while (true)
		kBootServices->Stall(1000000);
}


static void
allocate_ring_buffer(void)
{
	// allocate 1 MB memory
	void* buffer = NULL;
	size_t size = 1024 * 1024;

	if (platform_allocate_region(&buffer, size, 0) != B_OK)
		return;

	sDebugSyslogBuffer = create_ring_buffer_etc(buffer, size, 0);

	gKernelArgs.debug_output = sDebugSyslogBuffer;
	gKernelArgs.debug_size = sDebugSyslogBuffer->size;
}


void
debug_init_persistent_syslog(void)
{
	efi_physical_addr address = kPersistentSyslogBase;
	size_t pages = kPersistentSyslogSize / B_PAGE_SIZE;
	if (kBootServices->AllocatePages(AllocateAddress, EfiLoaderData, pages,
			&address) != EFI_SUCCESS) {
		dprintf("debug syslog: the pages at %#" B_PRIx64 " are in use, the "
			"syslog will not be kept across reboots\n",
			(uint64)kPersistentSyslogBase);
		return;
	}

	sPersistentSyslog = (void*)address;
	void* buffer = sPersistentSyslog;
	size_t size = kPersistentSyslogSize;

	// check whether there's a previous syslog we can recover
	size_t signatureLength = strlen(kDebugSyslogSignature);
	bool recover = memcmp(buffer, kDebugSyslogSignature, signatureLength) == 0;

	size -= signatureLength;
	buffer = (uint8*)buffer + ROUNDUP(signatureLength, sizeof(void*));

	sDebugSyslogBuffer = create_ring_buffer_etc(buffer, size,
		recover ? RING_BUFFER_INIT_FROM_BUFFER : 0);

	gKernelArgs.debug_output = sDebugSyslogBuffer;
	gKernelArgs.debug_size = sDebugSyslogBuffer->size;
	dprintf("debug syslog: kept at %#" B_PRIx64 "%s\n",
		(uint64)kPersistentSyslogBase,
		recover ? ", with the previous session's syslog" : "");
}


void
debug_cleanup(void)
{
	if (sPersistentSyslog != NULL) {
		if (platform_register_allocated_region((phys_addr_t)sPersistentSyslog,
				kPersistentSyslogSize) != B_OK) {
			sDebugSyslogBuffer = NULL;
			gKernelArgs.debug_output = NULL;
		}
	} else
		allocate_ring_buffer();

	if (sDebugSyslogBuffer != NULL) {
		// If desired, store the debug syslog data from the previous session
		// for the kernel (the same as the BIOS loader does).
		size_t bytesReadable = 0;
		if (sPersistentSyslog != NULL && gKernelArgs.previous_debug_size != 0)
			bytesReadable = ring_buffer_readable(sDebugSyslogBuffer);
		gKernelArgs.previous_debug_size = bytesReadable;

		if (bytesReadable != 0) {
			if (uint8* buffer = (uint8*)kernel_args_malloc(bytesReadable)) {
				ring_buffer_read(sDebugSyslogBuffer, buffer, bytesReadable);
				gKernelArgs.previous_debug_output = buffer;
			} else
				gKernelArgs.previous_debug_size = 0;
		}

		if (gKernelArgs.keep_debug_output_buffer) {
			// copy the output gathered so far into the ring buffer
			ring_buffer_clear(sDebugSyslogBuffer);
			ring_buffer_write(sDebugSyslogBuffer, (uint8*)sBuffer,
				sBufferPosition);
			if (sPersistentSyslog != NULL) {
				memcpy(sPersistentSyslog, kDebugSyslogSignature,
					strlen(kDebugSyslogSignature));
			}
		} else if (sPersistentSyslog != NULL) {
			// clear the signature
			memset(sPersistentSyslog, 0, strlen(kDebugSyslogSignature));
		}
	} else {
		gKernelArgs.keep_debug_output_buffer = false;
	}

	if (!gKernelArgs.keep_debug_output_buffer) {
		gKernelArgs.debug_output = kernel_args_malloc(sBufferPosition);
		if (gKernelArgs.debug_output != NULL) {
			memcpy(gKernelArgs.debug_output, sBuffer, sBufferPosition);
			gKernelArgs.debug_size = sBufferPosition;
		}
	}

	sPostCleanup = true;
}


char*
platform_debug_get_log_buffer(size_t *_size)
{
	if (_size != NULL)
		*_size = sizeof(sBuffer);

	return sBuffer;
}
