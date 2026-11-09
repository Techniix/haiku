/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for getcontext()/setcontext()/makecontext()/
	swapcontext() (<ucontext.h>, libroot). These are what coroutine code
	(QEMU's coroutines, libtask, older green-thread runtimes) is built on.
	Output format: see 3rdparty/ce/README.md.
*/


#include <ucontext.h>

#include <OS.h>

#include <errno.h>
#include <fenv.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "ucontext"


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


static const size_t kStackSize = 64 * 1024;


static void
test_getcontext_setcontext()
{
	// The classic: getcontext() returns twice, the second time through
	// setcontext().
	volatile int passes = 0;
	ucontext_t context;
	int result = getcontext(&context);
	passes++;
	if (passes == 1)
		setcontext(&context);
	report("get_set_returns_twice", result == 0 && passes == 2,
		"getcontext %d, passes %d", result, passes);
}


// #pragma mark - arguments, uc_link


static ucontext_t sMain, sWorker;
static long sArguments[9];
static void* sPointerArgument;
static bool sWorkerAligned;


static void
worker_with_arguments(int a, int b, int c, int d, int e, int f, int g, int h,
	void* pointer)
{
	sArguments[0] = a; sArguments[1] = b; sArguments[2] = c;
	sArguments[3] = d; sArguments[4] = e; sArguments[5] = f;
	sArguments[6] = g; sArguments[7] = h;
	sPointerArgument = pointer;

	// The ABI requires 16-byte stack alignment; a misaligned stack shows
	// up in a 16-byte aligned local (and crashes SSE code in real life).
	alignas(16) volatile char probe[16];
	sWorkerAligned = ((uintptr_t)probe & 15) == 0;
	// returning resumes uc_link (sMain)
}


static void
test_arguments_and_link()
{
	void* stack = malloc(kStackSize);
	getcontext(&sWorker);
	sWorker.uc_stack.ss_sp = stack;
	sWorker.uc_stack.ss_size = kStackSize;
	sWorker.uc_link = &sMain;
	makecontext(&sWorker, (void (*)(void))worker_with_arguments, 9,
		11, 22, 33, 44, 55, 66, 77, 88, (void*)&sMain);

	int result = swapcontext(&sMain, &sWorker);
	bool argumentsOk = true;
	for (int i = 0; i < 8; i++)
		argumentsOk = argumentsOk && sArguments[i] == (i + 1) * 11;
	report("arguments_and_uc_link", result == 0 && argumentsOk
			&& sPointerArgument == &sMain,
		"swap %d, args %ld %ld %ld %ld %ld %ld %ld %ld, pointer ok %d",
		result, sArguments[0], sArguments[1], sArguments[2], sArguments[3],
		sArguments[4], sArguments[5], sArguments[6], sArguments[7],
		sPointerArgument == &sMain);
	report("stack_aligned", sWorkerAligned, "16-byte aligned locals: %d",
		sWorkerAligned);
	free(stack);
}


// #pragma mark - generator ping-pong


static ucontext_t sCaller, sGenerator;
static volatile long sYielded;


static void
generator(int count)
{
	for (long i = 1; i <= count; i++) {
		sYielded = i * i;
		swapcontext(&sGenerator, &sCaller);
	}
	sYielded = -1;
}


static void
test_generator()
{
	const int kCount = 10000;
	void* stack = malloc(kStackSize);
	getcontext(&sGenerator);
	sGenerator.uc_stack.ss_sp = stack;
	sGenerator.uc_stack.ss_size = kStackSize;
	sGenerator.uc_link = &sCaller;
	makecontext(&sGenerator, (void (*)(void))generator, 1, kCount);

	bool sequenceOk = true;
	long received = 0;
	bigtime_t start = system_time();
	while (true) {
		swapcontext(&sCaller, &sGenerator);
		if (sYielded == -1)
			break;
		received++;
		if (sYielded != received * received)
			sequenceOk = false;
	}
	bigtime_t elapsed = system_time() - start;
	report("generator_ping_pong", received == kCount && sequenceOk,
		"%ld values, in order %d, %lld us for %d switches", received,
		sequenceOk, (long long)elapsed, 2 * kCount);
	free(stack);
}


// #pragma mark - signal mask and floating point state


static ucontext_t sOuter, sInner;
static bool sInnerBlocked;
static int sInnerRounding;


static void
mask_and_fpu_worker()
{
	sigset_t current;
	sigprocmask(SIG_BLOCK, NULL, &current);
	sInnerBlocked = sigismember(&current, SIGUSR1) == 1;

	fesetround(FE_DOWNWARD);
	swapcontext(&sInner, &sOuter);

	// Back in: our rounding mode must be ours again.
	sInnerRounding = fegetround();
}


static void
test_mask_and_fpu()
{
	sigset_t empty;
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);
	fesetround(FE_TONEAREST);

	void* stack = malloc(kStackSize);
	getcontext(&sInner);
	sInner.uc_stack.ss_sp = stack;
	sInner.uc_stack.ss_size = kStackSize;
	sInner.uc_link = &sOuter;
	sigemptyset(&sInner.uc_sigmask);
	sigaddset(&sInner.uc_sigmask, SIGUSR1);
	makecontext(&sInner, mask_and_fpu_worker, 0);

	swapcontext(&sOuter, &sInner);

	sigset_t current;
	sigprocmask(SIG_BLOCK, NULL, &current);
	bool outerBlocked = sigismember(&current, SIGUSR1) == 1;
	int outerRounding = fegetround();
	report("signal_mask_per_context", sInnerBlocked && !outerBlocked,
		"blocked inside %d, outside %d", sInnerBlocked, outerBlocked);

	swapcontext(&sOuter, &sInner);
	report("fpu_control_per_context", outerRounding == FE_TONEAREST
			&& sInnerRounding == FE_DOWNWARD
			&& fegetround() == FE_TONEAREST,
		"outer %#x (expected %#x), inner %#x (expected %#x)",
		outerRounding, FE_TONEAREST, sInnerRounding, FE_DOWNWARD);
	free(stack);
}


