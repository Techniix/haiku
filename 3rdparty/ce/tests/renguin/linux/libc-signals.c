/*
 * Signals through the C library (glibc or musl): signal(), sigaction() with
 * SA_SIGINFO, sigqueue() values, siglongjmp() out of a SIGSEGV handler,
 * setitimer() + pause(), sigwait() fed by pthread_kill(), a handler running
 * in a chosen thread, pthread_cancel() of a thread blocked in pause(), and
 * an alternate signal stack. Prints one line per part, then "ok".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#define CHECK(cond, what) do { \
	if (!(cond)) { \
		printf("FAIL: %s (line %d, errno %d)\n", what, __LINE__, errno); \
		exit(1); \
	} \
} while (0)

static volatile sig_atomic_t usr1Count;
static void usr1_handler(int sig) { (void)sig; usr1Count++; }

static volatile int queuedValue, queuedCode;
static void rt_handler(int sig, siginfo_t* info, void* context)
{
	(void)sig; (void)context;
	queuedValue = info->si_value.sival_int;
	queuedCode = info->si_code;
}

static sigjmp_buf segvJump;
static volatile void* segvAddress;
static void segv_handler(int sig, siginfo_t* info, void* context)
{
	(void)sig; (void)context;
	segvAddress = info->si_addr;
	siglongjmp(segvJump, 1);
}

static volatile sig_atomic_t alarmCount;
static void alarm_handler(int sig) { (void)sig; alarmCount++; }

static pthread_t mainThread;
static void* sigwait_sender(void* arg)
{
	(void)arg;
	usleep(20000);
	pthread_kill(mainThread, SIGUSR2);
	return NULL;
}

static volatile pthread_t handledIn;
static void which_thread_handler(int sig) { (void)sig; handledIn = pthread_self(); }
static volatile int workerReady;
static void* worker(void* arg)
{
	(void)arg;
	workerReady = 1;
	while (handledIn == 0)
		usleep(1000);
	return NULL;
}

static volatile int cancelReady;
static void* cancellable(void* arg)
{
	(void)arg;
	cancelReady = 1;
	for (;;)
		pause();		// a cancellation point
	return NULL;
}

static char altStack[65536];
static volatile int onAltStack;
static void alt_handler(int sig)
{
	(void)sig;
	char local;
	onAltStack = &local >= altStack && &local < altStack + sizeof(altStack);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	// signal() and raise()
	CHECK(signal(SIGUSR1, usr1_handler) != SIG_ERR, "signal");
	raise(SIGUSR1);
	raise(SIGUSR1);
	CHECK(usr1Count == 2, "raise");
	printf("raise %d\n", (int)usr1Count);

	// sigqueue() with a value, SA_SIGINFO
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = rt_handler;
	action.sa_flags = SA_SIGINFO;
	CHECK(sigaction(SIGRTMIN, &action, NULL) == 0, "sigaction SIGRTMIN");
	union sigval value = { .sival_int = 4242 };
	CHECK(sigqueue(getpid(), SIGRTMIN, value) == 0, "sigqueue");
	CHECK(queuedValue == 4242 && queuedCode == SI_QUEUE, "sigqueue value");
	printf("sigqueue %d\n", queuedValue);

	// siglongjmp() out of a SIGSEGV handler restores the mask
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = segv_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &action, NULL);
	if (sigsetjmp(segvJump, 1) == 0) {
		volatile int* bad = (volatile int*)0x40;
		*bad = 1;
		CHECK(0, "no SIGSEGV");
	}
	sigset_t current;
	sigprocmask(SIG_BLOCK, NULL, &current);
	CHECK(segvAddress == (void*)0x40 && !sigismember(&current, SIGSEGV),
		"SIGSEGV + siglongjmp");
	printf("segv %p\n", (void*)segvAddress);

	// setitimer() + pause()
	signal(SIGALRM, alarm_handler);
	struct itimerval timer = { { 0, 0 }, { 0, 20000 } };
	setitimer(ITIMER_REAL, &timer, NULL);
	errno = 0;
	CHECK(pause() == -1 && errno == EINTR && alarmCount == 1, "pause");
	printf("pause EINTR\n");

	// sigwait() for a signal another thread sends
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	mainThread = pthread_self();
	pthread_t sender;
	pthread_create(&sender, NULL, sigwait_sender, NULL);
	int got = 0;
	CHECK(sigwait(&set, &got) == 0 && got == SIGUSR2, "sigwait");
	pthread_join(sender, NULL);
	printf("sigwait %d\n", got);

	// pthread_kill(): the handler runs in that thread
	signal(SIGUSR1, which_thread_handler);
	pthread_t worker1;
	pthread_create(&worker1, NULL, worker, NULL);
	while (!workerReady)
		usleep(1000);
	pthread_kill(worker1, SIGUSR1);
	pthread_join(worker1, NULL);
	CHECK(pthread_equal(handledIn, worker1), "pthread_kill");
	printf("pthread_kill\n");

	// pthread_cancel() of a thread blocked in pause()
	pthread_t victim;
	pthread_create(&victim, NULL, cancellable, NULL);
	while (!cancelReady)
		usleep(1000);
	usleep(10000);
	CHECK(pthread_cancel(victim) == 0, "pthread_cancel");
	void* result = NULL;
	pthread_join(victim, &result);
	CHECK(result == PTHREAD_CANCELED, "cancelled thread's result");
	printf("cancel\n");

	// an alternate signal stack
	stack_t ss = { .ss_sp = altStack, .ss_size = sizeof(altStack) };
	CHECK(sigaltstack(&ss, NULL) == 0, "sigaltstack");
	memset(&action, 0, sizeof(action));
	action.sa_handler = alt_handler;
	action.sa_flags = SA_ONSTACK;
	sigaction(SIGUSR1, &action, NULL);
	raise(SIGUSR1);
	CHECK(onAltStack, "SA_ONSTACK");
	printf("altstack\n");

	printf("ok\n");
	return 0;
}
