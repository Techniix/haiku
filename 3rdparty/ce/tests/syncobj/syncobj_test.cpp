/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for the syncobj module (Quasar R-18) through /dev/syncobj:
	DRM sync objects (binary and timeline), sync_file descriptors, the
	sw_sync CPU timeline, SYNCOBJ_EVENTFD and the wait flags, with Linux's
	documented semantics (drm_syncobj.c, sync_file.c). Pending fences come
	from the sw_sync timeline, the way Linux's own tests (igt) get them
	without a GPU. Output format: see 3rdparty/ce/README.md.
*/


#include <drm/drm_syncobj.h>
#include <linux/sync_file.h>
#include <sys/eventfd.h>

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>


#define SUITE "syncobj"


static int sFailures = 0;
static int sNode = -1;


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


//! ioctl() returning 0 or the errno, as libdrm's drmIoctl() callers see it.
static int
request(int fd, unsigned long op, void* data)
{
	return ioctl(fd, op, data) == 0 ? 0 : errno;
}


static int64_t
now_ns()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (int64_t)time.tv_sec * 1000000000LL + time.tv_nsec;
}


static uint32_t
create(int flags = 0)
{
	drm_syncobj_create data = {};
	data.flags = flags;
	if (request(sNode, DRM_IOCTL_SYNCOBJ_CREATE, &data) != 0)
		return 0;
	return data.handle;
}


static void
destroy(uint32_t handle)
{
	drm_syncobj_destroy data = {};
	data.handle = handle;
	request(sNode, DRM_IOCTL_SYNCOBJ_DESTROY, &data);
}


static int
wait(uint32_t* handles, uint32_t count, uint32_t flags, int64_t timeout,
	uint32_t* first = NULL)
{
	drm_syncobj_wait data = {};
	data.handles = (uint64_t)(uintptr_t)handles;
	data.count_handles = count;
	data.flags = flags;
	data.timeout_nsec = timeout;
	int error = request(sNode, DRM_IOCTL_SYNCOBJ_WAIT, &data);
	if (first != NULL)
		*first = data.first_signaled;
	return error;
}


static int
wait1(uint32_t handle, uint32_t flags = 0, int64_t timeout = 0)
{
	return wait(&handle, 1, flags, timeout);
}


static int
timeline_wait(uint32_t handle, uint64_t point, uint32_t flags,
	int64_t timeout)
{
	drm_syncobj_timeline_wait data = {};
	data.handles = (uint64_t)(uintptr_t)&handle;
	data.points = (uint64_t)(uintptr_t)&point;
	data.count_handles = 1;
	data.flags = flags;
	data.timeout_nsec = timeout;
	return request(sNode, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &data);
}


static int
array_request(unsigned long op, uint32_t handle)
{
	drm_syncobj_array data = {};
	data.handles = (uint64_t)(uintptr_t)&handle;
	data.count_handles = 1;
	return request(sNode, op, &data);
}


static int
timeline_signal(uint32_t handle, uint64_t point)
{
	drm_syncobj_timeline_array data = {};
	data.handles = (uint64_t)(uintptr_t)&handle;
	data.points = (uint64_t)(uintptr_t)&point;
	data.count_handles = 1;
	return request(sNode, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &data);
}


static uint64_t
query(uint32_t handle, uint32_t flags = 0)
{
	uint64_t point = ~0ULL;
	drm_syncobj_timeline_array data = {};
	data.handles = (uint64_t)(uintptr_t)&handle;
	data.points = (uint64_t)(uintptr_t)&point;
	data.count_handles = 1;
	data.flags = flags;
	if (request(sNode, DRM_IOCTL_SYNCOBJ_QUERY, &data) != 0)
		return ~0ULL;
	return point;
}


//! A sw_sync fence on \a node that signals at counter \a value.
static int
sw_fence(int node, uint32_t value)
{
	sw_sync_create_fence_data data = {};
	data.value = value;
	strcpy(data.name, "test");
	if (request(node, SW_SYNC_IOC_CREATE_FENCE, &data) != 0)
		return -1;
	return data.fence;
}


static int
sw_inc(int node, uint32_t amount)
{
	return request(node, SW_SYNC_IOC_INC, &amount);
}


