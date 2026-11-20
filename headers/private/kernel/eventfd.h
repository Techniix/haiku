/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_EVENTFD_H
#define _KERNEL_EVENTFD_H


#include <OS.h>


#ifdef __cplusplus
extern "C" {
#endif


extern int		_user_eventfd(uint32 initialValue, int openFlags, bool semaphore);

struct file_descriptor;

/* Kernel API (Renga, 2026-11-20): for kernel objects that signal an
   eventfd a program handed them, such as SYNCOBJ_EVENTFD. */
extern bool		eventfd_is_descriptor(struct file_descriptor* descriptor);
extern status_t	eventfd_signal_descriptor(struct file_descriptor* descriptor,
					uint64 value);


#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_EVENTFD_H
