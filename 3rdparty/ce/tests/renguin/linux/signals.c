/*
 * Signals with raw system calls: rt_sigaction with an SA_RESTORER
 * trampoline, delivery through a Linux rt_sigframe (handler arguments,
 * siginfo, ucontext), rt_sigreturn (registers, vector registers, MXCSR, the
 * signal mask), masks and pending signals, SA_RESETHAND, SA_NODEFER, nested
 * signals, a SIGSEGV handler that skips the faulting instruction, the
 * alternate stack, syscall restart with and without SA_RESTART, sigsuspend,
 * sigtimedwait, sigqueueinfo, interval timers and a signal sent to one
 * thread. Prints "ok" and exits 0, or names the first failed check.
 */
#include "sys.h"

enum {
	SYS_rt_sigaction = 13, SYS_rt_sigprocmask = 14, SYS_alarm = 37,
	SYS_getitimer = 36, SYS_setitimer = 38, SYS_rt_sigpending = 127,
	SYS_rt_sigtimedwait = 128, SYS_rt_sigqueueinfo = 129,
	SYS_rt_sigsuspend = 130, SYS_sigaltstack = 131, SYS_tgkill = 234,
	SYS_futex = 202,
};

#define SIGSEGV 11
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGALRM 14
#define SIGCHLD 17
#define SIGRTMIN 34

#define SA_SIGINFO		0x00000004
#define SA_RESTORER		0x04000000
#define SA_ONSTACK		0x08000000
#define SA_RESTART		0x10000000
#define SA_NODEFER		0x40000000
#define SA_RESETHAND	0x80000000

#define SIG_BLOCK		0
#define SIG_UNBLOCK		1
#define SIG_SETMASK		2
#define SS_ONSTACK		1
#define SS_DISABLE		2

#define SI_QUEUE		(-1)
#define SI_TKILL		(-6)
#define SEGV_MAPERR		1

#define EINTR			4
#define EAGAIN			11
#define EPERM			1
#define EINVAL			22

#define BIT(s) (1UL << ((s) - 1))

struct k_sigaction {
	void*	handler;
	u64		flags;
	void*	restorer;
	u64		mask;
};

struct siginfo {
	int		si_signo, si_errno, si_code, pad;
	union {
		struct { int pid; unsigned uid; u64 value; } rt;
		struct { u64 addr; } fault;
		char raw[112];
	};
};

struct stack { void* ss_sp; int ss_flags; int pad; u64 ss_size; };

struct sigcontext {
	u64 r8, r9, r10, r11, r12, r13, r14, r15;
	u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
	unsigned short cs, gs, fs, ss;
	u64 err, trapno, oldmask, cr2, fpstate, reserved[8];
};

struct ucontext {
	u64 uc_flags;
	u64 uc_link;
	struct stack uc_stack;
	struct sigcontext uc_mcontext;
	u64 uc_sigmask;
};

void restorer(void);
__asm__(".globl restorer\nrestorer:\n\tmov $15, %eax\n\tsyscall\n\thlt\n");

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

static int pid, tid;

static i64 set_handler(int sig, void* handler, u64 flags, u64 mask)
{
	struct k_sigaction act = { handler, flags | SA_RESTORER, restorer, mask };
	return sys4(SYS_rt_sigaction, sig, &act, 0, 8);
}

static i64 raise_sig(int sig) { return sys3(SYS_tgkill, pid, tid, sig); }

static u64 get_mask(void)
{
	u64 mask = 0;
	sys4(SYS_rt_sigprocmask, SIG_BLOCK, 0, &mask, 8);
	return mask;
}

static void set_mask(u64 mask) { sys4(SYS_rt_sigprocmask, SIG_SETMASK, &mask, 0, 8); }


/* --- basic delivery, arguments, frame contents --- */

static volatile int gotSignal, gotCode, gotPid, gotErrno;
static volatile u64 maskInHandler, savedMaskInFrame;
static volatile int infoMatches, ucFlagsSet, frameAligned;

