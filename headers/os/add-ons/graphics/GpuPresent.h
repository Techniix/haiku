/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _GPU_PRESENT_H
#define _GPU_PRESENT_H


/*!	GPU buffer presentation through the accelerant (Renga, roadmap F-47;
	request R-1 of the Quasar NVIDIA driver, Techniix/nvidia-haiku,
	docs/RENGA-REQUESTS.md).

	PROVISIONAL. This is the table the Quasar driver proposed, with the
	names and the header's home decided (F-58); app_server does not use it
	yet. The app_server side (GPU buffers in overlay bitmaps, a flip queue,
	direct scanout) comes with the GPU stage (F-47), after the open points
	on real hardware are settled. Until then the layout may change; every
	change bumps B_GPU_PRESENT_VERSION, and an accelerant fills in the
	version it implements, so app_server can refuse a table it does not
	know.

	An accelerant offers the table through get_accelerant_hook():
		gpu_present_hooks* hooks
			= (gpu_present_hooks*)get_accelerant_hook(B_GPU_PRESENT_HOOKS,
				NULL);
	It is called by app_server only (the accelerant's owner).

	Buffers are file descriptors (B_GPU_BUFFER_FD): a driver-specific
	shareable descriptor, an nvidiactl memory descriptor for Quasar or a
	dma-buf descriptor for LinuxKPI DRM drivers. An application hands its
	descriptor to app_server with BPrivate::app_server_send_fd()
	(headers/private/app/ServerFdTransfer.h, SCM_RIGHTS on a local socket
	next to the app_server link), and refers to it in app_server messages
	by the token that returns.

	Events (flip completion, vblank) are queued by the accelerant;
	event_fd() is readable while any are queued, for select(), poll() and
	epoll in app_server's own thread.
*/


#include <GraphicsDefs.h>
#include <OS.h>


// get_accelerant_hook() feature code ('RGA', vendor neutral).
#define B_GPU_PRESENT_HOOKS		0x52474100
#define B_GPU_PRESENT_VERSION	1


// gpu_buffer_desc::handle_type
enum {
	B_GPU_BUFFER_FD = 1		// handle is a file descriptor
};

// gpu_present_caps::flags
enum {
	B_GPU_PRESENT_VRR			= 0x01,	// variable refresh rate
	B_GPU_PRESENT_ASYNC_FLIP	= 0x02,	// flips without waiting for vblank
	B_GPU_PRESENT_SCALING		= 0x04,	// planes scale source to destination
		// (implies B_GPU_PRESENT_PLACEMENT)
	B_GPU_PRESENT_PLACEMENT		= 0x08,	// overlay planes can be positioned
		// anywhere on the head at their source size (gpu_flip_params::
		// destination honoured, width and height equal to the source's);
		// for hardware that can place an overlay but not scale it
	B_GPU_PRESENT_SEMAPHORES	= 0x10	// flips wait on gpu_flip_params::
		// acquire and write release in GPU memory (Quasar R-1 amendment)
};

// gpu_flip_params::flags
enum {
	B_GPU_FLIP_VSYNC	= 0x01,
	B_GPU_FLIP_ASYNC	= 0x02,
	B_GPU_FLIP_VRR		= 0x04
};

// gpu_present_event::type
enum {
	B_GPU_EVENT_FLIP_COMPLETE	= 1,
	B_GPU_EVENT_VBLANK			= 2
};


typedef struct {
	int32		x;
	int32		y;
	uint32		width;
	uint32		height;
} gpu_rect;


typedef struct {
	uint32		handle_type;		// B_GPU_BUFFER_FD
	int			handle;				// driver-specific shareable descriptor
	uint32		width;
	uint32		height;
	uint32		bytes_per_row;		// row pitch in bytes; 0 for a non-linear
										// buffer lets the driver derive it
	color_space	format;
	uint64		modifier;			// driver tiling/layout, 0 = linear
	uint64		offset;				// of the image inside the object
} gpu_buffer_desc;


// A 32-bit value in GPU memory the display engine waits on or writes
// (Quasar R-1 amendment, provisional like the rest of this header).
typedef struct {
	int32		buffer;				// an imported buffer id holding the
									// semaphore, or -1 for none
	uint32		offset;				// byte offset of the 32-bit value,
									// 16-byte aligned
	uint32		value;				// acquire: flip once the word equals it;
									// release: written when this buffer is
									// replaced on screen
} gpu_semaphore;


typedef struct {
	uint32		planes_per_head;	// 1 = primary only
	uint32		flags;				// B_GPU_PRESENT_*
	uint32		max_width;
	uint32		max_height;
} gpu_present_caps;


typedef struct {
	gpu_rect	source;				// buffer pixels
	gpu_rect	destination;		// on screen; ignored for the primary plane
	uint32		flags;				// B_GPU_FLIP_*
	int			wait_fd;			// -1, or a descriptor that becomes
									// readable when rendering is complete
	gpu_semaphore acquire;			// with B_GPU_PRESENT_SEMAPHORES: flip
									// once the GPU wrote the value;
									// buffer -1 for none
	gpu_semaphore release;			// written when the buffer leaves the
									// screen; buffer -1 for none
		// A flip gives wait_fd or acquire, not both: wait_fd for drivers
		// whose display waits on fences, acquire for drivers that set
		// B_GPU_PRESENT_SEMAPHORES.
	uint64		user_data;			// returned in the completion event
} gpu_flip_params;


typedef struct {
	uint32		type;				// B_GPU_EVENT_*
	uint32		head;
	uint32		plane;
	uint64		user_data;
	bigtime_t	time;				// system_time() of the event
	uint64		sequence;			// vblank count
} gpu_present_event;


typedef struct {
	uint32		version;			// B_GPU_PRESENT_VERSION implemented
	status_t	(*get_caps)(uint32 head, gpu_present_caps* caps);
	status_t	(*import_buffer)(const gpu_buffer_desc* desc, uint32* _id);
	status_t	(*release_buffer)(uint32 id);
	status_t	(*flip)(uint32 head, uint32 plane, uint32 id,
					const gpu_flip_params* params);
	status_t	(*disable_plane)(uint32 head, uint32 plane);
	// Vblank events for a head: repeat == true sends a B_GPU_EVENT_VBLANK at
	// every vblank until called again with repeat == false; repeat == false
	// sends one event at the next vblank and stops repeating. A request
	// while one is still outstanding does not queue a second event.
	status_t	(*request_vblank)(uint32 head, bool repeat);
	int			(*event_fd)(void);
	status_t	(*next_event)(gpu_present_event* event);
	// R-2: the current scanout buffer of a head, as a new descriptor.
	status_t	(*export_scanout)(uint32 head, gpu_buffer_desc* desc);
} gpu_present_hooks;


#endif	// _GPU_PRESENT_H