static bool
readable(int fd)
{
	struct pollfd pfd = { fd, POLLIN, 0 };
	return poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0;
}


static int
sync_file_status(int fd)
{
	sync_file_info info = {};
	sync_fence_info fence = {};
	info.num_fences = 1;
	info.sync_fence_info = (uint64_t)(uintptr_t)&fence;
	if (request(fd, SYNC_IOC_FILE_INFO, &info) != 0)
		return -1000;
	return info.status;
}


static int
import_sync_file(uint32_t handle, int fd, uint64_t point)
{
	drm_syncobj_handle data = {};
	data.handle = handle;
	data.fd = fd;
	data.point = point;
	data.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE
		| (point != 0 ? DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE : 0);
	return request(sNode, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &data);
}


static int
export_sync_file(uint32_t handle, uint64_t point)
{
	drm_syncobj_handle data = {};
	data.handle = handle;
	data.fd = -1;
	data.point = point;
	data.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE
		| (point != 0 ? DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE : 0);
	if (request(sNode, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &data) != 0)
		return -1;
	return data.fd;
}


//	#pragma mark - tests


static void
test_create_destroy()
{
	uint32_t a = create();
	uint32_t b = create();
	drm_syncobj_destroy data = {};
	data.handle = a;
	int first = request(sNode, DRM_IOCTL_SYNCOBJ_DESTROY, &data);
	int second = request(sNode, DRM_IOCTL_SYNCOBJ_DESTROY, &data);
	data.handle = 4711;
	int unknown = request(sNode, DRM_IOCTL_SYNCOBJ_DESTROY, &data);
	uint32_t c = create();
	report("create_destroy", a != 0 && b != 0 && a != b && first == 0
			&& second == EINVAL && unknown == EINVAL && c == a,
		"handles %u %u, destroy %d, again %s, unknown %s, reused %u",
		a, b, first, strerror(second), strerror(unknown), c);
	destroy(b);
	destroy(c);
}


static void
test_binary_waits()
{
	uint32_t object = create();
	int unsubmitted = wait1(object);
	int submitTimeout = wait1(object,
		DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0);
	bigtime_t start = system_time();
	int submitLater = wait1(object, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
		now_ns() + 50000000LL);
	bigtime_t waited = system_time() - start;
	report("wait_unsubmitted", unsubmitted == EINVAL && submitTimeout == ETIME
			&& submitLater == ETIME && waited >= 45000 && waited < 2000000,
		"no fence: %s; WAIT_FOR_SUBMIT now: %s, in 50 ms: %s after %lld us",
		strerror(unsubmitted), strerror(submitTimeout), strerror(submitLater),
		(long long)waited);

	uint32_t signaled = create(DRM_SYNCOBJ_CREATE_SIGNALED);
	report("create_signaled", wait1(signaled) == 0, "");

	int signal = array_request(DRM_IOCTL_SYNCOBJ_SIGNAL, object);
	int afterSignal = wait1(object);
	int reset = array_request(DRM_IOCTL_SYNCOBJ_RESET, object);
	int afterReset = wait1(object);
	report("signal_reset", signal == 0 && afterSignal == 0 && reset == 0
			&& afterReset == EINVAL,
		"signal %d wait %d reset %d wait %s", signal, afterSignal, reset,
		strerror(afterReset));

	uint32_t handles[2] = { object, signaled };
	uint32_t first = 99;
	int any = wait(handles, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0,
		&first);
	int all = wait(handles, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT
		| DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, now_ns() + 10000000LL);
	uint32_t bad[1] = { 4711 };
	int unknown = wait(bad, 1, 0, 0);
	int flags = wait(handles, 2, 0x80, 0);
	report("wait_any_all", any == 0 && first == 1 && all == ETIME
			&& unknown == ENOENT && flags == EINVAL,
		"any %d first %u, all %s, unknown handle %s, bad flag %s", any,
		first, strerror(all), strerror(unknown), strerror(flags));

	drm_syncobj_wait data = {};
	data.handles = 16;	// unmapped
	data.count_handles = 1;
	report("bad_address", request(sNode, DRM_IOCTL_SYNCOBJ_WAIT, &data)
		== EFAULT, "");

	destroy(object);
	destroy(signaled);
}


