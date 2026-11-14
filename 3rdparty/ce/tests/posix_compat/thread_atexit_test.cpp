/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for __cxa_thread_atexit_impl() (libroot), the C library
	hook behind C++ thread_local destructors.

	Haiku's own libstdc++ was built without the hook and keeps using its
	pthread-key emulation; libc++abi (Chromium's bundled C++ runtime, LLVM
	toolchains) and a libstdc++ rebuilt against Renga call it. So the test
	calls it directly, the way those runtimes do, with the registering image's
	__dso_handle (or a symbol inside the image) as the third argument.

	Output format: see 3rdparty/ce/README.md.
*/


#include <OS.h>
#include <image.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "thread_atexit"


extern "C" int __cxa_thread_atexit_impl(void (*function)(void*),
	void* object, void* dsoSymbol);
extern "C" void* __dso_handle;


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


// #pragma mark - order log


static char sLog[64];
static int sLogLength = 0;


static void
log_char(void* object)
{
	if (sLogLength < (int)sizeof(sLog) - 1)
		sLog[sLogLength++] = (char)(addr_t)object;
	sLog[sLogLength] = '\0';
}


static void
reset_log()
{
	sLogLength = 0;
	sLog[0] = '\0';
}


static pthread_key_t sKey;


static void
key_destructor(void*)
{
	log_char((void*)'K');
}


// #pragma mark - threads


static void*
register_abc_and_return(void*)
{
	pthread_setspecific(sKey, (void*)1);
	__cxa_thread_atexit_impl(log_char, (void*)'a', &__dso_handle);
	__cxa_thread_atexit_impl(log_char, (void*)'b', &__dso_handle);
	__cxa_thread_atexit_impl(log_char, (void*)'c', &__dso_handle);
	return NULL;
}


static void*
register_and_pthread_exit(void*)
{
	__cxa_thread_atexit_impl(log_char, (void*)'x', &__dso_handle);
	__cxa_thread_atexit_impl(log_char, (void*)'y', &__dso_handle);
	pthread_exit(NULL);
	return NULL;
}


static void
nested_destructor(void* object)
{
	log_char(object);
	// A destructor that touches another thread_local registers it while the
	// destructors run; it must still run, right after this one.
	if ((char)(addr_t)object == '2')
		__cxa_thread_atexit_impl(log_char, (void*)'n', &__dso_handle);
}


static void*
register_nested(void*)
{
	__cxa_thread_atexit_impl(nested_destructor, (void*)'1', &__dso_handle);
	__cxa_thread_atexit_impl(nested_destructor, (void*)'2', &__dso_handle);
	return NULL;
}


static status_t
spawned_thread(void*)
{
	__cxa_thread_atexit_impl(log_char, (void*)'s', &__dso_handle);
	__cxa_thread_atexit_impl(log_char, (void*)'t', &__dso_handle);
	return B_OK;
}


static void*
register_without_dso(void*)
{
	__cxa_thread_atexit_impl(log_char, (void*)'z', NULL);
	return NULL;
}


// #pragma mark - dlclose


static const char* kLibrary = "libgnu.so";


static bool
library_loaded()
{
	void* handle = dlopen(kLibrary, RTLD_LAZY | RTLD_NOLOAD);
	if (handle == NULL)
		return false;
	dlclose(handle);
	return true;
}


struct DlcloseState {
	bool	loadedBefore;
	bool	opened;
	bool	loadedAfterClose;
	int		registerResult;
};


static void*
register_from_library(void* data)
{
	DlcloseState* state = (DlcloseState*)data;
	state->loadedBefore = library_loaded();

	void* handle = dlopen(kLibrary, RTLD_LAZY);
	state->opened = handle != NULL;
	if (handle == NULL)
		return NULL;

	// Any address inside the image identifies it, as __dso_handle would.
	// (timerfd_create: eventfd moved to libroot, so dlsym() on libgnu
	// finds libroot's and the test would pin the wrong image)
	void* symbol = dlsym(handle, "timerfd_create");
	state->registerResult = __cxa_thread_atexit_impl(log_char, (void*)'L',
		symbol != NULL ? symbol : handle);

	dlclose(handle);
	state->loadedAfterClose = library_loaded();
	return NULL;
}


// #pragma mark - exit()


