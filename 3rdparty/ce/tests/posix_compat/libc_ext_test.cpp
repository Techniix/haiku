/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for the libc additions: getrandom(), getauxval(),
	backtrace*(), sendmmsg()/recvmmsg() and copy_file_range().
	Output format: see 3rdparty/ce/README.md.
*/


#include <execinfo.h>
#include <sys/auxv.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define SUITE "libc_ext"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
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


static bool
all_zero(const uint8_t* data, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (data[i] != 0)
			return false;
	}
	return true;
}


static void
test_getrandom()
{
	uint8_t first[64], second[64];
	ssize_t a = getrandom(first, sizeof(first), 0);
	ssize_t b = getrandom(second, sizeof(second), GRND_NONBLOCK);
	report("getrandom", a == 64 && b == 64 && !all_zero(first, 64)
			&& memcmp(first, second, 64) != 0,
		"returned %zd and %zd, distinct %d", a, b,
		memcmp(first, second, 64) != 0);

	// Larger than getentropy()'s 256-byte limit.
	const size_t kLarge = 1024 * 1024;
	uint8_t* large = (uint8_t*)malloc(kLarge);
	memset(large, 0, kLarge);
	ssize_t c = getrandom(large, kLarge, 0);
	// A 4 KiB run of zeroes would mean a chunk was not filled.
	bool filled = c == (ssize_t)kLarge;
	for (size_t i = 0; filled && i + 4096 <= kLarge; i += 4096)
		filled = !all_zero(large + i, 4096);
	free(large);
	report("getrandom_large", filled, "1 MiB: returned %zd, filled %d", c,
		filled);

	errno = 0;
	ssize_t bad = getrandom(first, 8, 0x80);
	int badErrno = errno;
	errno = 0;
	ssize_t both = getrandom(first, 8, GRND_RANDOM | GRND_INSECURE);
	report("getrandom_bad_flags", bad < 0 && badErrno == EINVAL && both < 0
			&& errno == EINVAL,
		"unknown: %s, RANDOM|INSECURE: %s", strerror(badErrno),
		strerror(errno));
}


static void
test_getauxval()
{
	report("auxv_pagesz", getauxval(AT_PAGESZ) == B_PAGE_SIZE, "%lu",
		getauxval(AT_PAGESZ));

	// Program headers: must parse, and include a PT_LOAD and a PT_DYNAMIC.
	const Elf64_Phdr* headers = (const Elf64_Phdr*)getauxval(AT_PHDR);
	unsigned long count = getauxval(AT_PHNUM);
	unsigned long entrySize = getauxval(AT_PHENT);
	int loads = 0, dynamics = 0;
	for (unsigned long i = 0; headers != NULL && i < count; i++) {
		if (headers[i].p_type == PT_LOAD)
			loads++;
		else if (headers[i].p_type == PT_DYNAMIC)
			dynamics++;
	}
	report("auxv_phdr", headers != NULL && count > 0
			&& entrySize == sizeof(Elf64_Phdr) && loads > 0 && dynamics == 1,
		"phdr %p phnum %lu phent %lu, %d PT_LOAD, %d PT_DYNAMIC", headers,
		count, entrySize, loads, dynamics);

	// AT_ENTRY is inside the program's text; AT_BASE is the runtime loader.
	image_info app = {}, loader = {};
	bool haveApp = false, haveLoader = false;
	int32 cookie = 0;
	image_info info;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE) {
			app = info;
			haveApp = true;
		} else if (strstr(info.name, "runtime_loader") != NULL) {
			loader = info;
			haveLoader = true;
		}
	}
	unsigned long entry = getauxval(AT_ENTRY);
	report("auxv_entry_base", haveApp && haveLoader
			&& entry >= (unsigned long)app.text
			&& entry < (unsigned long)app.text + app.text_size
			&& getauxval(AT_BASE) == (unsigned long)loader.text,
		"entry %#lx in [%p, +%#lx], base %#lx (runtime_loader %p)", entry,
		haveApp ? app.text : NULL,
		haveApp ? (unsigned long)app.text_size : 0UL, getauxval(AT_BASE),
		haveLoader ? loader.text : NULL);

	const uint8_t* random = (const uint8_t*)getauxval(AT_RANDOM);
	report("auxv_random", random != NULL && !all_zero(random, 16)
			&& getauxval(AT_RANDOM) == (unsigned long)random,
		"%p", random);

	const char* execfn = (const char*)getauxval(AT_EXECFN);
	const char* platform = (const char*)getauxval(AT_PLATFORM);
	report("auxv_strings", execfn != NULL
			&& strstr(execfn, "libc_ext_test") != NULL && platform != NULL
			&& strcmp(platform, "x86_64") == 0,
		"execfn '%s' platform '%s'", execfn != NULL ? execfn : "",
		platform != NULL ? platform : "");

	// x86: CPUID 1 EDX; bit 26 is SSE2, present on every x86_64 CPU.
	unsigned long hwcap = getauxval(AT_HWCAP);
	report("auxv_hwcap", (hwcap & (1UL << 26)) != 0, "%#lx", hwcap);

	report("auxv_ids", getauxval(AT_UID) == getuid()
			&& getauxval(AT_EUID) == geteuid()
			&& getauxval(AT_GID) == getgid() && getauxval(AT_SECURE) == 0
			&& getauxval(AT_CLKTCK) > 0,
		"uid %lu secure %lu clktck %lu", getauxval(AT_UID),
		getauxval(AT_SECURE), getauxval(AT_CLKTCK));

	errno = 0;
	unsigned long unknown = getauxval(9999);
	int unknownErrno = errno;
	errno = 0;
	unsigned long vdso = getauxval(AT_SYSINFO_EHDR);
	report("auxv_unknown_enoent", unknown == 0 && unknownErrno == ENOENT
			&& vdso == 0 && errno == ENOENT,
		"unknown: %lu %s, AT_SYSINFO_EHDR: %lu", unknown,
		strerror(unknownErrno), vdso);
}