// #pragma mark - uc_link NULL exits


static void
last_context()
{
	// returns with uc_link == NULL: the process exits with status 0
}


static void
test_null_link_exits()
{
	pid_t child = fork();
	if (child == 0) {
		static ucontext_t self, final;
		void* stack = malloc(kStackSize);
		getcontext(&final);
		final.uc_stack.ss_sp = stack;
		final.uc_stack.ss_size = kStackSize;
		final.uc_link = NULL;
		makecontext(&final, last_context, 0);
		swapcontext(&self, &final);
		_exit(5);	// must not be reached
	}
	int status = -1;
	waitpid(child, &status, 0);
	report("null_uc_link_exits", WIFEXITED(status)
			&& WEXITSTATUS(status) == 0,
		"child status 0x%x", status);
}


// #pragma mark - many coroutines


static const int kCoroutines = 64;
static ucontext_t sScheduler;
static ucontext_t sCoroutines[kCoroutines];
static long sCounters[kCoroutines];


static void
counting_coroutine(int index, int rounds)
{
	for (int i = 0; i < rounds; i++) {
		sCounters[index]++;
		swapcontext(&sCoroutines[index], &sScheduler);
	}
}


static void
test_many()
{
	const int kRounds = 200;
	void* stacks = malloc((size_t)kCoroutines * kStackSize);
	for (int i = 0; i < kCoroutines; i++) {
		getcontext(&sCoroutines[i]);
		sCoroutines[i].uc_stack.ss_sp = (char*)stacks + i * kStackSize;
		sCoroutines[i].uc_stack.ss_size = kStackSize;
		sCoroutines[i].uc_link = &sScheduler;
		makecontext(&sCoroutines[i], (void (*)(void))counting_coroutine, 2,
			i, kRounds);
	}
	// Round-robin until every coroutine has finished its rounds (one more
	// switch each lets it return through uc_link).
	for (int round = 0; round <= kRounds; round++) {
		for (int i = 0; i < kCoroutines; i++)
			swapcontext(&sScheduler, &sCoroutines[i]);
	}
	bool allDone = true;
	for (int i = 0; i < kCoroutines; i++)
		allDone = allDone && sCounters[i] == kRounds;
	report("round_robin_64", allDone, "%d coroutines x %d rounds",
		kCoroutines, kRounds);
	free(stacks);
}


int
main()
{
	test_getcontext_setcontext();
	test_arguments_and_link();
	test_generator();
	test_mask_and_fpu();
	test_null_link_exits();
	test_many();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
