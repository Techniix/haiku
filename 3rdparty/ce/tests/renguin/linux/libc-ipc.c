/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * POSIX timers and System V IPC under Renguin (static glibc program).
 * One line per part that worked, then "ok"; the first failure prints what
 * went wrong and exits with status 1.
 *
 *  timer_signal   timer_create(SIGEV_SIGNAL) with a value, a periodic
 *                 timer, timer_gettime while armed, the signal's si_code
 *                 SI_TIMER and si_value, timer_getoverrun, disarming
 *  timer_thread   SIGEV_THREAD (glibc's helper thread, SIGEV_THREAD_ID in
 *                 the kernel) calls the function with the value
 *  timer_abstime  TIMER_ABSTIME on CLOCK_MONOTONIC; timer_delete, and an
 *                 unknown timer ID is EINVAL
 *  shm            shmget/shmat in parent and fork()ed child see the same
 *                 page; IPC_STAT (size, attach count); shmdt; IPC_RMID
 *  sem            SETVAL/GETVAL, SETALL/GETALL, IPC_STAT (nsems), a child
 *                 blocking in semop() until the parent posts, semtimedop()
 *                 timing out with EAGAIN, IPC_NOWAIT, SEM_UNDO undone at exit
 *  msg            msgsnd/msgrcv between parent and child by type, IPC_STAT
 *                 (qnum), IPC_NOWAIT ENOMSG, MSG_NOERROR truncation, E2BIG
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf(" (errno %d %s)\n", errno, strerror(errno)); \
			exit(1); \
		} \
	} while (0)


static volatile sig_atomic_t sTimerSignals;
static volatile int sTimerCode;
static volatile long sTimerValue;


static void
timer_handler(int signal, siginfo_t* info, void* context)
{
	(void)signal;
	(void)context;
	sTimerSignals++;
	sTimerCode = info->si_code;
	sTimerValue = (long)info->si_value.sival_ptr;
}


static void
test_timer_signal(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = timer_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	CHECK(sigaction(SIGRTMIN + 2, &action, NULL) == 0, "sigaction");

	struct sigevent event;
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_SIGNAL;
	event.sigev_signo = SIGRTMIN + 2;
	event.sigev_value.sival_ptr = (void*)0x5eed;
	timer_t timer;
	CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0, "timer_create");

	struct itimerspec spec = { { 0, 20000000 }, { 0, 20000000 } };
	CHECK(timer_settime(timer, 0, &spec, NULL) == 0, "timer_settime");
	struct itimerspec now;
	CHECK(timer_gettime(timer, &now) == 0, "timer_gettime");
	CHECK(now.it_interval.tv_nsec == 20000000 && (now.it_value.tv_sec != 0
			|| now.it_value.tv_nsec != 0),
		"armed timer reads %ld.%09ld interval %ld", (long)now.it_value.tv_sec,
		now.it_value.tv_nsec, now.it_interval.tv_nsec);

	for (int i = 0; i < 300 && sTimerSignals < 3; i++)
		usleep(10000);
	CHECK(sTimerSignals >= 3, "%d timer signals in 3 s", (int)sTimerSignals);
	CHECK(sTimerCode == SI_TIMER, "si_code %d", sTimerCode);
	CHECK(sTimerValue == 0x5eed, "si_value %#lx", sTimerValue);
	CHECK(timer_getoverrun(timer) >= 0, "timer_getoverrun");

	// disarm: no more signals, gettime reads zero
	struct itimerspec zero;
	memset(&zero, 0, sizeof(zero));
	struct itimerspec old;
	CHECK(timer_settime(timer, 0, &zero, &old) == 0, "disarm");
	CHECK(old.it_interval.tv_nsec == 20000000, "old interval %ld",
		old.it_interval.tv_nsec);
	usleep(30000);
	int count = sTimerSignals;
	usleep(100000);
	CHECK(sTimerSignals == count, "signals after disarming");
	CHECK(timer_gettime(timer, &now) == 0 && now.it_value.tv_sec == 0
		&& now.it_value.tv_nsec == 0, "disarmed timer reads armed");
	CHECK(timer_delete(timer) == 0, "timer_delete");
	printf("timer_signal\n");
}


static pthread_mutex_t sThreadLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sThreadCondition = PTHREAD_COND_INITIALIZER;
static int sThreadCalls;
static long sThreadValue;


