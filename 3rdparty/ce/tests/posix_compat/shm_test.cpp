/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest conformance test for System V shared memory (<sys/shm.h>,
	libroot + kernel). Besides the basics it checks what real users depend
	on: an accurate shm_nattch across fork(), exit() and exec() (PostgreSQL
	uses it to detect a running server), read-only attaches that really
	are read-only, and memory being returned after IPC_RMID and the last
	detach. Output format: see 3rdparty/ce/README.md.
*/


#include <sys/shm.h>

#include <OS.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "shm"


static int sFailures = 0;


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


static long
attach_count(int id)
{
	struct shmid_ds info;
	if (shmctl(id, IPC_STAT, &info) != 0)
		return -1;
	return (long)info.shm_nattch;
}


static int64
used_kib()
{
	system_info info;
	get_system_info(&info);
	return (int64)info.used_pages * (B_PAGE_SIZE / 1024);
}


static void
test_basics()
{
	int id = shmget(IPC_PRIVATE, 10000, IPC_CREAT | 0600);
	report("create_private", id >= 0, "id %d (%s)", id,
		id < 0 ? strerror(errno) : "ok");
	if (id < 0)
		return;

	struct shmid_ds info;
	memset(&info, 0, sizeof(info));
	int result = shmctl(id, IPC_STAT, &info);
	report("stat_new", result == 0 && info.shm_segsz == 10000
			&& info.shm_nattch == 0 && info.shm_cpid == getpid()
			&& (info.shm_perm.mode & 0777) == 0600
			&& info.shm_perm.uid == geteuid() && info.shm_ctime > 0,
		"segsz %zu nattch %lu cpid %d mode 0%o", info.shm_segsz,
		(unsigned long)info.shm_nattch, (int)info.shm_cpid,
		(unsigned)info.shm_perm.mode);

	uint8_t* first = (uint8_t*)shmat(id, NULL, 0);
	bool attached = first != (void*)-1;
	bool aligned = attached && ((uintptr_t)first % SHMLBA) == 0;
	bool zeroed = attached && first[0] == 0 && first[9999] == 0;
	report("attach", attached && aligned && zeroed && attach_count(id) == 1,
		"address %p aligned %d zero-filled %d nattch %ld", first, aligned,
		zeroed, attach_count(id));
	if (!attached)
		return;

	uint8_t* second = (uint8_t*)shmat(id, NULL, 0);
	bool coherent = false;
	if (second != (void*)-1) {
		first[42] = 0x5a;
		second[4000] = 0xa5;
		coherent = second[42] == 0x5a && first[4000] == 0xa5
			&& second != first;
	}
	report("second_attach_coherent", coherent && attach_count(id) == 2,
		"second %p nattch %ld", second, attach_count(id));

	result = shmdt(second);
	long afterDetach = attach_count(id);
	errno = 0;
	int badResult = shmdt((void*)((uintptr_t)first + 4096));
	int badErrno = errno;
	void* heap = malloc(64);
	errno = 0;
	int heapResult = shmdt(heap);
	int heapErrno = errno;
	free(heap);
	report("detach", result == 0 && afterDetach == 1 && badResult < 0
			&& badErrno == EINVAL && heapResult < 0 && heapErrno == EINVAL,
		"detach %d nattch %ld, mid-segment: %s, heap: %s", result,
		afterDetach, strerror(badErrno), strerror(heapErrno));

	// shm_atime/shm_lpid follow attaches.
	shmctl(id, IPC_STAT, &info);
	report("stat_times", info.shm_atime > 0 && info.shm_dtime > 0
			&& info.shm_lpid == getpid(),
		"atime %ld dtime %ld lpid %d", (long)info.shm_atime,
		(long)info.shm_dtime, (int)info.shm_lpid);

	shmdt(first);
	shmctl(id, IPC_RMID, NULL);
}