static void* sFrames[64];
static int sFrameCount = 0;


static int
compare_and_capture(const void* a, const void* b)
{
	if (sFrameCount == 0)
		sFrameCount = backtrace(sFrames, 64);
	return *(const int*)a - *(const int*)b;
}


static void
test_backtrace()
{
	// Capture from inside a qsort() comparator, so that the stack goes
	// through libroot: its frames must resolve to "qsort" in libroot.so.
	int values[] = { 3, 1, 2 };
	qsort(values, 3, sizeof(int), compare_and_capture);

	// libroot's qsort() works in internal helpers, so its frames must name
	// "qsort" (when the frame is in qsort itself) or be a plain offset, and
	// never another exported function. main() calls test_backtrace(), so
	// the frames must also reach back into the test program.
	char** symbols = backtrace_symbols(sFrames, sFrameCount);
	int librootFrames = 0, misnamed = 0, appFrames = 0;
	for (int i = 0; symbols != NULL && i < sFrameCount; i++) {
		if (strstr(symbols[i], "libroot.so(") != NULL) {
			librootFrames++;
			if (strstr(symbols[i], "(qsort+") == NULL
				&& strstr(symbols[i], "(+0x") == NULL) {
				misnamed++;
			}
		}
		if (strstr(symbols[i], "libc_ext_test") != NULL)
			appFrames++;
	}
	report("backtrace", sFrameCount >= 4 && librootFrames > 0
			&& misnamed == 0 && appFrames >= 2,
		"%d frames, %d in libroot (%d misnamed), %d in the program; "
		"frame 1: %s", sFrameCount, librootFrames, misnamed, appFrames,
		symbols != NULL && sFrameCount > 1 ? symbols[1] : "-");
	for (int i = 0; symbols != NULL && i < sFrameCount; i++)
		printf("  frame %d: %s\n", i, symbols[i]);
	free(symbols);

	// Frame 0 is compare_and_capture(), a static function of this program:
	// it must be attributed to the program image (as "(+0x...)", since a
	// static function has no dynamic symbol).
	char** own = backtrace_symbols(sFrames, 1);
	bool ownOk = own != NULL && strstr(own[0], "libc_ext_test(") != NULL;
	free(own);
	report("backtrace_own_frame", ownOk, "frame 0 is in the program");

	// backtrace_symbols_fd: one line per frame.
	int pipeFDs[2];
	pipe(pipeFDs);
	backtrace_symbols_fd(sFrames, sFrameCount, pipeFDs[1]);
	close(pipeFDs[1]);
	char text[16384];
	ssize_t length = read(pipeFDs[0], text, sizeof(text) - 1);
	close(pipeFDs[0]);
	int lines = 0;
	for (ssize_t i = 0; i < length; i++) {
		if (text[i] == '\n')
			lines++;
	}
	report("backtrace_symbols_fd", lines == sFrameCount,
		"%d lines for %d frames", lines, sFrameCount);
}


