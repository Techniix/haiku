/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * __cxa_thread_atexit_impl(): destructors of C++ thread_local objects.
 *
 * The C++ runtime (libstdc++'s or libc++abi's __cxa_thread_atexit()) calls
 * this the first time a thread uses a thread_local object with a non-trivial
 * destructor. glibc, musl, FreeBSD's and the other C libraries provide it;
 * both C++ runtimes use it when it exists (libc++abi looks it up as a weak
 * symbol at run time) and otherwise fall back to an emulation built on
 * pthread keys, which runs nothing for the main thread at exit() and whose
 * order relative to other key destructors is unspecified.
 *
 * Semantics, as in glibc (stdlib/cxa_thread_atexit_impl.c):
 *
 *  - Destructors are kept per thread and run in reverse order of
 *    registration when the thread exits: returning from its entry function,
 *    pthread_exit(), exit_thread(), or, for the thread calling exit(),
 *    from exit() before the atexit()/__cxa_atexit() handlers and static
 *    destructors run.
 *  - A destructor may itself use (and so register) further thread_local
 *    objects; those run too, before the thread goes on to its pthread key
 *    destructors.
 *  - The image that registered a destructor (found through dso_symbol, its
 *    __dso_handle) is kept loaded until the destructor has run: dlclose()
 *    of a library while some thread still holds one of its thread_local
 *    objects would otherwise unmap the destructor's code. glibc keeps a
 *    counter in the link map; Haiku's runtime_loader has no such counter,
 *    so a reference to the image is taken with the same call as
 *    dlopen(path, RTLD_NOLOAD) and dropped with dlclose() after the
 *    destructor ran. The program image and libraries can be pinned this
 *    way; add-ons loaded with load_add_on() cannot (the runtime_loader only
 *    finds libraries by name), so their destructors run unpinned, as with
 *    the pthread-key emulation.
 *
 * The list lives in the thread's pthread_thread structure, which libroot
 * allocates for every thread (spawn_thread() ones included) and keeps
 * statically for the main thread.
 */


#include <stdlib.h>

#include <dlfcn.h>

#include <libroot_private.h>
#include <pthread_private.h>
#include <runtime_loader.h>


struct __cxa_thread_dtor {
	struct __cxa_thread_dtor*	next;
	void						(*function)(void*);
	void*						object;
	void*						image;
		// runtime_loader handle keeping the owning image loaded, or NULL
};


extern "C" int
__cxa_thread_atexit_impl(void (*function)(void*), void* object,
	void* dsoSymbol)
{
	__cxa_thread_dtor* entry
		= (__cxa_thread_dtor*)malloc(sizeof(__cxa_thread_dtor));
	if (entry == NULL)
		return -1;
			// glibc aborts here; both C++ runtimes treat non-zero as
			// failure and std::terminate() for us.

	entry->function = function;
	entry->object = object;
	entry->image = NULL;

	if (dsoSymbol != NULL) {
		// Find the image the symbol lies in and take a reference to it.
		image_id id;
		char* path = NULL;
		if (__gRuntimeLoader->get_nearest_symbol_at_address(dsoSymbol, &id,
				&path, NULL, NULL, NULL, NULL, NULL) == B_OK && path != NULL) {
			void* handle;
			if (__gRuntimeLoader->load_library(path, RTLD_LAZY | RTLD_NOLOAD,
					dsoSymbol, &handle) >= 0) {
				entry->image = handle;
			}
		}
	}

	pthread_thread* thread = pthread_self();
	entry->next = thread->thread_dtors;
	thread->thread_dtors = entry;
	return 0;
}


/*!	Runs (and removes) the calling thread's thread_local destructors, the most
	recently registered first, including ones registered while this runs.
*/
extern "C" void
__cxa_thread_call_dtors(void)
{
	pthread_thread* thread = pthread_self();

	while (thread->thread_dtors != NULL) {
		__cxa_thread_dtor* entry = thread->thread_dtors;
		thread->thread_dtors = entry->next;

		entry->function(entry->object);

		if (entry->image != NULL)
			__gRuntimeLoader->unload_library(entry->image);
		free(entry);
	}
}
