/*
 * Threads and futexes, with raw system calls: clone(CLONE_THREAD), clone3(),
 * CLONE_SETTLS/PARENT_SETTID/CHILD_SETTID/CHILD_CLEARTID, futex WAIT/WAKE,
 * the bitset variants, timeouts, CMP_REQUEUE, WAKE_OP, set_tid_address,
 * robust lists, CPU affinity. Prints "ok" and exits 0 if all is well;
 * otherwise prints the first failed check and exits with its number.
 */
#include "sys.h"

enum {
	SYS_futex = 202, SYS_sched_getaffinity = 204, SYS_set_tid_address = 218,
	SYS_set_robust_list = 273, SYS_get_robust_list = 274, SYS_getcpu = 309,
	SYS_clone3 = 435, SYS_arch_prctl = 158,
};

#define CLONE_VM				0x00000100
#define CLONE_FS				0x00000200
#define CLONE_FILES				0x00000400
#define CLONE_SIGHAND			0x00000800
#define CLONE_THREAD			0x00010000
#define CLONE_SYSVSEM			0x00040000
#define CLONE_SETTLS			0x00080000
#define CLONE_PARENT_SETTID		0x00100000
#define CLONE_CHILD_CLEARTID	0x00200000
#define CLONE_CHILD_SETTID		0x01000000
#define THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND \
	| CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID \
	| CLONE_CHILD_CLEARTID)

#define FUTEX_WAIT			0
#define FUTEX_WAKE			1
#define FUTEX_CMP_REQUEUE	4
#define FUTEX_WAKE_OP		5
#define FUTEX_WAIT_BITSET	9
#define FUTEX_WAKE_BITSET	10
#define FUTEX_PRIVATE		128
#define FUTEX_OWNER_DIED	0x40000000
#define FUTEX_OP(op, oparg, cmp, cmparg) \
	(((op & 0xf) << 28) | ((cmp & 0xf) << 24) | ((oparg & 0xfff) << 12) \
		| (cmparg & 0xfff))

#define EAGAIN		11
#define EINVAL		22
#define ENOSYS		38
#define ETIMEDOUT	110

#define STACK_SIZE	(64 * 1024)

typedef int (*thread_fn)(void*);

/*	Starts a thread on the given stack top: fn and arg go on the new stack,
	the child pops them, runs fn(arg) and exits with its result. */
long raw_clone(long flags, void* stackTop, int* parentTid, int* childTid,
	void* tls, thread_fn fn, void* arg);
__asm__(
	".globl raw_clone\n"
	"raw_clone:\n"
	"\tsub $16, %rsi\n"
	"\tmov %r9, (%rsi)\n"
	"\tmov 8(%rsp), %rax\n"
	"\tmov %rax, 8(%rsi)\n"
	"\tmov %rcx, %r10\n"
	"\tmov $56, %eax\n"
	"\tsyscall\n"
	"\ttest %rax, %rax\n"
	"\tjnz 1f\n"
	"\txor %ebp, %ebp\n"
	"\tpop %rax\n"
	"\tpop %rdi\n"
	"\tcall *%rax\n"
	"\tmov %eax, %edi\n"
	"\tmov $60, %eax\n"
	"\tsyscall\n"
	"\thlt\n"
	"1:\tret\n");

/*	clone3(): the child's stack pointer is stack + stack_size. fn and arg are
	placed at the top by the caller. */
long raw_clone3(void* args, long size);
__asm__(
	".globl raw_clone3\n"
	"raw_clone3:\n"
	"\tmov $435, %eax\n"
	"\tsyscall\n"
	"\ttest %rax, %rax\n"
	"\tjnz 1f\n"
	"\txor %ebp, %ebp\n"
	"\tpop %rax\n"
	"\tpop %rdi\n"
	"\tcall *%rax\n"
	"\tmov %eax, %edi\n"
	"\tmov $60, %eax\n"
	"\tsyscall\n"
	"\thlt\n"
	"1:\tret\n");

struct clone_args {
	u64 flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size,
		tls;
};

static int failed;
static void check(int ok, int number, const char* what)
{
	if (ok || failed)
		return;
	failed = number;
	put("FAIL: ");
	put(what);
	put("\n");
}

static i64 futex(volatile int* word, int op, int value, const void* timeout,
	volatile int* word2, int value3)
{
	return sys6(SYS_futex, (i64)word, op, value, (i64)timeout, (i64)word2,
		value3);
}

static void* alloc_stack(void)
{
	i64 p = sys6(SYS_mmap, 0, STACK_SIZE, 3, 0x22 | 0x20000, -1, 0);
		/* PROT_READ|WRITE, MAP_PRIVATE|ANONYMOUS|MAP_STACK */
	return p < 0 ? 0 : (void*)p;
}

