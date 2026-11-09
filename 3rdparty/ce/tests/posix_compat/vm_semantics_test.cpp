/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test of the memory semantics V8 and PartitionAlloc (Chromium,
	Node.js, Electron) rely on, as Linux provides them:

	 - large PROT_NONE reservations (V8's pointer cage and sandbox reserve
	   up to a terabyte) that commit no memory;
	 - mprotect() of sub-ranges of a reservation, committing only those
	   pages; access to the rest still faults;
	 - mmap(MAP_FIXED) over parts of a reservation (V8's DecommitPages() maps
	   PROT_NONE anonymous memory over committed pages and CHECKs that it got
	   the same address back);
	 - madvise(MADV_DONTNEED) on private anonymous memory: the pages read as
	   zero afterwards (PartitionAlloc's decommit relies on this to hand out
	   zeroed memory again), also after fork() and in split mappings;
	 - madvise(MADV_FREE) accepted (V8's DiscardSystemPages());
	 - munmap() of sub-ranges (V8's OS::Release()).

	Memory use is read from get_system_info()'s needed_memory (committed
	memory); limits are generous because other programs run meanwhile.
	Output format: see 3rdparty/ce/README.md.
*/


#include <OS.h>

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "vm_semantics"


static int sFailures = 0;
static const size_t kPage = B_PAGE_SIZE;
static const size_t kMiB = 1024 * 1024;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
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


static int64
committed()
{
	system_info info;
	get_system_info(&info);
	return (int64)info.needed_memory;
}


//!	Physical memory in use: what touching pages costs and freeing gives back
//	(user anonymous memory is overcommitted, so committed memory does not
//	show it).
static int64
resident()
{
	system_info info;
	get_system_info(&info);
	return (int64)info.used_pages * B_PAGE_SIZE;
}


// #pragma mark - fault probing


static sigjmp_buf sFaultJump;
static volatile sig_atomic_t sFaulted;


static void
fault_handler(int)
{
	sFaulted = 1;
	siglongjmp(sFaultJump, 1);
}


//!	Returns whether reading (or writing) \a address faults.
static bool
faults(volatile char* address, bool write)
{
	struct sigaction action = {};
	action.sa_handler = fault_handler;
	struct sigaction oldSegv, oldBus;
	sigaction(SIGSEGV, &action, &oldSegv);
	sigaction(SIGBUS, &action, &oldBus);

	sFaulted = 0;
	if (sigsetjmp(sFaultJump, 1) == 0) {
		if (write)
			*address = 1;
		else
			(void)*address;
	}

	sigaction(SIGSEGV, &oldSegv, NULL);
	sigaction(SIGBUS, &oldBus, NULL);
	return sFaulted != 0;
}


static bool
all_zero(const char* address, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (address[i] != 0)
			return false;
	}
	return true;
}


static void*
reserve(size_t size)
{
	return mmap(NULL, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}


static void*
map_anonymous(size_t size)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}


// #pragma mark - tests


static void
test_reservations()
{
	// A terabyte (V8's sandbox), then 64 GiB (pointer cage and code range).
	static const size_t kSizes[] = { (size_t)1 << 40, (size_t)64 << 30 };
	static const char* kNames[] = { "reserve_1tib", "reserve_64gib" };
	for (int i = 0; i < 2; i++) {
		int64 before = committed();
		void* address = reserve(kSizes[i]);
		int error = address == MAP_FAILED ? errno : 0;
		int64 grown = committed() - before;
		report(kNames[i], address != MAP_FAILED && grown < (int64)(64 * kMiB),
			"%s, committed memory grew by %lld KiB",
			address == MAP_FAILED ? strerror(error) : "reserved",
			(long long)(grown / 1024));
		if (address != MAP_FAILED) {
			bool readFaults = faults((char*)address + kSizes[i] / 2, false);
			if (i == 1)
				report("reserve_faults", readFaults, "read in a PROT_NONE "
					"reservation %s", readFaults ? "faults" : "did NOT fault");
			munmap(address, kSizes[i]);
		}
	}
}


