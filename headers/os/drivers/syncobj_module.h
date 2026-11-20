/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYNCOBJ_MODULE_H
#define _SYNCOBJ_MODULE_H


/*!	Renga's vendor-neutral GPU synchronisation core (Quasar R-18).

	It implements Linux's DRM sync objects (DRM_IOCTL_SYNCOBJ_*, layouts in
	<drm/drm_syncobj.h>) and sync_file descriptors (<linux/sync_file.h>), and
	gives GPU drivers a kernel API to create and signal the fences behind
	them.

	Three users:
	 - GPU drivers. A render node forwards the DRM_IOCTL_SYNCOBJ_* requests
	   it receives to ioctl() below, with one syncobj_file per open
	   descriptor of the node (open_file() in the node's open hook,
	   close_file() in its free hook). Handles are per syncobj_file, as on
	   Linux, so Mesa and libdrm work on the GPU's own descriptor. The
	   driver's submit path looks up in-fences with find_fence() and
	   attaches its out-fences with replace_fence().
	 - Programs without a GPU descriptor (the compositor, tests) open
	   /dev/syncobj, which is such a node with nothing else behind it.
	 - Linux programs under Renguin: their DRM_IOCTL_SYNCOBJ_* and
	   SYNC_IOC_* requests are passed through unchanged.

	Fences. A syncobj_fence is reference counted and signals once, with
	B_OK or an error. User space signals through the ioctls (CPU-signalled
	points, such as the compositor's release points); drivers call
	signal_fence(). Timeline points are kept as Linux's fence chains: point
	N is signalled once its own fence and every earlier point have
	signalled.

	Context rules:
	 - Every function may block briefly on the module's lock and must be
	   called from thread context (a driver's interrupt handler hands the
	   signal to a DPC or a thread). None may be called from a callback.
	 - Fence callbacks run once, in the thread that signals the fence, with
	   the module's lock held: they must not block and must not call into
	   this module. Use them to release a semaphore or schedule a DPC.

	The module is "generic/syncobj/v1". The interface is add-only: new
	functions are appended, and existing ones keep their meaning.
*/


#include <module.h>
#include <OS.h>


#ifdef __cplusplus
extern "C" {
#endif


#define B_SYNCOBJ_MODULE_NAME	"generic/syncobj/v1"


typedef struct syncobj_fence syncobj_fence;
typedef struct syncobj_file syncobj_file;
typedef struct syncobj_fence_callback syncobj_fence_callback;

typedef void (*syncobj_fence_func)(syncobj_fence* fence,
	syncobj_fence_callback* callback);

/*!	Caller-owned callback record; add_fence_callback() links it to a fence.
	\c data is the caller's. The private fields belong to the module. */
struct syncobj_fence_callback {
	syncobj_fence_callback*	_next;
	syncobj_fence_func		func;
	void*					data;
};

/* find_fence() flags (the DRM wait flags with the same meaning) */
#define B_SYNCOBJ_FIND_WAIT_FOR_SUBMIT	0x02
	/* block until a fence exists for the point */

/* wait_fence() flags */
#define B_SYNCOBJ_WAIT_INTERRUPTIBLE	0x01
	/* signals interrupt the wait (B_INTERRUPTED) */


typedef struct syncobj_module_info {
	module_info		info;

	/* Render-node forwarding. Each open descriptor of a GPU node gets a
	   syncobj_file with its own handle table. */
	status_t		(*open_file)(syncobj_file** _file);
	void			(*close_file)(syncobj_file* file);
		/* drops all handles; objects shared through descriptors or
		   other files live on */
	bool			(*is_syncobj_ioctl)(uint32 op);
		/* DRM_IOCTL_SYNCOBJ_* (any size the program was built with) */
	status_t		(*ioctl)(syncobj_file* file, uint32 op, void* buffer,
						size_t length);
		/* \a buffer is the caller's (user or kernel) address, as the
		   node's control hook got it; \a length is ignored (the size is
		   in \a op, as in Linux's drm_ioctl()). Returns B_OK or an error
		   such as B_BAD_VALUE (EINVAL), ETIME, B_ENTRY_NOT_FOUND
		   (ENOENT) or B_BAD_ADDRESS. It also answers the sw_sync CPU
		   timeline requests (SW_SYNC_IOC_*) that /dev/syncobj offers;
		   render nodes forward only what is_syncobj_ioctl() accepts. */

	/* Fences */
	syncobj_fence*	(*create_fence)(const char* driverName,
						const char* timelineName);
		/* one reference, unsignalled; names (up to 31 characters) are
		   shown by SYNC_IOC_FILE_INFO */
	void			(*acquire_fence)(syncobj_fence* fence);
	void			(*release_fence)(syncobj_fence* fence);
	status_t		(*signal_fence)(syncobj_fence* fence, status_t error);
		/* B_OK, or an error the waiters see as the fence's status;
		   B_NOT_ALLOWED if it had signalled already */
	status_t		(*fence_status)(syncobj_fence* fence);
		/* 1 signalled, 0 pending, < 0 the error it signalled with */
	status_t		(*wait_fence)(syncobj_fence* fence, uint32 flags,
						bigtime_t absoluteTimeout);
		/* B_OK once signalled (whatever its error), B_TIMED_OUT,
		   B_INTERRUPTED; B_INFINITE_TIMEOUT waits for ever, 0 polls */
	status_t		(*add_fence_callback)(syncobj_fence* fence,
						syncobj_fence_callback* callback,
						syncobj_fence_func func, void* data);
		/* B_NOT_ALLOWED (and nothing is called) if it has signalled */
	bool			(*remove_fence_callback)(syncobj_fence* fence,
						syncobj_fence_callback* callback);
		/* true if it was still linked and will not be called */

	/* Sync objects of a file */
	status_t		(*find_fence)(syncobj_file* file, uint32 handle,
						uint64 point, uint32 flags, bigtime_t absoluteTimeout,
						syncobj_fence** _fence);
		/* point 0: the binary fence; returns a reference. Without a fence
		   for the point: B_BAD_VALUE, or with WAIT_FOR_SUBMIT a wait */
	status_t		(*replace_fence)(syncobj_file* file, uint32 handle,
						uint64 point, syncobj_fence* fence);
		/* point 0: replaces the binary fence (NULL resets it); else adds
		   timeline point \a point. Takes its own reference. */

	/* sync_file descriptors of the calling team */
	status_t		(*get_sync_file_fence)(int fd, syncobj_fence** _fence);
		/* the fence of sync_file \a fd, with a reference */
	int				(*create_sync_file)(syncobj_fence* fence,
						const char* name, bool closeOnExec);
		/* a new sync_file descriptor, or an error */
} syncobj_module_info;


#ifdef __cplusplus
}
#endif


#endif	/* _SYNCOBJ_MODULE_H */
