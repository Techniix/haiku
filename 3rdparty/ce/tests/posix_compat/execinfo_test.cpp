/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for backtrace_symbols() image lookup and offsets
	(Chromium request R-3).

	The program is linked with "-z separate-code" and a non-zero image base
	("-Ttext-segment"), so that its layout is that of an lld-linked image (a
	read-only segment with the ELF headers before the executable one) and
	its program headers' virtual addresses do not start at 0. glibc prints
	a frame without a symbol as "image(+0xOFF)" with OFF = address - load
	bias (l_addr), which is the ELF virtual address addr2line and
	llvm-symbolizer expect; the test reads the program's own file from disk
	to compute the expected OFF independently of libgnu.

	Checks:
	- layout: the binary really has two read-only PT_LOADs and p_vaddr != 0;
	- static_frame: a frame in a static function names the program and its
	  OFF is the function's ELF virtual address (plus the frame offset);
	- exported_frame: an exported function is named "exported_frame+0x...";
	- data_address: an address in the program's data segment is attributed
	  to the program with the right OFF (it printed as a bare "[0x...]"
	  before, as did anything outside the first read-only region);
	- outside: an address in no image prints as "[0x...]".
	Output format: see 3rdparty/ce/README.md.
*/


#include <execinfo.h>

#include <OS.h>
#include <image.h>

#include <elf.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "execinfo"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void* sFrames[16];
static int sFrameCount;
int gDataWord = 42;
	// in .data, the writable segment


extern "C" __attribute__((noinline, visibility("default"))) void
exported_frame()
{
	sFrameCount = backtrace(sFrames, 16);
	__asm__ volatile("" ::: "memory");
}


static __attribute__((noinline)) void
static_frame()
{
	exported_frame();
	__asm__ volatile("" ::: "memory");
}


/*!	Reads the program's own ELF file and returns the p_vaddr of the PT_LOAD
	with file offset 0, and the number of non-writable PT_LOADs.
*/
static bool
read_layout(const char* path, uint64_t& _baseVaddr, int& _readOnlyLoads)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	Elf64_Ehdr header;
	bool ok = read(fd, &header, sizeof(header)) == sizeof(header)
		&& memcmp(header.e_ident, ELFMAG, SELFMAG) == 0;
	_readOnlyLoads = 0;
	bool haveBase = false;
	for (int i = 0; ok && i < header.e_phnum; i++) {
		Elf64_Phdr phdr;
		if (pread(fd, &phdr, sizeof(phdr),
				header.e_phoff + i * header.e_phentsize) != sizeof(phdr)) {
			ok = false;
			break;
		}
		if (phdr.p_type != PT_LOAD)
			continue;
		if ((phdr.p_flags & PF_WRITE) == 0)
			_readOnlyLoads++;
		if (phdr.p_offset == 0) {
			_baseVaddr = phdr.p_vaddr;
			haveBase = true;
		}
	}
	close(fd);
	return ok && haveBase;
}


static bool
parse_offset(const char* line, const char* image, uintptr_t& _offset)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "%s(+0x", image);
	const char* found = strstr(line, pattern);
	if (found == NULL)
		return false;
	_offset = strtoul(found + strlen(pattern), NULL, 16);
	return true;
}


int
main()
{
	image_info info;
	int32 cookie = 0;
	bool haveImage = false;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE) {
			haveImage = true;
			break;
		}
	}
	uint64_t baseVaddr = 0;
	int readOnlyLoads = 0;
	bool haveLayout = haveImage
		&& read_layout(info.name, baseVaddr, readOnlyLoads);
	report("layout", haveLayout && readOnlyLoads >= 2 && baseVaddr != 0,
		"read-only PT_LOADs %d, first p_vaddr 0x%llx", readOnlyLoads,
		(unsigned long long)baseVaddr);

	// The load bias: where the offset-0 segment was mapped, minus its
	// virtual address. image_info.text is the lowest read-only region,
	// which is that segment.
	uintptr_t bias = (uintptr_t)info.text - (uintptr_t)baseVaddr;

	static_frame();
	char** symbols = backtrace_symbols(sFrames, sFrameCount);

	// Frame 0 is in exported_frame(), frame 1 in static_frame().
	bool exportedOk = symbols != NULL && sFrameCount >= 2
		&& strstr(symbols[0], "execinfo_test(exported_frame+0x") != NULL;
	report("exported_frame", exportedOk, "%s",
		symbols != NULL && sFrameCount > 0 ? symbols[0] : "-");

	uintptr_t offset = 0;
	uintptr_t expected = sFrameCount >= 2
		? (uintptr_t)sFrames[1] - bias : 0;
	bool staticOk = symbols != NULL && sFrameCount >= 2
		&& parse_offset(symbols[1], "execinfo_test", offset)
		&& offset == expected;
	report("static_frame", staticOk,
		"%s; expected +0x%lx (ELF address, static_frame at 0x%lx)",
		symbols != NULL && sFrameCount > 1 ? symbols[1] : "-",
		(unsigned long)expected,
		(unsigned long)((uintptr_t)&static_frame - bias));
	for (int i = 0; symbols != NULL && i < sFrameCount; i++)
		printf("  frame %d: %s\n", i, symbols[i]);
	free(symbols);

	// An address in the data segment (backtrace_symbols() takes any
	// address; it looks up address - 1, as for a return address).
	void* dataAddress = (char*)&gDataWord + 1;
	char** data = backtrace_symbols(&dataAddress, 1);
	expected = (uintptr_t)dataAddress - bias;
	bool dataOk = data != NULL
		&& parse_offset(data[0], "execinfo_test", offset)
		&& offset == expected;
	report("data_address", dataOk, "%s; expected +0x%lx",
		data != NULL ? data[0] : "-", (unsigned long)expected);
	free(data);

	// An address in no image.
	void* nowhere = (void*)(uintptr_t)0x1000;
	char** outside = backtrace_symbols(&nowhere, 1);
	report("outside", outside != NULL && outside[0][0] == '[', "%s",
		outside != NULL ? outside[0] : "-");
	free(outside);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