static void
test_subrange_commit()
{
	const size_t size = 1024 * kMiB;
	char* base = (char*)reserve(size);
	if (base == MAP_FAILED) {
		report("subrange_mprotect", false, "reserve 1 GiB: %s",
			strerror(errno));
		return;
	}

	int64 before = resident();
	char* middle = base + 512 * kMiB;
	int result = mprotect(middle, 4 * kMiB, PROT_READ | PROT_WRITE);
	bool zero = result == 0 && all_zero(middle, 4 * kMiB);
	if (result == 0)
		memset(middle, 0x5a, 4 * kMiB);
	int64 grown = resident() - before;
	report("subrange_mprotect", result == 0 && zero
			&& grown < (int64)(64 * kMiB),
		"mprotect %s, zero-filled %d, memory in use grew by %lld KiB "
		"(want about 4096, not the 1 GiB reservation)",
		result == 0 ? "ok" : strerror(errno), zero,
		(long long)(grown / 1024));

	bool beforeFaults = faults(middle - kPage, false);
	bool afterFaults = faults(middle + 4 * kMiB, false);
	bool insideOK = !faults(middle + kPage, true);
	report("subrange_neighbours_fault", beforeFaults && afterFaults
			&& insideOK,
		"page before %s, page after %s, inside %s",
		beforeFaults ? "faults" : "NO fault", afterFaults ? "faults"
			: "NO fault", insideOK ? "writable" : "FAULTS");

	// V8 toggles code pages between RW and RX.
	int toExec = mprotect(middle, kPage, PROT_READ | PROT_EXEC);
	bool readable = !faults(middle, false);
	bool notWritable = faults(middle, true);
	int back = mprotect(middle, kPage, PROT_READ | PROT_WRITE);
	report("subrange_rx_toggle", toExec == 0 && readable && notWritable
			&& back == 0 && middle[1] == 0x5a,
		"RX %d (read %d, write faults %d), back to RW %d, content kept %d",
		toExec, readable, notWritable, back, middle[1] == 0x5a);

	// PartitionAlloc's decommit: PROT_NONE, then MADV_DONTNEED; recommit
	// with PROT_READ | PROT_WRITE must give zeroed pages and the memory
	// back to the system meanwhile.
	int64 residentBefore = resident();
	int none = mprotect(middle, 4 * kMiB, PROT_NONE);
	int advice = madvise(middle, 4 * kMiB, MADV_DONTNEED);
	int64 released = residentBefore - resident();
	int again = mprotect(middle, 4 * kMiB, PROT_READ | PROT_WRITE);
	bool zeroAgain = again == 0 && all_zero(middle, 4 * kMiB);
	report("decommit_recommit_zero", none == 0 && advice == 0 && zeroAgain,
		"PROT_NONE %d, MADV_DONTNEED %d, recommit %d, zero-filled %d",
		none, advice, again, zeroAgain);
	report("decommit_releases_memory", released >= (int64)(2 * kMiB),
		"memory in use dropped by %lld KiB (want about 4096)",
		(long long)(released / 1024));

	// munmap() of a sub-range (V8's OS::Release() / FreePages()).
	int unmapped = munmap(middle + kMiB, kMiB);
	bool goneFaults = faults(middle + kMiB + kPage, false);
	bool restOK = !faults(middle, true) && !faults(middle + 2 * kMiB, true);
	report("munmap_subrange", unmapped == 0 && goneFaults && restOK,
		"munmap %d, unmapped part faults %d, rest usable %d", unmapped,
		goneFaults, restOK);

	munmap(base, size);
}