static void info_handler(int sig, struct siginfo* info, struct ucontext* uc)
{
	gotSignal = sig;
	gotCode = info->si_code;
	gotPid = info->rt.pid;
	gotErrno = info->si_errno;
	infoMatches = info->si_signo == sig;
	maskInHandler = get_mask();
	savedMaskInFrame = uc->uc_sigmask;
	ucFlagsSet = uc->uc_mcontext.fpstate != 0
		&& (uc->uc_mcontext.fpstate & 15) == 0;
	/* The frame starts 8 bytes into a 16-byte unit (as if called), so the
	   ucontext right after pretcode is 16-byte aligned. */
	frameAligned = ((u64)uc & 15) == 0;
}


/* --- registers and vector state survive a handler --- */

static volatile int clobbered;

static void clobber_handler(int sig)
{
	(void)sig;
	/* Wreck vector registers and the rounding mode; rt_sigreturn must put
	   them back. */
	unsigned mxcsr = 0x7f80 | 0x6000;		/* all masked, round to zero */
	__asm__ volatile("pcmpeqd %%xmm0, %%xmm0\n\tpcmpeqd %%xmm7, %%xmm7\n\t"
		"pcmpeqd %%xmm15, %%xmm15\n\tldmxcsr %0" : : "m"(mxcsr)
		: "xmm0", "xmm7", "xmm15");
	clobbered = 1;
}

static int vectors_survive(void)
{
	unsigned long a = 0x1122334455667788UL, b = 0x99aabbccddeeff00UL;
	unsigned long outA = 0, outB = 0, outC = 0;
	unsigned mxcsrBefore, mxcsrAfter;
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsrBefore));
	register long rax __asm__("rax") = SYS_tgkill;
	register long rdi __asm__("rdi") = pid;
	register long rsi __asm__("rsi") = tid;
	register long rdx __asm__("rdx") = SIGUSR2;
	register long r12 __asm__("r12") = 0x5a5a5a5a;
	__asm__ volatile(
		"movq %[a], %%xmm0\n\t"
		"movq %[b], %%xmm7\n\t"
		"movq %[a], %%xmm15\n\t"
		"syscall\n\t"
		"movq %%xmm0, %[oa]\n\t"
		"movq %%xmm7, %[ob]\n\t"
		"movq %%xmm15, %[oc]\n\t"
		: "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r12),
		  [oa] "=m"(outA), [ob] "=m"(outB), [oc] "=m"(outC)
		: [a] "r"(a), [b] "r"(b)
		: "rcx", "r11", "memory", "xmm0", "xmm7", "xmm15");
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsrAfter));
	return clobbered && rax == 0 && r12 == 0x5a5a5a5a && outA == a
		&& outB == b && outC == a && mxcsrAfter == mxcsrBefore;
}


/* --- SA_NODEFER, nesting, SA_RESETHAND --- */

static volatile int depth, maxDepth, nestedOrder;

static void nested_inner(int sig)
{
	(void)sig;
	if (depth == 1)
		nestedOrder = 1;
}

static void nested_outer(int sig)
{
	(void)sig;
	depth++;
	if (depth > maxDepth)
		maxDepth = depth;
	if (depth < 3)
		raise_sig(SIGUSR1);		/* with SA_NODEFER: nests */
	if (depth == 1)
		raise_sig(SIGUSR2);		/* a different, unblocked signal */
	depth--;
}

static volatile int resetCount;
static void reset_handler(int sig) { (void)sig; resetCount++; }


/* --- SIGSEGV: skip the faulting instruction --- */

static volatile u64 faultAddress;
static volatile int faultCode, faultRecovered;

static void segv_handler(int sig, struct siginfo* info, struct ucontext* uc)
{
	(void)sig;
	faultAddress = info->fault.addr;
	faultCode = info->si_code;
	/* The fault is "mov 0x10, %eax" (8 bytes: 8b 04 25 10 00 00 00 ... ),
	   see below; skip it and report through rax. */
	uc->uc_mcontext.rip += 7;
	uc->uc_mcontext.rax = 0x600d;
	faultRecovered = uc->uc_mcontext.cr2 == 0x10;
}

static int read_null(void)
{
	int value;
	__asm__ volatile("xor %%eax, %%eax\n\t"
		"mov 0x10, %%eax\n\t"		/* 8b 04 25 10 00 00 00: 7 bytes */
		: "=a"(value) : : "memory");
	return value;
}


/* --- alternate stack --- */

static char altStack[16384] __attribute__((aligned(16)));
static volatile int onAltStack, altFlagsInHandler, altChangeRefused;