static void
test_mmsg()
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) != 0) {
		report("mmsg", false, "socketpair: %s", strerror(errno));
		return;
	}

	char payloads[5][16];
	struct iovec sendVectors[5];
	struct mmsghdr out[5];
	memset(out, 0, sizeof(out));
	for (int i = 0; i < 5; i++) {
		snprintf(payloads[i], sizeof(payloads[i]), "message %d", i);
		sendVectors[i].iov_base = payloads[i];
		sendVectors[i].iov_len = strlen(payloads[i]) + 1;
		out[i].msg_hdr.msg_iov = &sendVectors[i];
		out[i].msg_hdr.msg_iovlen = 1;
	}
	int sent = sendmmsg(sockets[0], out, 5, 0);

	char buffers[8][32];
	struct iovec receiveVectors[8];
	struct mmsghdr in[8];
	memset(in, 0, sizeof(in));
	for (int i = 0; i < 8; i++) {
		receiveVectors[i].iov_base = buffers[i];
		receiveVectors[i].iov_len = sizeof(buffers[i]);
		in[i].msg_hdr.msg_iov = &receiveVectors[i];
		in[i].msg_hdr.msg_iovlen = 1;
	}
	// MSG_WAITFORONE: block for the first, then take what is there.
	int received = recvmmsg(sockets[1], in, 8, MSG_WAITFORONE, NULL);
	bool contents = received == 5;
	for (int i = 0; contents && i < 5; i++) {
		contents = in[i].msg_len == strlen(payloads[i]) + 1
			&& strcmp(buffers[i], payloads[i]) == 0
			&& out[i].msg_len == in[i].msg_len;
	}
	report("sendmmsg_recvmmsg", sent == 5 && contents,
		"sent %d, received %d, contents match %d", sent, received, contents);

	errno = 0;
	int empty = recvmmsg(sockets[1], in, 8, MSG_DONTWAIT, NULL);
	report("recvmmsg_empty_eagain", empty < 0 && errno == EAGAIN,
		"result %d errno %s", empty, strerror(errno));

	close(sockets[0]);
	close(sockets[1]);
}


static void
test_copy_file_range()
{
	const char* sourcePath = "/tmp/ce_cfr_source";
	const char* targetPath = "/tmp/ce_cfr_target";
	const size_t kSize = 1024 * 1024 + 4321;	// not a multiple of anything

	uint8_t* data = (uint8_t*)malloc(kSize);
	getrandom(data, kSize, 0);
	int source = open(sourcePath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(source, data, kSize);
	lseek(source, 0, SEEK_SET);
	int target = open(targetPath, O_RDWR | O_CREAT | O_TRUNC, 0644);

	// File positions: copy everything; both positions advance.
	ssize_t copied = copy_file_range(source, NULL, target, NULL, kSize * 2,
		0);
	off_t sourcePosition = lseek(source, 0, SEEK_CUR);
	off_t targetPosition = lseek(target, 0, SEEK_CUR);
	uint8_t* check = (uint8_t*)malloc(kSize);
	ssize_t readBack = pread(target, check, kSize, 0);
	report("copy_file_range_whole", copied == (ssize_t)kSize
			&& sourcePosition == (off_t)kSize
			&& targetPosition == (off_t)kSize && readBack == (ssize_t)kSize
			&& memcmp(data, check, kSize) == 0,
		"copied %zd (asked for twice the size), positions %lld/%lld, "
		"identical %d", copied, (long long)sourcePosition,
		(long long)targetPosition,
		readBack == (ssize_t)kSize && memcmp(data, check, kSize) == 0);

	// Explicit offsets: updated, file positions untouched.
	off_t in = 1000, out = 50;
	lseek(source, 7, SEEK_SET);
	copied = copy_file_range(source, &in, target, &out, 3000, 0);
	pread(target, check, 3000, 50);
	report("copy_file_range_offsets", copied == 3000 && in == 4000
			&& out == 3050 && lseek(source, 0, SEEK_CUR) == 7
			&& memcmp(check, data + 1000, 3000) == 0,
		"copied %zd, offsets %lld/%lld, position kept %d", copied,
		(long long)in, (long long)out, lseek(source, 0, SEEK_CUR) == 7);

	// Overlapping ranges in the same file: EINVAL; disjoint ones work.
	in = 0;
	out = 100;
	errno = 0;
	ssize_t overlap = copy_file_range(source, &in, source, &out, 1000, 0);
	int overlapErrno = errno;
	in = 0;
	out = kSize;
	ssize_t disjoint = copy_file_range(source, &in, source, &out, 1000, 0);
	errno = 0;
	ssize_t flags = copy_file_range(source, NULL, target, NULL, 10, 1);
	int flagsErrno = errno;
	int appendFD = open(targetPath, O_WRONLY | O_APPEND);
	errno = 0;
	ssize_t append = copy_file_range(source, NULL, appendFD, NULL, 10, 0);
	int appendErrno = errno;
	close(appendFD);
	int pipeFDs[2];
	pipe(pipeFDs);
	errno = 0;
	ssize_t piped = copy_file_range(source, NULL, pipeFDs[1], NULL, 10, 0);
	int pipeErrno = errno;
	close(pipeFDs[0]);
	close(pipeFDs[1]);
	report("copy_file_range_errors", overlap < 0 && overlapErrno == EINVAL
			&& disjoint == 1000 && flags < 0 && flagsErrno == EINVAL
			&& append < 0 && appendErrno == EBADF && piped < 0
			&& pipeErrno == EINVAL,
		"overlap: %s, disjoint %zd, flags: %s, O_APPEND: %s, pipe: %s",
		strerror(overlapErrno), disjoint, strerror(flagsErrno),
		strerror(appendErrno), strerror(pipeErrno));

	free(data);
	free(check);
	close(source);
	close(target);
	unlink(sourcePath);
	unlink(targetPath);
}


int
main()
{
	test_getrandom();
	test_getauxval();
	test_backtrace();
	test_mmsg();
	test_copy_file_range();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