static int sExitPipe = -1;


static void
write_char(void* object)
{
	char c = (char)(addr_t)object;
	write(sExitPipe, &c, 1);
}


static void
atexit_handler()
{
	char c = 'E';
	write(sExitPipe, &c, 1);
}


static void
run_exit_child(bool viaExit)
{
	// Order wanted: thread_local destructors of the exiting thread first
	// (reverse order), then the atexit() handlers.
	atexit(atexit_handler);
	__cxa_thread_atexit_impl(write_char, (void*)'1', &__dso_handle);
	__cxa_thread_atexit_impl(write_char, (void*)'2', &__dso_handle);
	__cxa_thread_atexit_impl(write_char, (void*)'3', &__dso_handle);
	if (viaExit)
		exit(0);
	// otherwise return from main()
}


static void
test_exit(const char* name, bool viaExit)
{
	int fds[2];
	if (pipe(fds) != 0) {
		report(name, false, "pipe: %s", strerror(errno));
		return;
	}

	pid_t child = fork();
	if (child == 0) {
		close(fds[0]);
		sExitPipe = fds[1];
		run_exit_child(viaExit);
		return;
			// "return from main()": main() sees sExitPipe and returns 0
	}
	close(fds[1]);

	char buffer[16] = {};
	ssize_t total = 0;
	while (total < (ssize_t)sizeof(buffer) - 1) {
		ssize_t bytes = read(fds[0], buffer + total,
			sizeof(buffer) - 1 - total);
		if (bytes <= 0)
			break;
		total += bytes;
	}
	close(fds[0]);

	int status = 0;
	waitpid(child, &status, 0);
	report(name, strcmp(buffer, "321E") == 0 && WIFEXITED(status)
			&& WEXITSTATUS(status) == 0,
		"order \"%s\" (want \"321E\"), exit status %d", buffer,
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}


// #pragma mark -


static void
run_pthread(const char* name, void* (*function)(void*), void* argument,
	const char* expected)
{
	reset_log();
	pthread_t thread;
	if (pthread_create(&thread, NULL, function, argument) != 0) {
		report(name, false, "pthread_create failed");
		return;
	}
	pthread_join(thread, NULL);
	report(name, strcmp(sLog, expected) == 0, "order \"%s\" (want \"%s\")",
		sLog, expected);
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	pthread_key_create(&sKey, key_destructor);

	run_pthread("return_reverse_order", register_abc_and_return, NULL,
		"cbaK");
	run_pthread("pthread_exit", register_and_pthread_exit, NULL, "yx");
	run_pthread("nested_registration", register_nested, NULL, "2n1");
	run_pthread("null_dso", register_without_dso, NULL, "z");

	// spawn_thread() threads exit through the same path.
	reset_log();
	thread_id spawned = spawn_thread(spawned_thread, "thread_atexit spawned",
		B_NORMAL_PRIORITY, NULL);
	status_t returnValue;
	resume_thread(spawned);
	wait_for_thread(spawned, &returnValue);
	report("spawn_thread", strcmp(sLog, "ts") == 0,
		"order \"%s\" (want \"ts\")", sLog);

	// A library dlclose()d while one of its thread_local destructors is
	// pending stays loaded until the destructor has run.
	reset_log();
	DlcloseState state = {};
	pthread_t thread;
	pthread_create(&thread, NULL, register_from_library, &state);
	pthread_join(thread, NULL);
	bool loadedAfterExit = library_loaded();
	if (state.loadedBefore) {
		report("dlclose_pinned", true,
			"INFO %s was already loaded; pinning not observable", kLibrary);
	} else {
		report("dlclose_pinned", state.opened && state.registerResult == 0
				&& state.loadedAfterClose,
			"opened %d, register %d, loaded after dlclose %d (want 1)",
			state.opened, state.registerResult, state.loadedAfterClose);
		report("dlclose_released", !loadedAfterExit
				&& strcmp(sLog, "L") == 0,
			"loaded after thread exit %d (want 0), destructor log \"%s\"",
			loadedAfterExit, sLog);
	}

	// The main thread: exit() and returning from main().
	test_exit("exit_main_thread", true);
	test_exit("return_from_main", false);
	if (sExitPipe >= 0) {
		// forked child of the return_from_main case
		return 0;
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
