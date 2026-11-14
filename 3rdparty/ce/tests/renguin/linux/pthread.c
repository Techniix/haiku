/*
 * POSIX threads through the C library (glibc or musl): create/join, thread
 * locals, mutexes, condition variables with and without timeouts, once,
 * semaphores, barriers, detached threads and robust mutexes. Exercises
 * clone()/clone3(), futex(), set_tid_address() and set_robust_list() the way
 * real programs do. Prints one line per part, then "ok".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, what) do { \
	if (!(cond)) { \
		printf("FAIL: %s (line %d, errno %d)\n", what, __LINE__, errno); \
		exit(1); \
	} \
} while (0)

#define THREADS 8
#define LOOPS 20000

static __thread int tlsValue = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void* adder(void* arg)
{
	tlsValue = (int)(long)arg + 100;
	for (int i = 0; i < LOOPS; i++) {
		pthread_mutex_lock(&lock);
		counter++;
		pthread_mutex_unlock(&lock);
	}
	// thread-local storage is per thread
	return (void*)(long)(tlsValue == (int)(long)arg + 100
		&& (pid_t)syscall(SYS_gettid) != getpid());
}


// Bounded queue: producer/consumer with two condition variables.
#define QUEUE 4
#define ITEMS 5000
static int queue[QUEUE];
static int head, tail, count;
static pthread_mutex_t queueLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notEmpty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t notFull = PTHREAD_COND_INITIALIZER;

static void* producer(void* arg)
{
	(void)arg;
	for (int i = 1; i <= ITEMS; i++) {
		pthread_mutex_lock(&queueLock);
		while (count == QUEUE)
			pthread_cond_wait(&notFull, &queueLock);
		queue[tail] = i;
		tail = (tail + 1) % QUEUE;
		count++;
		pthread_cond_signal(&notEmpty);
		pthread_mutex_unlock(&queueLock);
	}
	return NULL;
}

static void* consumer(void* arg)
{
	long sum = 0;
	(void)arg;
	for (int i = 0; i < ITEMS; i++) {
		pthread_mutex_lock(&queueLock);
		while (count == 0)
			pthread_cond_wait(&notEmpty, &queueLock);
		sum += queue[head];
		head = (head + 1) % QUEUE;
		count--;
		pthread_cond_signal(&notFull);
		pthread_mutex_unlock(&queueLock);
	}
	return (void*)sum;
}


static pthread_once_t once = PTHREAD_ONCE_INIT;
static int onceCount;
static void once_init(void) { onceCount++; }
static void* once_thread(void* arg)
{
	(void)arg;
	pthread_once(&once, once_init);
	return NULL;
}


static sem_t sem;
static void* sem_poster(void* arg)
{
	(void)arg;
	for (int i = 0; i < 100; i++)
		sem_post(&sem);
	return NULL;
}


static pthread_barrier_t barrier;
static int serialCount;
static pthread_mutex_t serialLock = PTHREAD_MUTEX_INITIALIZER;
static void* barrier_thread(void* arg)
{
	(void)arg;
	for (int round = 0; round < 10; round++) {
		if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
			pthread_mutex_lock(&serialLock);
			serialCount++;
			pthread_mutex_unlock(&serialLock);
		}
	}
	return NULL;
}


static sem_t detachedDone;
static void* detached_thread(void* arg)
{
	(void)arg;
	sem_post(&detachedDone);
	return NULL;
}


static pthread_mutex_t robust;
static void* robust_owner(void* arg)
{
	(void)arg;
	pthread_mutex_lock(&robust);
	return NULL;	// dies holding the lock
}


int main(void)
{
	pthread_t threads[THREADS];

	// create, join, mutex, thread-local storage
	for (long i = 0; i < THREADS; i++)
		CHECK(pthread_create(&threads[i], NULL, adder, (void*)i) == 0, "create");
	for (int i = 0; i < THREADS; i++) {
		void* result;
		CHECK(pthread_join(threads[i], &result) == 0, "join");
		CHECK(result == (void*)1L, "thread-local value / gettid in thread");
	}
	CHECK(counter == (long)THREADS * LOOPS, "mutex-protected counter");
	CHECK(tlsValue == 1, "main thread's thread-local value");
	printf("mutex %ld\n", counter);

	// condition variables
	pthread_t prod, cons;
	CHECK(pthread_create(&cons, NULL, consumer, NULL) == 0, "create consumer");
	CHECK(pthread_create(&prod, NULL, producer, NULL) == 0, "create producer");
	void* sum;
	pthread_join(prod, NULL);
	pthread_join(cons, &sum);
	CHECK((long)sum == (long)ITEMS * (ITEMS + 1) / 2, "producer/consumer sum");
	printf("condvar %ld\n", (long)sum);

	// timed wait that times out
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 30 * 1000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&queueLock);
	int status = pthread_cond_timedwait(&notEmpty, &queueLock, &deadline);
	pthread_mutex_unlock(&queueLock);
	CHECK(status == ETIMEDOUT, "cond_timedwait timeout");
	printf("timedwait ETIMEDOUT\n");

	// once
	for (int i = 0; i < THREADS; i++)
		pthread_create(&threads[i], NULL, once_thread, NULL);
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);
	CHECK(onceCount == 1, "pthread_once");
	printf("once %d\n", onceCount);

	// semaphores
	sem_init(&sem, 0, 0);
	pthread_t poster;
	pthread_create(&poster, NULL, sem_poster, NULL);
	for (int i = 0; i < 100; i++)
		CHECK(sem_wait(&sem) == 0, "sem_wait");
	pthread_join(poster, NULL);
	CHECK(sem_trywait(&sem) == -1 && errno == EAGAIN, "sem_trywait empty");
	printf("semaphore 100\n");

	// barriers
	pthread_barrier_init(&barrier, NULL, 4);
	for (int i = 0; i < 4; i++)
		pthread_create(&threads[i], NULL, barrier_thread, NULL);
	for (int i = 0; i < 4; i++)
		pthread_join(threads[i], NULL);
	CHECK(serialCount == 10, "barrier serial thread once per round");
	printf("barrier %d\n", serialCount);

	// detached thread
	sem_init(&detachedDone, 0, 0);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t detached;
	CHECK(pthread_create(&detached, &attr, detached_thread, NULL) == 0,
		"create detached");
	CHECK(sem_wait(&detachedDone) == 0, "detached thread ran");
	printf("detached\n");

	// robust mutex: the next locker learns the owner died
	pthread_mutexattr_t mattr;
	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
	pthread_mutex_init(&robust, &mattr);
	pthread_t owner;
	pthread_create(&owner, NULL, robust_owner, NULL);
	pthread_join(owner, NULL);
	status = pthread_mutex_lock(&robust);
	CHECK(status == EOWNERDEAD, "robust mutex EOWNERDEAD");
	pthread_mutex_consistent(&robust);
	pthread_mutex_unlock(&robust);
	printf("robust EOWNERDEAD\n");

	printf("ok\n");
	return 0;
}
