/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Shared futexes between processes under Renguin (static glibc program).
 * One line per part that worked, then "ok".
 *
 *  futex      a child's FUTEX_WAIT (no FUTEX_PRIVATE_FLAG) on a word in a
 *             MAP_SHARED anonymous mapping is woken by the parent's
 *             FUTEX_WAKE, which reports one woken waiter
 *  mutex      a PTHREAD_PROCESS_SHARED mutex and condition variable in
 *             shared memory, parent and child taking turns 200 times
 *  semaphore  sem_init(pshared) in shared memory: the child blocks in
 *             sem_wait() until the parent posts
 *  shm        FUTEX_WAIT/FUTEX_WAKE on a word in a System V segment that
 *             parent and child attached separately (at different addresses)
 *  memfd      the same on a memfd both map MAP_SHARED
 */

#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf(" (errno %d %s)\n", errno, strerror(errno)); \
			exit(1); \
		} \
	} while (0)


static long
futex(volatile uint32_t* word, int op, uint32_t value)
{
	return syscall(SYS_futex, word, op, value, NULL, NULL, 0);
}


//! A child waits on *word (shared futex) until it is 1; the parent sets it
//! and wakes it. \a childWord is the child's address of the same word.
static void
wait_and_wake(volatile uint32_t* word, volatile uint32_t* childWord,
	const char* what)
{
	*word = 0;
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		while (*childWord == 0) {
			if (futex(childWord, FUTEX_WAIT, 0) != 0 && errno != EAGAIN
				&& errno != EINTR) {
				_exit(2);
			}
		}
		_exit(0);
	}
	// wait until the child sleeps in the kernel (a wake that comes too
	// early would only be a missed check, not a hang: the child re-checks)
	int woken = 0;
	for (int i = 0; i < 200 && woken == 0; i++) {
		usleep(10000);
		if (i == 50)
			*word = 1;
		if (i >= 50)
			woken = (int)futex(word, FUTEX_WAKE, 1);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "%s: child status %#x", what, status);
	CHECK(woken == 1, "%s: FUTEX_WAKE woke %d waiters", what, woken);
}


static void
test_futex(void)
{
	volatile uint32_t* word = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(word != MAP_FAILED, "mmap");
	wait_and_wake(word, word, "anonymous");
	munmap((void*)word, 4096);
	printf("futex\n");
}


struct shared_state {
	pthread_mutex_t	mutex;
	pthread_cond_t	condition;
	int				turn;
	sem_t			semaphore;
};


static void
test_mutex(struct shared_state* state)
{
	pthread_mutexattr_t mutexAttributes;
	pthread_mutexattr_init(&mutexAttributes);
	pthread_mutexattr_setpshared(&mutexAttributes, PTHREAD_PROCESS_SHARED);
	CHECK(pthread_mutex_init(&state->mutex, &mutexAttributes) == 0,
		"mutex init");
	pthread_condattr_t conditionAttributes;
	pthread_condattr_init(&conditionAttributes);
	pthread_condattr_setpshared(&conditionAttributes, PTHREAD_PROCESS_SHARED);
	CHECK(pthread_cond_init(&state->condition, &conditionAttributes) == 0,
		"cond init");
	state->turn = 0;

	const int kRounds = 200;
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	int me = child == 0 ? 1 : 0;
	for (int i = 0; i < kRounds; i++) {
		pthread_mutex_lock(&state->mutex);
		while (state->turn % 2 != me)
			pthread_cond_wait(&state->condition, &state->mutex);
		state->turn++;
		pthread_cond_broadcast(&state->condition);
		pthread_mutex_unlock(&state->mutex);
	}
	if (child == 0)
		_exit(0);
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "child status %#x", status);
	CHECK(state->turn == 2 * kRounds, "%d turns", state->turn);
	printf("mutex\n");
}


static void
test_semaphore(struct shared_state* state)
{
	CHECK(sem_init(&state->semaphore, 1, 0) == 0, "sem_init");
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0)
		_exit(sem_wait(&state->semaphore) == 0 ? 0 : 2);
	usleep(100000);
	CHECK(sem_post(&state->semaphore) == 0, "sem_post");
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "child status %#x", status);
	sem_destroy(&state->semaphore);
	printf("semaphore\n");
}


static void
test_shm(void)
{
	int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	CHECK(id >= 0, "shmget");
	volatile uint32_t* word = shmat(id, NULL, 0);
	CHECK(word != (void*)-1, "shmat");
	// a second attachment in the same process stands for the child's own
	// (another address, the same memory)
	volatile uint32_t* other = shmat(id, NULL, 0);
	CHECK(other != (void*)-1 && other != word, "second shmat");
	wait_and_wake(word, other, "shm");
	shmdt((void*)word);
	shmdt((void*)other);
	shmctl(id, IPC_RMID, NULL);
	printf("shm\n");
}


static void
test_memfd(void)
{
	int fd = memfd_create("renguin-futex", 0);
	CHECK(fd >= 0, "memfd_create");
	CHECK(ftruncate(fd, 4096) == 0, "ftruncate");
	volatile uint32_t* word = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	volatile uint32_t* other = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	CHECK(word != MAP_FAILED && other != MAP_FAILED, "mmap memfd");
	wait_and_wake(word, other, "memfd");
	munmap((void*)word, 4096);
	munmap((void*)other, 4096);
	close(fd);
	printf("memfd\n");
}


int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_futex();
	struct shared_state* state = mmap(NULL, sizeof(struct shared_state),
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(state != MAP_FAILED, "mmap");
	test_mutex(state);
	test_semaphore(state);
	test_shm();
	test_memfd();
	printf("ok\n");
	return 0;
}