static void alt_handler(int sig)
{
	(void)sig;
	char local;
	onAltStack = &local >= altStack && &local < altStack + sizeof(altStack);
	struct stack current;
	sys2(SYS_sigaltstack, 0, &current);
	altFlagsInHandler = current.ss_flags;
	struct stack other = { altStack, 0, 0, sizeof(altStack) };
	altChangeRefused = sys2(SYS_sigaltstack, &other, 0) == -EPERM;
}


/* --- restart --- */

static volatile int alarms;
static void alarm_handler(int sig) { (void)sig; alarms++; }

static i64 wait_child_with_alarm(u64 flags)
{
	set_handler(SIGALRM, alarm_handler, flags, 0);
	i64 child = sys0(SYS_fork);
	if (child == 0) {
		i64 ts[2] = { 0, 300 * 1000000L };
		sys2(SYS_nanosleep, ts, 0);
		quit(0);
	}
	/* SIGALRM in 50 ms, while waiting for the child */
	i64 timer[4] = { 0, 0, 0, 50000 };	/* it_interval, it_value */
	sys3(SYS_setitimer, 0, timer, 0);
	int status;
	i64 r = sys4(SYS_wait4, child, &status, 0, 0);
	if (r == -EINTR)
		r = sys4(SYS_wait4, child, &status, 0, 0) == child ? -EINTR : -1;
	return r == child ? 1 : r == -EINTR ? 2 : 0;
}


/* SIGALRM in 40 ms, while reading a pipe whose writer (a child) writes
   after 200 ms. 1: the read restarted and got the byte; 2: EINTR. */
static int read_pipe_with_alarm(u64 flags)
{
	set_handler(SIGALRM, alarm_handler, flags, 0);
	int fds[2];
	if (sys2(SYS_pipe2, fds, 0) != 0)
		return 0;
	i64 child = sys0(SYS_fork);
	if (child == 0) {
		i64 ts[2] = { 0, 200 * 1000000L };
		sys2(SYS_nanosleep, ts, 0);
		sys3(SYS_write, fds[1], "x", 1);
		quit(0);
	}
	i64 timer[4] = { 0, 0, 0, 40000 };
	sys3(SYS_setitimer, 0, timer, 0);
	char c = 0;
	i64 r = sys3(SYS_read, fds[0], &c, 1);
	int result = r == 1 && c == 'x' ? 1 : r == -EINTR ? 2 : 0;
	int status;
	sys4(SYS_wait4, child, &status, 0, 0);
	sys1(SYS_close, fds[0]);
	sys1(SYS_close, fds[1]);
	return result;
}


static i64 now_us(void)
{
	i64 ts[2];
	sys2(SYS_clock_gettime, 1, ts);
	return ts[0] * 1000000 + ts[1] / 1000;
}


/* --- what kills: forced fault signals, default real-time action --- */

/* Runs fn in a child; returns its wait status. */
static int in_child(void (*fn)(void))
{
	i64 child = sys0(SYS_fork);
	if (child == 0) {
		pid = (int)sys0(SYS_getpid);	/* raise_sig() targets the child */
		tid = (int)sys0(SYS_gettid);
		set_handler(SIGSEGV, (void*)0, 0, 0);	/* not the test's handler */
		fn();
		quit(0);
	}
	int status = 0;
	sys4(SYS_wait4, child, &status, 0, 0);
	return status;
}

static void fault_while_blocked(void)
{
	set_handler(SIGSEGV, (void*)0, 0, 0);
	u64 segv = BIT(SIGSEGV);
	sys4(SYS_rt_sigprocmask, SIG_BLOCK, &segv, 0, 8);
	*(volatile int*)0x20 = 1;
}

static void fault_while_ignored(void)
{
	set_handler(SIGSEGV, (void*)1, 0, 0);
	*(volatile int*)0x20 = 1;
}

static void unhandled_realtime(void)
{
	set_handler(SIGRTMIN + 1, (void*)0, 0, 0);
	raise_sig(SIGRTMIN + 1);
}

static void bad_restorer(void)
{
	struct k_sigaction act = { (void*)reset_handler, 0, 0, 0 };
	sys4(SYS_rt_sigaction, SIGUSR1, &act, 0, 8);	/* no SA_RESTORER */
	raise_sig(SIGUSR1);
}