static void
test_map_fixed()
{
	const size_t size = 64 * kMiB;
	char* base = (char*)reserve(size);
	if (base == MAP_FAILED) {
		report("map_fixed_over_reservation", false, "reserve: %s",
			strerror(errno));
		return;
	}

	// Commit a part by mapping read-write memory over it.
	char* part = base + 16 * kMiB;
	void* mapped = mmap(part, 8 * kMiB, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	bool ok = mapped == part && all_zero(part, 8 * kMiB);
	if (ok)
		memset(part, 0x33, 8 * kMiB);
	report("map_fixed_over_reservation", ok,
		"mmap(MAP_FIXED) returned %p for %p, zero-filled %d", mapped, part,
		ok);

	// V8's DecommitPages(): PROT_NONE anonymous memory over committed pages,
	// at the same address; the memory goes back, recommitting gives zeros.
	int64 before = resident();
	void* decommitted = mmap(part, 8 * kMiB, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
	int64 released = before - resident();
	bool decommitFaults = faults(part, false);
	int recommit = mprotect(part, 8 * kMiB, PROT_READ | PROT_WRITE);
	bool zero = recommit == 0 && all_zero(part, 8 * kMiB);
	report("v8_decommit_pages", decommitted == part && decommitFaults
			&& recommit == 0 && zero,
		"mmap returned %p (want %p), faults %d, recommit %d, zero %d",
		decommitted, part, decommitFaults, recommit, zero);
	report("v8_decommit_releases_memory", released >= (int64)(4 * kMiB),
		"memory in use dropped by %lld KiB (want about 8192)",
		(long long)(released / 1024));

	munmap(base, size);
}


static void
test_dontneed()
{
	// Plain private anonymous memory.
	const size_t size = 16 * kPage;
	char* memory = (char*)map_anonymous(size);
	memset(memory, 0x77, size);
	int result = madvise(memory + 4 * kPage, 8 * kPage, MADV_DONTNEED);
	bool zeroed = all_zero(memory + 4 * kPage, 8 * kPage);
	bool kept = memory[0] == 0x77 && memory[15 * kPage] == 0x77;
	report("dontneed_zero_fill", result == 0 && zeroed && kept,
		"madvise %d, range zero %d, outside kept %d", result, zeroed, kept);

	// Unaligned start is EINVAL, as on Linux.
	errno = 0;
	result = madvise(memory + 1, kPage, MADV_DONTNEED);
	report("dontneed_unaligned_einval", result == -1 && errno == EINVAL,
		"madvise %d %s", result, strerror(errno));

	// In a mapping split by mprotect() and by a MAP_FIXED hole.
	memset(memory, 0x66, size);
	mprotect(memory + 2 * kPage, 2 * kPage, PROT_READ);
	mmap(memory + 8 * kPage, kPage, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	memory[8 * kPage] = 0x66;
	result = madvise(memory, size, MADV_DONTNEED);
	bool splitZero = all_zero(memory, size);
	report("dontneed_split_mapping", result == 0 && splitZero,
		"madvise %d, all zero %d (first byte %#x, after hole %#x)", result,
		splitZero, (unsigned char)memory[0],
		(unsigned char)memory[9 * kPage]);
	munmap(memory, size);

	// After fork(): the parent's pages are copy-on-write, shared with the
	// child; DONTNEED must still give the parent zeros and leave the child
	// its copy.
	memory = (char*)map_anonymous(size);
	memset(memory, 0x44, size);
	int fds[2];
	pipe(fds);
	int back[2];
	pipe(back);
	pid_t child = fork();
	if (child == 0) {
		char go;
		read(fds[0], &go, 1);
		char value = memory[5 * kPage];
		write(back[1], &value, 1);
		_exit(0);
	}
	result = madvise(memory, size, MADV_DONTNEED);
	bool parentZero = all_zero(memory, size);
	write(fds[1], "g", 1);
	char childValue = 0;
	read(back[0], &childValue, 1);
	waitpid(child, NULL, 0);
	report("dontneed_after_fork", result == 0 && parentZero
			&& childValue == 0x44,
		"madvise %d, parent zero %d, child still sees %#x (want 0x44)",
		result, parentZero, (unsigned char)childValue);
	close(fds[0]);
	close(fds[1]);
	close(back[0]);
	close(back[1]);

	// And after the child is gone.
	memset(memory, 0x45, size);
	child = fork();
	if (child == 0)
		_exit(0);
	waitpid(child, NULL, 0);
	result = madvise(memory, size, MADV_DONTNEED);
	parentZero = all_zero(memory, size);
	report("dontneed_after_child_exit", result == 0 && parentZero,
		"madvise %d, zero %d", result, parentZero);
	munmap(memory, size);

	// Shared anonymous memory keeps its contents (Linux: shmem pages are
	// only unmapped); that is what the other process sees.
	memory = (char*)mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	memset(memory, 0x22, size);
	result = madvise(memory, size, MADV_DONTNEED);
	report("dontneed_shared_kept", result == 0 && memory[3 * kPage] == 0x22,
		"madvise %d, content %#x (want 0x22)", result,
		(unsigned char)memory[3 * kPage]);
	munmap(memory, size);

	// MADV_DONTNEED on a PROT_NONE reservation is fine.
	char* reservation = (char*)reserve(64 * kMiB);
	result = madvise(reservation, 64 * kMiB, MADV_DONTNEED);
	report("dontneed_reservation", result == 0, "madvise %d %s", result,
		result == 0 ? "" : strerror(errno));
	munmap(reservation, 64 * kMiB);
}


static void
test_free()
{
	const size_t size = 16 * kPage;
	char* memory = (char*)map_anonymous(size);
	memset(memory, 0x11, size);
	int result = madvise(memory, size, MADV_FREE);
	// Either the old contents or zeros are allowed; the memory stays usable.
	bool consistent = true;
	for (size_t i = 0; i < size; i += kPage) {
		if (memory[i] != 0 && memory[i] != 0x11)
			consistent = false;
	}
	memory[0] = 0x12;
	report("madv_free", result == 0 && consistent && memory[0] == 0x12,
		"madvise %d, contents old-or-zero %d", result, consistent);
	munmap(memory, size);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	test_reservations();
	test_subrange_commit();
	test_map_fixed();
	test_dontneed();
	test_free();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