static void
test_keys()
{
	key_t key = (key_t)(0x43450000 | (getpid() & 0xffff));
	int id = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0600);
	errno = 0;
	int again = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0600);
	int againErrno = errno;
	int same = shmget(key, 4096, 0600);
	errno = 0;
	int larger = shmget(key, 16384, 0600);
	int largerErrno = errno;
	errno = 0;
	int missing = shmget(key + 1, 4096, 0600);
	int missingErrno = errno;
	report("keys", id >= 0 && again < 0 && againErrno == EEXIST && same == id
			&& larger < 0 && largerErrno == EINVAL && missing < 0
			&& missingErrno == ENOENT,
		"id %d, excl: %s, lookup %d, larger: %s, missing: %s", id,
		strerror(againErrno), same, strerror(largerErrno),
		strerror(missingErrno));

	// Another process finds the segment by key.
	pid_t child = fork();
	if (child == 0) {
		int childID = shmget(key, 0, 0600);
		char* memory = (char*)shmat(childID, NULL, 0);
		if (memory == (void*)-1)
			_exit(2);
		strcpy(memory, "hello from the child");
		shmdt(memory);
		_exit(0);
	}
	int status = -1;
	waitpid(child, &status, 0);
	char* memory = (char*)shmat(id, NULL, SHM_RDONLY);
	bool seen = memory != (void*)-1
		&& strcmp(memory, "hello from the child") == 0;
	report("other_process_by_key", WIFEXITED(status)
			&& WEXITSTATUS(status) == 0 && seen,
		"child status 0x%x, read '%s'", status,
		memory != (void*)-1 ? memory : "");
	if (memory != (void*)-1)
		shmdt(memory);

	errno = 0;
	int zero = shmget(IPC_PRIVATE, 0, IPC_CREAT | 0600);
	int zeroErrno = errno;
	errno = 0;
	int huge = shmget(IPC_PRIVATE, (size_t)1 << 62, IPC_CREAT | 0600);
	report("bad_sizes_einval", zero < 0 && zeroErrno == EINVAL && huge < 0
			&& errno == EINVAL,
		"size 0: %s, 4 EiB: %s", strerror(zeroErrno), strerror(errno));

	shmctl(id, IPC_RMID, NULL);
}


static void
segv_exit(int)
{
	// Report the fault through the exit status. Handling SIGSEGV ourselves
	// also keeps debug_server (and its "Crashed program" alert) out of it.
	_exit(42);
}


static void
test_read_only()
{
	int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	uint8_t* writable = (uint8_t*)shmat(id, NULL, 0);
	writable[0] = 7;

	// Writing through a read-only attach must fault; do it in a child.
	pid_t child = fork();
	if (child == 0) {
		signal(SIGSEGV, segv_exit);
		volatile uint8_t* readOnly = (volatile uint8_t*)shmat(id, NULL,
			SHM_RDONLY);
		if (readOnly == (void*)-1)
			_exit(2);
		if (readOnly[0] != 7)
			_exit(3);
		readOnly[0] = 8;
		_exit(0);
	}
	int status = -1;
	waitpid(child, &status, 0);
	report("read_only_faults", WIFEXITED(status)
			&& WEXITSTATUS(status) == 42 && writable[0] == 7,
		"child status 0x%x, value %d", status, writable[0]);

	shmdt(writable);
	shmctl(id, IPC_RMID, NULL);
}


static void
test_fork_exec_exit()
{
	int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	volatile int* shared = (volatile int*)shmat(id, NULL, 0);
	*shared = 0;

	// fork(): the child inherits the attach (nattch 2) and writes through it;
	// the parent sees the write (shared, not copy-on-write). The child's exit
	// detaches it without shmdt().
	int pipeFDs[2];
	pipe(pipeFDs);
	pid_t child = fork();
	if (child == 0) {
		*shared = 1234;
		char go;
		read(pipeFDs[0], &go, 1);
		_exit(0);
	}
	snooze(100000);
	long duringChild = attach_count(id);
	int seen = *shared;
	write(pipeFDs[1], "x", 1);
	int status;
	waitpid(child, &status, 0);
	long afterExit = attach_count(id);
	close(pipeFDs[0]);
	close(pipeFDs[1]);
	report("fork_shared_and_exit_detaches", duringChild == 2 && seen == 1234
			&& afterExit == 1,
		"nattch with child %ld, child wrote %d, nattch after exit %ld",
		duringChild, seen, afterExit);

	// exec(): attached segments are dropped from the new image.
	child = fork();
	if (child == 0) {
		execl("/bin/sleep", "sleep", "1", (char*)NULL);
		_exit(127);
	}
	snooze(400000);
	long duringExec = attach_count(id);
	waitpid(child, &status, 0);
	report("exec_detaches", duringExec == 1 && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0,
		"nattch while child runs /bin/sleep: %ld, status 0x%x", duringExec,
		status);

	shmdt((const void*)shared);
	shmctl(id, IPC_RMID, NULL);
}