/* --- the handler's FPU state, and AVX state across a handler --- */

static volatile unsigned handlerMxcsr;
static void mxcsr_handler(int sig)
{
	(void)sig;
	unsigned value;
	__asm__ volatile("stmxcsr %0" : "=m"(value));
	handlerMxcsr = value;
}

static int have_avx(void)
{
	unsigned a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
		: "a"(1), "c"(0));
	if ((c & (1u << 27)) == 0 || (c & (1u << 28)) == 0)
		return 0;		/* no OSXSAVE or no AVX */
	unsigned lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return (lo & 6) == 6;
}

static volatile int vzeroed;
static void vzero_handler(int sig)
{
	(void)sig;
	__asm__ volatile("vzeroupper" ::: "memory");
	vzeroed = 1;
}

/* Sets ymm0 to all ones, raises SIGUSR2 (whose handler runs vzeroupper),
   and checks the upper half of ymm0 afterwards. */
static int ymm_survives(void)
{
	unsigned long upper = 0;
	register long rax __asm__("rax") = SYS_tgkill;
	register long rdi __asm__("rdi") = pid;
	register long rsi __asm__("rsi") = tid;
	register long rdx __asm__("rdx") = SIGUSR2;
	__asm__ volatile(
		"vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\t"
		"syscall\n\t"
		"vextractf128 $1, %%ymm0, %%xmm1\n\t"
		"movq %%xmm1, %[upper]\n\t"
		"vzeroupper\n\t"
		: "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), [upper] "=m"(upper)
		:
		: "rcx", "r11", "memory", "xmm0", "xmm1");
	return vzeroed && upper == ~0UL;
}

static volatile int sentPid, sentCode;
static void segv_info_handler(int sig, struct siginfo* info, void* uc)
{
	(void)sig; (void)uc;
	sentPid = info->rt.pid;
	sentCode = info->si_code;
}


/* --- a signal for one thread --- */

#define CLONE_THREAD_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000 \
	| 0x40000 | 0x100000 | 0x200000)
long raw_clone(long flags, void* stackTop, int* parentTid, int* childTid,
	void* tls, int (*fn)(void*), void* arg);
__asm__(
	".globl raw_clone\nraw_clone:\n\tsub $16, %rsi\n\tmov %r9, (%rsi)\n"
	"\tmov 8(%rsp), %rax\n\tmov %rax, 8(%rsi)\n\tmov %rcx, %r10\n"
	"\tmov $56, %eax\n\tsyscall\n\ttest %rax, %rax\n\tjnz 1f\n"
	"\txor %ebp, %ebp\n\tpop %rax\n\tpop %rdi\n\tcall *%rax\n"
	"\tmov %eax, %edi\n\tmov $60, %eax\n\tsyscall\n\thlt\n1:\tret\n");

static volatile int threadSignalTid, threadReady, threadStop;
static char threadStack[65536] __attribute__((aligned(16)));

static void thread_signal_handler(int sig)
{
	(void)sig;
	threadSignalTid = (int)sys0(SYS_gettid);
}

static int signal_thread(void* arg)
{
	(void)arg;
	threadReady = 1;
	while (!threadStop) {
		i64 ts[2] = { 0, 1000000 };
		sys2(SYS_nanosleep, ts, 0);
	}
	return 0;
}