/* Waits until the kernel clears the TID word (CLONE_CHILD_CLEARTID). */
static void join(volatile int* tid)
{
	for (;;) {
		int value = *tid;
		if (value == 0)
			return;
		futex(tid, FUTEX_WAIT, value, 0, 0, 0);
	}
}

static void pause_ms(int ms)
{
	i64 ts[2] = { 0, ms * 1000000L };
	sys2(SYS_nanosleep, ts, 0);
}

static int atomic_add(volatile int* p, int v)
{
	return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

static i64 thread_start(void* stack, volatile int* tid, void* tls,
	thread_fn fn, void* arg)
{
	return raw_clone(THREAD_FLAGS, (char*)stack + STACK_SIZE, (int*)tid,
		(int*)tid, tls, fn, arg);
}


/* --- 1: basic thread, TLS, TIDs --- */

struct tls_block { void* self; int magic; };
static struct tls_block tls1;
static volatile int shared;
static volatile int childTidSeen, childPidSeen;
static volatile u64 childFs;

static int basic_thread(void* arg)
{
	u64 fs;
	__asm__ volatile("mov %%fs:0, %0" : "=r"(fs));
	childFs = fs;
	childTidSeen = (int)sys0(SYS_gettid);
	childPidSeen = (int)sys0(SYS_getpid);
	shared = (int)(long)arg;
	return 0;
}


/* --- 2: futex ping-pong --- */

static volatile int ball;		/* 0: main's turn, 1: thread's turn */
#define ROUNDS 2000

static int pong_thread(void* arg)
{
	(void)arg;
	for (int i = 0; i < ROUNDS; i++) {
		while (ball != 1)
			futex(&ball, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0, 0, 0);
		ball = 0;
		futex(&ball, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0);
	}
	return 0;
}


/* --- 3: many threads --- */

#define WORKERS 8
static volatile int counter;

static int worker_thread(void* arg)
{
	(void)arg;
	for (int i = 0; i < 1000; i++)
		atomic_add(&counter, 1);
	return 0;
}


/* --- 4: requeue --- */

static volatile int cond1, cond2, released;

static int requeue_thread(void* arg)
{
	(void)arg;
	while (!released)
		futex(&cond1, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0, 0, 0);
	return 0;
}


/* --- 5: wake-op --- */

static volatile int opWord, opDone;

static int wakeop_thread(void* arg)
{
	(void)arg;
	while (!opDone)
		futex(&opWord, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0, 0, 0);
	return 0;
}


/* --- 6: robust list --- */

struct robust_list { struct robust_list* next; };
struct robust_head {
	struct robust_list list;
	long futex_offset;
	struct robust_list* pending;
};
struct robust_mutex { struct robust_list link; volatile int lock; };
static struct robust_head robustHead;
static struct robust_mutex robustMutex;

static int robust_thread(void* arg)
{
	(void)arg;
	robustHead.list.next = &robustMutex.link;
	robustHead.futex_offset = (char*)&robustMutex.lock
		- (char*)&robustMutex.link;
	robustHead.pending = 0;
	robustMutex.link.next = &robustHead.list;
	robustMutex.lock = (int)sys0(SYS_gettid);
	if (sys2(SYS_set_robust_list, &robustHead, sizeof(robustHead)) != 0)
		return 1;
	/* exit while holding the lock */
	return 0;
}


/* --- 9: a new thread inherits the floating-point control state --- */

static volatile unsigned threadMxcsr;

static int mxcsr_thread(void* arg)
{
	(void)arg;
	unsigned value;
	__asm__ volatile("stmxcsr %0" : "=m"(value));
	threadMxcsr = value;
	return 0;
}


/* --- 7: clone3 --- */

static volatile int clone3Ran;

static int clone3_thread(void* arg)
{
	clone3Ran = (int)(long)arg;
	return 0;
}


int check_main(int argc, char** argv)
{
	(void)argc; (void)argv;
	int pid = (int)sys0(SYS_getpid);
	int mainTid = (int)sys0(SYS_gettid);

	/* set_tid_address returns the caller's TID. */
	static volatile int mainTidWord;
	check(sys1(SYS_set_tid_address, &mainTidWord) == mainTid, 1,
		"set_tid_address");

	/* 1: a thread with its own TLS, sharing memory. */
	tls1.self = &tls1;
	tls1.magic = 0x5eed;
	void* stack = alloc_stack();
	check(stack != 0, 2, "mmap stack");
	static volatile int tid1;
	i64 t = thread_start(stack, &tid1, &tls1, basic_thread, (void*)77L);
	check(t > 0, 3, "clone thread");
	if (t > 0) {
		join(&tid1);
		check(shared == 77, 4, "thread wrote shared memory");
		check(childTidSeen == (int)t, 5, "gettid in thread == clone result");
		check(childPidSeen == pid, 6, "getpid in thread == process id");
		check(childTidSeen != mainTid, 7, "thread TID differs");
		check(childFs == (u64)&tls1, 8, "CLONE_SETTLS");
	}
	/* The main thread's FS base is unchanged. */
	u64 mainFs = 0;
	sys2(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, &mainFs);
	check(mainFs != (u64)&tls1, 9, "main FS base untouched");

	/* futex basics */
	static volatile int word = 5;
	check(futex(&word, FUTEX_WAIT, 6, 0, 0, 0) == -EAGAIN, 10,
		"WAIT value mismatch -> EAGAIN");
	i64 rel[2] = { 0, 20 * 1000000L };
	check(futex(&word, FUTEX_WAIT | FUTEX_PRIVATE, 5, rel, 0, 0) == -ETIMEDOUT,
		11, "WAIT relative timeout");
	i64 past[2] = { 0, 1 };	/* CLOCK_MONOTONIC: long gone */
	check(futex(&word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE, 5, past, 0, -1)
		== -ETIMEDOUT, 12, "WAIT_BITSET absolute timeout");
	check(futex(&word, FUTEX_WAIT_BITSET, 5, 0, 0, 0) == -EINVAL, 13,
		"WAIT_BITSET bitset 0 -> EINVAL");
	check(futex(&word, FUTEX_WAKE, 1, 0, 0, 0) == 0, 14,
		"WAKE without waiters");
	check(futex((volatile int*)((char*)&word + 1), FUTEX_WAKE, 1, 0, 0, 0)
		== -EINVAL, 15, "misaligned futex -> EINVAL");
	check(futex(&word, FUTEX_WAKE_BITSET, 1, 0, 0, 0) == -EINVAL, 17,
		"WAKE_BITSET bitset 0 -> EINVAL");

	/* 2: ping-pong between two threads */
	static volatile int tid2;
	t = thread_start(alloc_stack(), &tid2, &tls1, pong_thread, 0);
	check(t > 0, 20, "clone pong thread");
	if (t > 0) {
		for (int i = 0; i < ROUNDS; i++) {
			ball = 1;
			futex(&ball, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0);
			while (ball != 0)
				futex(&ball, FUTEX_WAIT | FUTEX_PRIVATE, 1, 0, 0, 0);
		}
		join(&tid2);
	}

	/* 3: several threads at once */
	static volatile int tids[WORKERS];
	int started = 0;
	for (int i = 0; i < WORKERS; i++) {
		if (thread_start(alloc_stack(), &tids[i], &tls1, worker_thread, 0) > 0)
			started++;
	}
	check(started == WORKERS, 30, "start workers");
	for (int i = 0; i < WORKERS; i++)
		join(&tids[i]);
	check(counter == started * 1000, 31, "workers' count");

	/* 4: CMP_REQUEUE moves waiters from cond1 to cond2 */
	static volatile int rtid[2];
	for (int i = 0; i < 2; i++) {
		check(thread_start(alloc_stack(), &rtid[i], &tls1, requeue_thread, 0)
			> 0, 40, "start requeue waiters");
	}
	check(futex(&cond1, FUTEX_CMP_REQUEUE, 0, (void*)2L, &cond2, 1)
		== -EAGAIN, 41, "CMP_REQUEUE value mismatch -> EAGAIN");
	int moved = 0;
	for (int tries = 0; moved < 2 && tries < 2000; tries++) {
		i64 r = futex(&cond1, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 0,
			(void*)2L, &cond2, 0);
		if (r > 0)
			moved += (int)r;
		if (moved < 2)
			pause_ms(1);
	}
	check(moved == 2, 42, "CMP_REQUEUE moved both waiters");
	released = 1;
	int woken = 0;
	for (int tries = 0; woken < 2 && tries < 2000; tries++) {
		i64 r = futex(&cond2, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0);
		if (r > 0)
			woken += (int)r;
		else
			pause_ms(1);
	}
	check(woken == 2, 43, "waiters woken on the requeue target");
	join(&rtid[0]);
	join(&rtid[1]);

	/* 5: WAKE_OP */
	static volatile int scratch = 4;
	check(futex(&word, FUTEX_WAKE_OP, 1, (void*)1L, &scratch,
		FUTEX_OP(1 /* ADD */, 3, 0 /* EQ */, 4)) == 0, 50,
		"WAKE_OP without waiters");
	check(scratch == 7, 51, "WAKE_OP ADD");
	check(futex(&word, FUTEX_WAKE_OP, 1, (void*)1L, &scratch,
		FUTEX_OP(2 | 8 /* OR, oparg shift */, 4, 0, 0)) == 0, 52,
		"WAKE_OP OR shifted");
	check(scratch == (7 | 16), 53, "WAKE_OP OR shifted value");
	static volatile int optid;
	check(thread_start(alloc_stack(), &optid, &tls1, wakeop_thread, 0) > 0,
		54, "start wake-op waiter");
	int opWoken = 0;
	for (int tries = 0; !opWoken && tries < 2000; tries++) {
		/* Keep opWord 0 (SET 0); wake its waiter when the old value is 0. */
		i64 r = futex(&word, FUTEX_WAKE_OP | FUTEX_PRIVATE, 1, (void*)1L,
			&opWord, FUTEX_OP(0 /* SET */, 0, 0 /* EQ */, 0));
		if (r == 1)
			opWoken = 1;
		else
			pause_ms(1);
	}
	check(opWoken, 55, "WAKE_OP woke the waiter on the second word");
	opDone = 1;
	opWord = 1;
	futex(&opWord, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0, 0, 0);
	join(&optid);

	/* 6: robust list: a thread dies holding a lock */
	static volatile int robtid;
	check(thread_start(alloc_stack(), &robtid, &tls1, robust_thread, 0) > 0,
		60, "start robust thread");
	join(&robtid);
	check(robustMutex.lock == FUTEX_OWNER_DIED, 61,
		"robust lock marked FUTEX_OWNER_DIED");
	check(sys2(SYS_set_robust_list, &robustHead, 16) == -EINVAL, 62,
		"set_robust_list bad length");
	check(sys2(SYS_set_robust_list, &robustHead, sizeof(robustHead)) == 0, 63,
		"set_robust_list");
	void* head = 0;
	u64 headLen = 0;
	check(sys3(SYS_get_robust_list, 0, &head, &headLen) == 0
		&& head == &robustHead && headLen == sizeof(robustHead), 64,
		"get_robust_list");
	sys2(SYS_set_robust_list, 0, sizeof(robustHead));

	/* 7: clone3 */
	char* stack3 = alloc_stack();
	static volatile int tid3;
	if (stack3 != 0) {
		/* fn and arg at the top, where the child's stack pointer starts */
		u64* top = (u64*)(stack3 + STACK_SIZE - 16);
		top[0] = (u64)clone3_thread;
		top[1] = 33;
		struct clone_args args = {
			THREAD_FLAGS & ~(u64)CLONE_SETTLS, 0, (u64)&tid3, (u64)&tid3, 0,
			(u64)stack3, STACK_SIZE - 16, 0
		};
		i64 r = raw_clone3(&args, sizeof(args));
		if (r == -ENOSYS) {
			put("clone3 not supported\n");
			failed = failed ? failed : 70;
		} else {
			check(r > 0, 71, "clone3");
			if (r > 0) {
				join(&tid3);
				check(clone3Ran == 33, 72, "clone3 thread ran");
			}
		}
	}

	/* 8: CPU affinity and getcpu */
	u64 mask[16];
	for (int i = 0; i < 16; i++)
		mask[i] = 0;
	i64 size = sys3(SYS_sched_getaffinity, 0, sizeof(mask), mask);
	check(size > 0 && size <= (i64)sizeof(mask) && (size % 8) == 0, 80,
		"sched_getaffinity size");
	int cpus = 0;
	for (int i = 0; i < 16; i++) {
		for (u64 bits = mask[i]; bits != 0; bits &= bits - 1)
			cpus++;
	}
	check(cpus >= 1, 81, "sched_getaffinity CPU count");
	check(sys3(SYS_sched_getaffinity, 0, 3, mask) == -EINVAL, 82,
		"sched_getaffinity short buffer");
	unsigned cpu = 999, node = 999;
	check(sys3(SYS_getcpu, &cpu, &node, 0) == 0 && cpu < 1024 && node == 0,
		83, "getcpu");

	/* 9: MXCSR (rounding mode) is inherited, as C11 and Linux require */
	unsigned savedMxcsr, roundDown;
	__asm__ volatile("stmxcsr %0" : "=m"(savedMxcsr));
	roundDown = (savedMxcsr & ~0x6000u) | 0x2000u;	/* RC = round down */
	__asm__ volatile("ldmxcsr %0" : : "m"(roundDown));
	static volatile int mxtid;
	i64 mt = thread_start(alloc_stack(), &mxtid, &tls1, mxcsr_thread, 0);
	__asm__ volatile("ldmxcsr %0" : : "m"(savedMxcsr));
	check(mt > 0, 90, "start MXCSR thread");
	if (mt > 0) {
		join(&mxtid);
		check((threadMxcsr & 0x6000u) == 0x2000u, 91,
			"thread inherited the rounding mode");
	}

	if (failed)
		return failed;
	put("ok\n");
	return 0;
}