static void
timer_function(union sigval value)
{
	pthread_mutex_lock(&sThreadLock);
	sThreadCalls++;
	sThreadValue = value.sival_int;
	pthread_cond_signal(&sThreadCondition);
	pthread_mutex_unlock(&sThreadLock);
}


static void
test_timer_thread(void)
{
	struct sigevent event;
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_THREAD;
	event.sigev_notify_function = timer_function;
	event.sigev_value.sival_int = 77;
	timer_t timer;
	CHECK(timer_create(CLOCK_REALTIME, &event, &timer) == 0,
		"timer_create SIGEV_THREAD");
	struct itimerspec spec = { { 0, 0 }, { 0, 30000000 } };
	CHECK(timer_settime(timer, 0, &spec, NULL) == 0, "timer_settime");

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	pthread_mutex_lock(&sThreadLock);
	while (sThreadCalls == 0) {
		if (pthread_cond_timedwait(&sThreadCondition, &sThreadLock,
				&deadline) != 0) {
			break;
		}
	}
	int calls = sThreadCalls;
	pthread_mutex_unlock(&sThreadLock);
	CHECK(calls == 1, "%d SIGEV_THREAD calls", calls);
	CHECK(sThreadValue == 77, "value %ld", sThreadValue);
	CHECK(timer_delete(timer) == 0, "timer_delete");
	printf("timer_thread\n");
}


static void
test_timer_abstime(void)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGRTMIN + 3);
	sigprocmask(SIG_BLOCK, &set, NULL);

	struct sigevent event;
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_SIGNAL;
	event.sigev_signo = SIGRTMIN + 3;
	timer_t timer;
	CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0, "timer_create");

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct itimerspec spec;
	memset(&spec, 0, sizeof(spec));
	spec.it_value = start;
	spec.it_value.tv_nsec += 50000000;
	if (spec.it_value.tv_nsec >= 1000000000) {
		spec.it_value.tv_sec++;
		spec.it_value.tv_nsec -= 1000000000;
	}
	CHECK(timer_settime(timer, TIMER_ABSTIME, &spec, NULL) == 0,
		"timer_settime TIMER_ABSTIME");
	siginfo_t info;
	struct timespec wait = { 5, 0 };
	CHECK(sigtimedwait(&set, &info, &wait) == SIGRTMIN + 3, "sigtimedwait");
	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	long elapsed = (end.tv_sec - start.tv_sec) * 1000
		+ (end.tv_nsec - start.tv_nsec) / 1000000;
	CHECK(elapsed >= 45, "fired after %ld ms", elapsed);
	CHECK(info.si_code == SI_TIMER, "si_code %d", info.si_code);
	CHECK(timer_delete(timer) == 0, "timer_delete");
	errno = 0;
	CHECK(timer_delete(timer) == -1 && errno == EINVAL,
		"deleting twice");
	CHECK(timer_create(99, &event, &timer) == -1 && errno == EINVAL,
		"unknown clock");
	printf("timer_abstime\n");
}


static void
test_shm(void)
{
	int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
	CHECK(id >= 0, "shmget");
	char* memory = shmat(id, NULL, 0);
	CHECK(memory != (void*)-1, "shmat");
	strcpy(memory, "parent");

	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		char* other = shmat(id, NULL, 0);
		if (other == (void*)-1 || strcmp(other, "parent") != 0)
			_exit(2);
		strcpy(other + 4096, "child");
		shmdt(other);
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "child status %#x", status);
	CHECK(strcmp(memory + 4096, "child") == 0, "child's write not seen");

	struct shmid_ds info;
	CHECK(shmctl(id, IPC_STAT, &info) == 0, "IPC_STAT");
	CHECK(info.shm_segsz == 8192, "segsz %zu", (size_t)info.shm_segsz);
	CHECK(info.shm_nattch == 1, "nattch %lu", (unsigned long)info.shm_nattch);
	CHECK((info.shm_perm.mode & 0777) == 0600, "mode %o",
		(unsigned)info.shm_perm.mode);
	CHECK(shmdt(memory) == 0, "shmdt");
	CHECK(shmctl(id, IPC_RMID, NULL) == 0, "IPC_RMID");
	errno = 0;
	CHECK(shmat(id, NULL, 0) == (void*)-1 && errno == EINVAL,
		"attach after IPC_RMID");
	printf("shm\n");
}