static void
test_rmid_and_set()
{
	int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);

	struct shmid_ds info;
	shmctl(id, IPC_STAT, &info);
	time_t created = info.shm_ctime;
	info.shm_perm.mode = 0640;
	sleep(1);
	int setResult = shmctl(id, IPC_SET, &info);
	memset(&info, 0, sizeof(info));
	shmctl(id, IPC_STAT, &info);
	report("ipc_set", setResult == 0 && (info.shm_perm.mode & 0777) == 0640
			&& info.shm_ctime > created,
		"mode 0%o ctime advanced %d", (unsigned)info.shm_perm.mode,
		info.shm_ctime > created);

	// IPC_RMID while attached: the identifier is gone at once, the memory
	// stays usable until the last detach.
	uint8_t* memory = (uint8_t*)shmat(id, NULL, 0);
	memory[0] = 99;
	int removeResult = shmctl(id, IPC_RMID, NULL);
	errno = 0;
	void* again = shmat(id, NULL, 0);
	int againErrno = errno;
	errno = 0;
	int statResult = shmctl(id, IPC_STAT, &info);
	int statErrno = errno;
	memory[1] = memory[0] + 1;
	bool stillUsable = memory[1] == 100;
	int detachResult = shmdt(memory);
	report("rmid_while_attached", removeResult == 0 && again == (void*)-1
			&& againErrno == EINVAL && statResult < 0 && statErrno == EINVAL
			&& stillUsable && detachResult == 0,
		"rmid %d, attach after: %s, stat after: %s, still usable %d, "
		"detach %d", removeResult, strerror(againErrno), strerror(statErrno),
		stillUsable, detachResult);

	errno = 0;
	int badCommand = shmctl(IPC_PRIVATE + 999999, IPC_STAT, &info);
	report("bad_id_einval", badCommand < 0 && errno == EINVAL, "errno %s",
		strerror(errno));
}


static void
test_address_rounding()
{
	int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	// Find a free, aligned address by attaching and detaching once.
	void* probe = shmat(id, NULL, 0);
	shmdt(probe);

	void* unaligned = (void*)((uintptr_t)probe + 123);
	errno = 0;
	void* strict = shmat(id, unaligned, 0);
	int strictErrno = errno;
	void* rounded = shmat(id, unaligned, SHM_RND);
	report("shm_rnd", strict == (void*)-1 && strictErrno == EINVAL
			&& rounded == probe,
		"unaligned: %s, with SHM_RND %p (expected %p)", strerror(strictErrno),
		rounded, probe);
	if (rounded != (void*)-1)
		shmdt(rounded);
	shmctl(id, IPC_RMID, NULL);
}


static void
test_memory_returned()
{
	const size_t kSize = 64 * 1024 * 1024;
	// Warm-up round: one-time costs are not part of the measurement.
	int warm = shmget(IPC_PRIVATE, kSize, IPC_CREAT | 0600);
	shmctl(warm, IPC_RMID, NULL);

	int64 before = used_kib();
	int id = shmget(IPC_PRIVATE, kSize, IPC_CREAT | 0600);
	uint8_t* memory = (uint8_t*)shmat(id, NULL, 0);
	int64 untouched = used_kib() - before;
	if (memory != (void*)-1)
		memset(memory, 0x3c, kSize);
	int64 touched = used_kib() - before;
	shmctl(id, IPC_RMID, NULL);
	int64 afterRemove = used_kib() - before;
	shmdt(memory);
	int64 afterDetach = used_kib() - before;

	// Lazy: an untouched 64 MiB segment costs (almost) nothing; touching
	// it costs ~64 MiB; IPC_RMID keeps it while attached; the last detach
	// frees it. 8 MiB tolerance for unrelated activity.
	const int64 kTolerance = 8 * 1024;
	report("memory_lazy_and_returned", memory != (void*)-1
			&& untouched < kTolerance
			&& touched > 64 * 1024 - kTolerance
			&& afterRemove > 64 * 1024 - kTolerance
			&& afterDetach < kTolerance,
		"KiB: untouched %+lld, touched %+lld, after rmid %+lld, after "
		"detach %+lld", (long long)untouched, (long long)touched,
		(long long)afterRemove, (long long)afterDetach);

	// Many segments created and removed: no exhaustion.
	int created = 0;
	for (int i = 0; i < 600; i++) {
		int churn = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
		if (churn < 0)
			break;
		void* mapped = shmat(churn, NULL, 0);
		if (mapped != (void*)-1)
			shmdt(mapped);
		shmctl(churn, IPC_RMID, NULL);
		created++;
	}
	report("churn_600_segments", created == 600, "created %d", created);
}


int
main()
{
	test_basics();
	test_keys();
	test_read_only();
	test_fork_exec_exit();
	test_rmid_and_set();
	test_address_rounding();
	test_memory_returned();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