static void
test_timeline_signal_query()
{
	uint32_t object = create();
	int one = timeline_signal(object, 1);
	int three = timeline_signal(object, 3);
	uint64_t value = query(object);
	uint64_t last = query(object, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED);
	int waitTwo = timeline_wait(object, 2, 0, 0);
	int waitFour = timeline_wait(object, 4, 0, 0);
	int waitFourSubmit = timeline_wait(object, 4,
		DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0);
	report("timeline_signal_query", one == 0 && three == 0 && value == 3
			&& last == 3 && waitTwo == 0 && waitFour == EINVAL
			&& waitFourSubmit == ETIME,
		"payload %llu, last submitted %llu, point 2 %d, point 4 %s / %s",
		(unsigned long long)value, (unsigned long long)last, waitTwo,
		strerror(waitFour), strerror(waitFourSubmit));
	destroy(object);
}


struct WaitArgs {
	uint32_t	handle;
	uint64_t	point;
	int			result;
	bigtime_t	returned;
};


static void*
waiter_thread(void* _args)
{
	WaitArgs* args = (WaitArgs*)_args;
	args->result = timeline_wait(args->handle, args->point,
		DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, now_ns() + 5000000000LL);
	args->returned = system_time();
	return NULL;
}


static void
test_wait_for_submit_thread()
{
	WaitArgs args = { create(), 5, -1, 0 };
	pthread_t thread;
	pthread_create(&thread, NULL, waiter_thread, &args);
	usleep(50000);
	bool stillWaiting = args.returned == 0;
	bigtime_t signaled = system_time();
	timeline_signal(args.handle, 5);
	pthread_join(thread, NULL);
	bigtime_t latency = args.returned - signaled;
	report("wait_for_submit_thread", stillWaiting && args.result == 0
			&& latency < 500000,
		"waited until signalled: %s, result %d, woke %lld us later",
		stillWaiting ? "yes" : "no", args.result, (long long)latency);
	destroy(args.handle);
}


static void
test_sw_sync()
{
	int fence = sw_fence(sNode, 2);
	bool pending = fence >= 0 && !readable(fence)
		&& sync_file_status(fence) == 0;
	sw_inc(sNode, 1);
	bool stillPending = !readable(fence);
	sw_inc(sNode, 1);
	bool signaled = readable(fence) && sync_file_status(fence) == 1;
	int past = sw_fence(sNode, 1);
	bool pastSignaled = past >= 0 && readable(past);
	report("sw_sync", pending && stillPending && signaled && pastSignaled,
		"pending %d, after 1 %d, after 2 %d, created signalled %d", pending,
		stillPending, signaled, pastSignaled);
	close(fence);
	close(past);
}


static void
test_import_export()
{
	uint32_t object = create();
	int fence = sw_fence(sNode, 3);
		// test_sw_sync() left the counter at 2
	int imported = import_sync_file(object, fence, 0);
	int pending = wait1(object, 0, 0);
	int exported = export_sync_file(object, 0);
	bool exportPending = exported >= 0 && !readable(exported);
	sw_inc(sNode, 3);
	int done = wait1(object, 0, 0);
	bool exportSignaled = exported >= 0 && readable(exported);
	uint32_t empty = create();
	int exportEmpty = export_sync_file(empty, 0);
	report("import_export", imported == 0 && pending == ETIME
			&& exportPending && done == 0 && exportSignaled
			&& exportEmpty < 0,
		"import %d, pending wait %s, export pending %d, after signal %d/%d,"
		" export without fence %d", imported, strerror(pending),
		exportPending, done, exportSignaled, exportEmpty);
	close(fence);
	close(exported);
	destroy(object);
	destroy(empty);
}


