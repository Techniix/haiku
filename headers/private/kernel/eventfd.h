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


#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_EVENTFD_H
