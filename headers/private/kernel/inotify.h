/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_INOTIFY_H
#define _KERNEL_INOTIFY_H


#include <OS.h>


/* Events the VFS reports to inotify (same values as Linux's IN_*, which
   kernel/events/inotify.cpp asserts). */
#define INOTIFY_ACCESS			0x00000001
#define INOTIFY_MODIFY			0x00000002
#define INOTIFY_ATTRIB			0x00000004
#define INOTIFY_CLOSE_WRITE		0x00000008
#define INOTIFY_CLOSE_NOWRITE	0x00000010
#define INOTIFY_OPEN			0x00000020


#ifdef __cplusplus
extern "C" {
#endif


/* Number of active inotify watches; the VFS hooks do nothing while 0. */
extern int32	gInotifyWatchCount;

extern void		inotify_notify_node(dev_t device, ino_t node, bool isDirectory,
					uint32 events);

/*!	Called by the VFS for reads, writes, attribute changes, opens and closes
	of vnodes (see vfs.cpp); cheap when nothing is watched. */
static inline void
inotify_node_event(dev_t device, ino_t node, bool isDirectory, uint32 events)
{
	if (atomic_get(&gInotifyWatchCount) > 0)
		inotify_notify_node(device, node, isDirectory, events);
}

extern int		_user_inotify_init(int openFlags);
extern int		_user_inotify_add_watch(int fd, const char* path, uint32 mask);
extern status_t	_user_inotify_rm_watch(int fd, int wd);


#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_INOTIFY_H