static void
test_timeline_chain()
{
	// A fresh node: its sw_sync counter starts at 0.
	int node = open("/dev/syncobj", O_RDWR);
	int saved = sNode;
	sNode = node;

	uint32_t object = create();
	int fence = sw_fence(node, 1);
	import_sync_file(object, fence, 1);
	timeline_signal(object, 2);
		// point 2 is signalled itself, but point 1 before it is not
	uint64_t before = query(object);
	int waitTwo = timeline_wait(object, 2, 0, 0);
	int availableTwo = timeline_wait(object, 2,
		DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0);
	int exported = export_sync_file(object, 2);
	bool exportPending = exported >= 0 && !readable(exported);
	sw_inc(node, 1);
	uint64_t after = query(object);
	int waitAfter = timeline_wait(object, 2, 0, 0);
	bool exportSignaled = exported >= 0 && readable(exported);
	report("timeline_chain", before == 0 && waitTwo == ETIME
			&& availableTwo == 0 && exportPending && after == 2
			&& waitAfter == 0 && exportSignaled,
		"payload %llu -> %llu, point 2 %s then %d, available %d, exported"
		" point pending %d then %d", (unsigned long long)before,
		(unsigned long long)after, strerror(waitTwo), waitAfter,
		availableTwo, exportPending, exportSignaled);
	close(fence);
	close(exported);
	destroy(object);

	sNode = saved;
	close(node);
}


static void
test_merge()
{
	int node = open("/dev/syncobj", O_RDWR);
	int a = sw_fence(node, 1);
	int b = sw_fence(node, 2);
	sync_merge_data data = {};
	strcpy(data.name, "merged");
	data.fd2 = b;
	int merge = request(a, SYNC_IOC_MERGE, &data);
	sw_inc(node, 1);
	bool halfPending = merge == 0 && !readable(data.fence)
		&& readable(a);
	sw_inc(node, 1);
	bool signaled = merge == 0 && readable(data.fence)
		&& sync_file_status(data.fence) == 1;
	report("merge", halfPending && signaled, "merge %d, after one %d,"
		" after both %d", merge, halfPending, signaled);
	close(a);
	close(b);
	if (merge == 0)
		close(data.fence);
	close(node);
}


static void
test_eventfd()
{
	int node = open("/dev/syncobj", O_RDWR);
	int saved = sNode;
	sNode = node;

	uint32_t object = create();
	int event = eventfd(0, EFD_NONBLOCK);
	drm_syncobj_eventfd data = {};
	data.handle = object;
	data.point = 7;
	data.fd = event;
	int registered = request(node, DRM_IOCTL_SYNCOBJ_EVENTFD, &data);
	bool quiet = !readable(event);
	timeline_signal(object, 7);
	uint64_t count = 0;
	bool fired = read(event, &count, sizeof(count)) == sizeof(count)
		&& count == 1;

	// WAIT_AVAILABLE: fires once a fence is there, before it signals
	data.point = 8;
	data.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	request(node, DRM_IOCTL_SYNCOBJ_EVENTFD, &data);
	int fence = sw_fence(node, 1);
	import_sync_file(object, fence, 8);
	count = 0;
	bool available = read(event, &count, sizeof(count)) == sizeof(count)
		&& count == 1 && !readable(fence);

	// already there: fires right away
	data.point = 7;
	data.flags = 0;
	request(node, DRM_IOCTL_SYNCOBJ_EVENTFD, &data);
	count = 0;
	bool immediate = read(event, &count, sizeof(count)) == sizeof(count);

	// not an eventfd
	data.fd = fence;
	int notEventfd = request(node, DRM_IOCTL_SYNCOBJ_EVENTFD, &data);

	// one-shot: a second signal does not write again
	timeline_signal(object, 9);
	bool oneShot = !readable(event);

	report("eventfd", registered == 0 && quiet && fired && available
			&& immediate && notEventfd == EINVAL && oneShot,
		"registered %d, quiet %d, fired %d, available %d, immediate %d,"
		" non-eventfd %s, one-shot %d", registered, quiet, fired, available,
		immediate, strerror(notEventfd), oneShot);

	sw_inc(node, 1);
	close(fence);
	close(event);
	destroy(object);
	sNode = saved;
	close(node);
}