static void
test_sem(void)
{
	int id = semget(IPC_PRIVATE, 3, IPC_CREAT | 0600);
	CHECK(id >= 0, "semget");
	CHECK(semctl(id, 1, SETVAL, 5) == 0, "SETVAL");
	CHECK(semctl(id, 1, GETVAL) == 5, "GETVAL");
	unsigned short values[3] = { 0, 2, 9 };
	CHECK(semctl(id, 0, SETALL, values) == 0, "SETALL");
	unsigned short read[3] = { 0, 0, 0 };
	CHECK(semctl(id, 0, GETALL, read) == 0 && read[1] == 2 && read[2] == 9,
		"GETALL %u %u %u", read[0], read[1], read[2]);
	struct semid_ds info;
	CHECK(semctl(id, 0, IPC_STAT, &info) == 0 && info.sem_nsems == 3,
		"IPC_STAT nsems %lu", (unsigned long)info.sem_nsems);

	// a child waits on semaphore 0 until the parent posts it
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		struct sembuf wait = { 0, -1, 0 };
		_exit(semop(id, &wait, 1) == 0 ? 0 : 2);
	}
	usleep(100000);
	CHECK(semctl(id, 0, GETNCNT) == 1, "GETNCNT %d",
		semctl(id, 0, GETNCNT));
	struct sembuf post = { 0, 1, 0 };
	CHECK(semop(id, &post, 1) == 0, "semop post");
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "child status %#x", status);

	// semtimedop times out
	struct sembuf take = { 0, -1, 0 };
	struct timespec timeout = { 0, 100000000 };
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	errno = 0;
	CHECK(semtimedop(id, &take, 1, &timeout) == -1 && errno == EAGAIN,
		"semtimedop timeout");
	clock_gettime(CLOCK_MONOTONIC, &end);
	long elapsed = (end.tv_sec - start.tv_sec) * 1000
		+ (end.tv_nsec - start.tv_nsec) / 1000000;
	CHECK(elapsed >= 90, "timed out after %ld ms", elapsed);

	struct sembuf noWait = { 0, -1, IPC_NOWAIT };
	errno = 0;
	CHECK(semop(id, &noWait, 1) == -1 && errno == EAGAIN, "IPC_NOWAIT");

	// SEM_UNDO: the child's +3 is undone when it exits
	child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		struct sembuf up = { 2, 3, SEM_UNDO };
		_exit(semop(id, &up, 1) == 0 && semctl(id, 2, GETVAL) == 12 ? 0 : 2);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "SEM_UNDO child status %#x", status);
	CHECK(semctl(id, 2, GETVAL) == 9, "after SEM_UNDO: %d",
		semctl(id, 2, GETVAL));
	CHECK(semctl(id, 0, IPC_RMID) == 0, "IPC_RMID");
	printf("sem\n");
}


struct message {
	long type;
	char text[32];
};


static void
test_msg(void)
{
	int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
	CHECK(id >= 0, "msgget");
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		struct message first = { 2, "second type" };
		struct message second = { 1, "first type" };
		if (msgsnd(id, &first, strlen(first.text) + 1, 0) != 0
			|| msgsnd(id, &second, strlen(second.text) + 1, 0) != 0) {
			_exit(2);
		}
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "child status %#x", status);

	struct msqid_ds info;
	CHECK(msgctl(id, IPC_STAT, &info) == 0 && info.msg_qnum == 2,
		"IPC_STAT qnum %lu", (unsigned long)info.msg_qnum);
	struct message got;
	CHECK(msgrcv(id, &got, sizeof(got.text), 1, 0) == 11
		&& got.type == 1 && strcmp(got.text, "first type") == 0,
		"msgrcv type 1: %ld %s", got.type, got.text);
	errno = 0;
	CHECK(msgrcv(id, &got, 4, 2, IPC_NOWAIT) == -1 && errno == E2BIG,
		"short buffer without MSG_NOERROR");
	CHECK(msgrcv(id, &got, 4, 2, MSG_NOERROR) == 4
		&& memcmp(got.text, "seco", 4) == 0, "MSG_NOERROR");
	errno = 0;
	CHECK(msgrcv(id, &got, sizeof(got.text), 0, IPC_NOWAIT) == -1
		&& errno == ENOMSG, "empty queue IPC_NOWAIT");
	CHECK(msgctl(id, IPC_RMID, NULL) == 0, "IPC_RMID");
	printf("msg\n");
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_timer_signal();
	test_timer_thread();
	test_timer_abstime();
	test_shm();
	test_sem();
	test_msg();
	printf("ok\n");
	return 0;
}