int check_main(int argc, char** argv)
{
	(void)argc; (void)argv;
	pid = (int)sys0(SYS_getpid);
	tid = (int)sys0(SYS_gettid);

	/* 1: delivery with SA_SIGINFO */
	check(set_handler(SIGUSR1, info_handler, SA_SIGINFO, BIT(SIGUSR2)) == 0,
		1, "rt_sigaction");
	u64 maskBefore = get_mask();
	check(raise_sig(SIGUSR1) == 0, 2, "tgkill");
	check(gotSignal == SIGUSR1 && infoMatches, 3, "handler got the signal");
	check(gotCode == SI_TKILL && gotPid == pid, 4, "si_code SI_TKILL, si_pid");
	check(gotErrno == 0, 5, "si_errno 0");
	check((maskInHandler & BIT(SIGUSR1)) && (maskInHandler & BIT(SIGUSR2)), 6,
		"signal and sa_mask blocked in the handler");
	check(savedMaskInFrame == maskBefore, 7, "uc_sigmask is the old mask");
	check(get_mask() == maskBefore, 8, "mask restored after the handler");
	check(ucFlagsSet, 9, "fpstate present and aligned");
	check(frameAligned, 10, "frame alignment");

	/* reading the action back */
	struct k_sigaction old;
	sys4(SYS_rt_sigaction, SIGUSR1, 0, &old, 8);
	check(old.handler == (void*)info_handler && old.restorer == restorer
		&& (old.flags & SA_SIGINFO) && old.mask == BIT(SIGUSR2), 11,
		"rt_sigaction reads back");
	check(sys4(SYS_rt_sigaction, 9, &old, 0, 8) == -EINVAL, 12,
		"SIGKILL can't be caught");
	check(sys4(SYS_rt_sigaction, SIGUSR1, 0, &old, 4) == -EINVAL, 13,
		"bad sigsetsize");

	/* 2: registers, vector registers and MXCSR survive */
	set_handler(SIGUSR2, clobber_handler, 0, 0);
	check(vectors_survive(), 20, "registers/XMM/MXCSR restored");

	/* 3: blocked and pending */
	gotSignal = 0;
	u64 block = BIT(SIGUSR1);
	sys4(SYS_rt_sigprocmask, SIG_BLOCK, &block, 0, 8);
	raise_sig(SIGUSR1);
	check(gotSignal == 0, 30, "blocked signal not delivered");
	u64 pending = 0;
	sys2(SYS_rt_sigpending, &pending, 8);
	check(pending & BIT(SIGUSR1), 31, "rt_sigpending");
	sys4(SYS_rt_sigprocmask, SIG_UNBLOCK, &block, 0, 8);
	check(gotSignal == SIGUSR1, 32, "delivered on unblock");
	/* a signal Haiku has no number for still round-trips in the mask */
	u64 rtmax = BIT(64);
	sys4(SYS_rt_sigprocmask, SIG_BLOCK, &rtmax, 0, 8);
	check((get_mask() & BIT(64)) != 0, 33, "SIGRTMAX stays blocked");
	sys4(SYS_rt_sigprocmask, SIG_UNBLOCK, &rtmax, 0, 8);
	check(!(get_mask() & BIT(64)), 34, "SIGRTMAX unblocked");

	/* 4: SA_NODEFER nesting, a second signal inside a handler */
	set_handler(SIGUSR1, nested_outer, SA_NODEFER, 0);
	set_handler(SIGUSR2, nested_inner, 0, 0);
	raise_sig(SIGUSR1);
	check(maxDepth == 3, 40, "SA_NODEFER lets the handler nest");
	check(nestedOrder == 1, 41, "nested handler of another signal");

	/* SA_RESETHAND */
	set_handler(SIGUSR2, reset_handler, SA_RESETHAND, 0);
	raise_sig(SIGUSR2);
	sys4(SYS_rt_sigaction, SIGUSR2, 0, &old, 8);
	check(resetCount == 1 && old.handler == 0, 42, "SA_RESETHAND");
	set_handler(SIGUSR2, (void*)1, 0, 0);		/* SIG_IGN */
	raise_sig(SIGUSR2);
	check(resetCount == 1, 43, "ignored signal");

	/* 5: SIGSEGV handler fixes up the context */
	set_handler(SIGSEGV, segv_handler, SA_SIGINFO, 0);
	check(read_null() == 0x600d, 50, "returned past the faulting load");
	check(faultAddress == 0x10 && faultCode == SEGV_MAPERR, 51,
		"SIGSEGV si_addr and si_code");
	check(faultRecovered, 52, "sigcontext cr2");

	/* 6: alternate stack */
	struct stack ss = { altStack, 0, 0, sizeof(altStack) };
	check(sys2(SYS_sigaltstack, &ss, 0) == 0, 60, "sigaltstack");
	struct stack small = { altStack, 0, 0, 1024 };
	check(sys2(SYS_sigaltstack, &small, 0) == -12, 61, "too small: ENOMEM");
	set_handler(SIGUSR1, alt_handler, SA_ONSTACK, 0);
	raise_sig(SIGUSR1);
	check(onAltStack, 62, "handler ran on the alternate stack");
	check(altFlagsInHandler == SS_ONSTACK, 63, "SS_ONSTACK in the handler");
	check(altChangeRefused, 64, "can't change it while on it");
	struct stack now;
	sys2(SYS_sigaltstack, 0, &now);
	check(now.ss_flags == 0 && now.ss_sp == altStack, 65,
		"still set up afterwards");
	struct stack off = { 0, SS_DISABLE, 0, 0 };
	sys2(SYS_sigaltstack, &off, 0);
	sys2(SYS_sigaltstack, 0, &now);
	check(now.ss_flags == SS_DISABLE, 66, "disabled");

	/* 7: restart */
	alarms = 0;
	check(wait_child_with_alarm(SA_RESTART) == 1 && alarms == 1, 70,
		"SA_RESTART: wait4 restarted");
	alarms = 0;
	check(wait_child_with_alarm(0) == 2 && alarms == 1, 71,
		"no SA_RESTART: wait4 fails with EINTR");
	alarms = 0;
	check(read_pipe_with_alarm(SA_RESTART) == 1 && alarms == 1, 72,
		"SA_RESTART: pipe read restarted");
	alarms = 0;
	check(read_pipe_with_alarm(0) == 2 && alarms == 1, 73,
		"no SA_RESTART: pipe read fails with EINTR");
	/* nanosleep: EINTR after a handler even with SA_RESTART ... */
	set_handler(SIGALRM, alarm_handler, SA_RESTART, 0);
	i64 shortTimer[4] = { 0, 0, 0, 20000 };
	sys3(SYS_setitimer, 0, shortTimer, 0);
	i64 longSleep[2] = { 2, 0 }, left[2] = { 0, 0 };
	check(sys2(SYS_nanosleep, longSleep, left) == -EINTR
		&& left[0] >= 1, 74, "nanosleep: EINTR and time left");
	/* ... but a signal without a handler doesn't cut it short */
	set_handler(SIGCHLD, (void*)0, 0, 0);
	i64 start = now_us();
	i64 child = sys0(SYS_fork);
	if (child == 0)
		quit(0);
	i64 sleep100[2] = { 0, 100 * 1000000L };
	check(sys2(SYS_nanosleep, sleep100, 0) == 0 && now_us() - start >= 95000,
		75, "nanosleep continues across an unhandled SIGCHLD");
	int childStatus;
	sys4(SYS_wait4, child, &childStatus, 0, 0);

	/* 8: sigsuspend */
	set_handler(SIGUSR1, info_handler, SA_SIGINFO, 0);
	sys4(SYS_rt_sigprocmask, SIG_BLOCK, &block, 0, 8);
	gotSignal = 0;
	raise_sig(SIGUSR1);
	u64 empty = 0;
	check(sys2(SYS_rt_sigsuspend, &empty, 8) == -EINTR && gotSignal == SIGUSR1,
		80, "rt_sigsuspend");
	check(get_mask() & BIT(SIGUSR1), 81, "mask back after sigsuspend");

	/* 9: sigtimedwait */
	u64 waitSet = BIT(SIGUSR1);
	struct siginfo info;
	gotSignal = 0;
	raise_sig(SIGUSR1);
	check(sys4(SYS_rt_sigtimedwait, &waitSet, &info, 0, 8) == SIGUSR1
		&& info.si_signo == SIGUSR1 && gotSignal == 0, 90,
		"rt_sigtimedwait takes a pending signal");
	i64 shortWait[2] = { 0, 10000000 };
	check(sys4(SYS_rt_sigtimedwait, &waitSet, &info, shortWait, 8) == -EAGAIN,
		91, "rt_sigtimedwait timeout");

	/* 10: sigqueueinfo with a value, to a real-time signal */
	set_handler(SIGRTMIN, info_handler, SA_SIGINFO, 0);
	struct siginfo qinfo;
	for (int i = 0; i < (int)sizeof(qinfo); i++)
		((char*)&qinfo)[i] = 0;
	qinfo.si_signo = SIGRTMIN;
	qinfo.si_code = SI_QUEUE;
	qinfo.rt.value = 1234;
	gotSignal = 0;
	check(sys3(SYS_rt_sigqueueinfo, pid, SIGRTMIN, &qinfo) == 0
		&& gotSignal == SIGRTMIN && gotCode == SI_QUEUE, 100,
		"rt_sigqueueinfo");
	set_mask(maskBefore);

	/* 11: interval timers and alarm */
	i64 timer[4] = { 0, 0, 2, 0 };
	i64 current[4] = { -1, -1, -1, -1 };
	check(sys3(SYS_setitimer, 0, timer, 0) == 0, 110, "setitimer");
	sys2(SYS_getitimer, 0, current);
	check(current[2] <= 2 && (current[2] >= 1 || current[3] > 0)
		&& current[0] == 0 && current[1] == 0, 111, "getitimer");
	i64 zero[4] = { 0, 0, 0, 0 };
	sys3(SYS_setitimer, 0, zero, current);
	check(current[2] == 1 || current[2] == 2, 112, "old value");
	check(sys1(SYS_alarm, 5) == 0 && sys1(SYS_alarm, 0) == 5, 113, "alarm");
	check(sys3(SYS_setitimer, 7, timer, 0) == -EINVAL, 114, "bad timer");

	/* 12a: what kills */
	int status = in_child(fault_while_blocked);
	check((status & 0x7f) == SIGSEGV, 130, "fault with SIGSEGV blocked kills");
	status = in_child(fault_while_ignored);
	check((status & 0x7f) == SIGSEGV, 131, "fault with SIGSEGV ignored kills");
	status = in_child(unhandled_realtime);
	check((status & 0x7f) == SIGRTMIN + 1, 132,
		"unhandled real-time signal kills");
	status = in_child(bad_restorer);
	check((status & 0x7f) == SIGSEGV, 133, "no SA_RESTORER: SIGSEGV");

	/* 12b: handlers start with the initial MXCSR; AVX state survives */
	set_handler(SIGUSR2, mxcsr_handler, 0, 0);
	unsigned savedMxcsr, roundUp;
	__asm__ volatile("stmxcsr %0" : "=m"(savedMxcsr));
	roundUp = (savedMxcsr & ~0x6000u) | 0x4000u;
	__asm__ volatile("ldmxcsr %0" : : "m"(roundUp));
	raise_sig(SIGUSR2);
	unsigned afterMxcsr;
	__asm__ volatile("stmxcsr %0" : "=m"(afterMxcsr));
	__asm__ volatile("ldmxcsr %0" : : "m"(savedMxcsr));
	check(handlerMxcsr == 0x1f80, 134, "handler starts with the default MXCSR");
	check(afterMxcsr == roundUp, 135, "interrupted MXCSR restored");
	if (have_avx()) {
		set_handler(SIGUSR2, vzero_handler, 0, 0);
		check(ymm_survives(), 136, "YMM upper halves survive a handler");
	}

	/* 12c: a fault signal sent by a process carries the sender */
	set_handler(SIGSEGV, segv_info_handler, SA_SIGINFO, 0);
	raise_sig(SIGSEGV);
	check(sentPid == pid && sentCode == SI_TKILL, 137,
		"tgkill'd SIGSEGV has si_pid");
	struct k_sigaction killAction;
	check(sys4(SYS_rt_sigaction, 9, 0, &killAction, 8) == 0
		&& killAction.handler == 0, 138, "reading SIGKILL's action");

	/* 12: a signal for one thread */
	set_handler(SIGUSR2, thread_signal_handler, 0, 0);
	static volatile int ctid;
	i64 t = raw_clone(CLONE_THREAD_FLAGS, threadStack + sizeof(threadStack),
		(int*)&ctid, (int*)&ctid, 0, signal_thread, 0);
	check(t > 0, 120, "start thread");
	if (t > 0) {
		while (!threadReady)
			;
		sys3(SYS_tgkill, pid, (int)t, SIGUSR2);
		for (int i = 0; i < 2000 && threadSignalTid == 0; i++) {
			i64 ts[2] = { 0, 1000000 };
			sys2(SYS_nanosleep, ts, 0);
		}
		check(threadSignalTid == (int)t, 121, "handler ran in the thread");
		threadStop = 1;
		while (ctid != 0)
			sys6(SYS_futex, (i64)&ctid, 0, ctid, 0, 0, 0);
	}

	if (failed)
		return failed;
	put("ok\n");
	return 0;
}