static void
test_transfer()
{
	uint32_t timeline = create();
	uint32_t binary = create();
	timeline_signal(timeline, 2);
	drm_syncobj_transfer data = {};
	data.src_handle = timeline;
	data.src_point = 2;
	data.dst_handle = binary;
	int toBinary = request(sNode, DRM_IOCTL_SYNCOBJ_TRANSFER, &data);
	int binaryWait = wait1(binary);

	data.src_handle = binary;
	data.src_point = 0;
	data.dst_handle = timeline;
	data.dst_point = 9;
	int toTimeline = request(sNode, DRM_IOCTL_SYNCOBJ_TRANSFER, &data);
	uint64_t last = query(timeline, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED);

	data.src_handle = timeline;
	data.src_point = 20;
	data.dst_point = 0;
	data.dst_handle = binary;
	int missing = request(sNode, DRM_IOCTL_SYNCOBJ_TRANSFER, &data);
	report("transfer", toBinary == 0 && binaryWait == 0 && toTimeline == 0
			&& last == 9 && missing == EINVAL,
		"to binary %d (wait %d), to point 9 %d (last %llu), missing point %s",
		toBinary, binaryWait, toTimeline, (unsigned long long)last,
		strerror(missing));
	destroy(timeline);
	destroy(binary);
}


static void
test_handle_fd()
{
	uint32_t object = create();
	drm_syncobj_handle data = {};
	data.handle = object;
	int exported = request(sNode, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &data);
	int fd = data.fd;
	bool cloexec = fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;

	int other = open("/dev/syncobj", O_RDWR);
	drm_syncobj_handle import = {};
	import.fd = fd;
	int imported = request(other, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import);
	destroy(object);
		// the object lives on in the other file

	// signal through the second file, wait there
	uint32_t handle = import.handle;
	drm_syncobj_array array = {};
	array.handles = (uint64_t)(uintptr_t)&handle;
	array.count_handles = 1;
	int signal = request(other, DRM_IOCTL_SYNCOBJ_SIGNAL, &array);
	drm_syncobj_wait waitData = {};
	waitData.handles = (uint64_t)(uintptr_t)&handle;
	waitData.count_handles = 1;
	int waited = request(other, DRM_IOCTL_SYNCOBJ_WAIT, &waitData);

	// handles are per file
	waitData.handles = (uint64_t)(uintptr_t)&object;
	int foreign = request(sNode, DRM_IOCTL_SYNCOBJ_WAIT, &waitData);

	// an old drm.h without "point" (16 bytes): same request, smaller size
	struct { uint32_t handle, flags; int32_t fd; uint32_t pad; } old = {};
	old.handle = create(DRM_SYNCOBJ_CREATE_SIGNALED);
	old.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	unsigned long oldOp = _LINUX_IOWR('d', 0xC1, old);
	int oldSize = request(sNode, oldOp, &old);
	bool oldWorks = oldSize == 0 && old.fd >= 0 && readable(old.fd);

	report("handle_fd", exported == 0 && cloexec && imported == 0
			&& signal == 0 && waited == 0 && foreign == ENOENT && oldWorks,
		"export %d (cloexec %d), import %d, signal %d, wait %d, other"
		" file's handle %s, 16-byte struct %d", exported, cloexec, imported,
		signal, waited, strerror(foreign), oldWorks);
	close(fd);
	close(old.fd);
	destroy(old.handle);
	close(other);
}


static void
test_close_signals()
{
	int node = open("/dev/syncobj", O_RDWR);
	int fence = sw_fence(node, 5);
	bool pending = !readable(fence);
	close(node);
	int status = sync_file_status(fence);
	report("close_signals_error", pending && readable(fence)
			&& status == ENOENT,
			// Haiku's errno values are negative already
		"pending %d, after the timeline closed: readable %d, status %d",
		pending, readable(fence), status);
	close(fence);
}


static void
test_other_ioctls()
{
	int unknown = request(sNode, _LINUX_IOWR('d', 0xA0, uint64_t), NULL);
	report("unknown_request", unknown != 0, "DRM_IOCTL 0xA0: %s",
		strerror(unknown));
}


int
main()
{
	sNode = open("/dev/syncobj", O_RDWR);
	report("open_node", sNode >= 0, "/dev/syncobj: %s",
		sNode >= 0 ? "ok" : strerror(errno));
	if (sNode < 0)
		return 1;

	test_create_destroy();
	test_binary_waits();
	test_timeline_signal_query();
	test_wait_for_submit_thread();
	test_sw_sync();
	test_import_export();
	test_timeline_chain();
	test_merge();
	test_eventfd();
	test_transfer();
	test_handle_fd();
	test_close_signals();
	test_other_ioctls();

	close(sNode);
	return sFailures == 0 ? 0 : 1;
}
