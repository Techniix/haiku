/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * ce_vmouse: a virtual relative mouse for the harness's pointer tests
 * (3rdparty/ce/tests/pointer/pointer_rate_test.cpp). Harness images only.
 *
 * The driver publishes only its control node, /dev/misc/ce_vmouse, so a
 * normal boot never shows a mouse. A test program plugs a mouse in with
 * CE_VMOUSE_PLUG: the driver publishes one or two nodes in
 * /dev/input/mouse/virtual/, input_server's mouse add-on finds them through
 * its node monitor and runs them like any other mouse, and the program
 * write()s reports (ce_vmouse_report) at whatever rate it likes, with the
 * timestamps it likes. The nodes answer MS_READ the way the two kinds of
 * real drivers do (CE_VMOUSE_QUEUED like PS/2, CE_VMOUSE_ON_DEMAND like
 * usb_hid with a mouse that sums its motion between polls), and with two
 * nodes both answer MS_GET_DEVICE_IDENTITY with the same device: a mouse
 * with two HID mouse interfaces, like the Darmoshark M3. Closing the
 * control node unplugs the mouse, so a test that crashes leaves nothing
 * behind.
 *
 * See ce_vmouse.h for the interface.
 */


#include "ce_vmouse.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <condition_variable.h>
#include <fs/devfs.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* kControlNames[] = { CE_VMOUSE_CONTROL_NAME, NULL };
	// a literal: legacy drivers get no static constructors
static const uint32 kQueueSize = 4096;


struct vmouse_node {
	bool				published;
	bool				removed;
	char				path[64];
	mouse_movement		queue[kQueueSize];
	uint32				head;
	uint32				count;
	ConditionVariable	readable;
};


static mutex sLock;
static vmouse_node sNodes[CE_VMOUSE_MAX_NODES];
static uint32 sNodeCount = 0;
static uint32 sMode = CE_VMOUSE_QUEUED;
static int32 sControlOpen = 0;


//	#pragma mark - nodes


static status_t
node_open(const char* name, uint32 flags, void** _cookie)
{
	MutexLocker locker(sLock);
	for (uint32 i = 0; i < sNodeCount; i++) {
		// devfs passes the path below /dev
		if (sNodes[i].published && !sNodes[i].removed
			&& strcmp(name, sNodes[i].path) == 0) {
			*_cookie = &sNodes[i];
			return B_OK;
		}
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
node_close(void* cookie)
{
	return B_OK;
}


static status_t
node_free(void* cookie)
{
	return B_OK;
}


static status_t
node_read_movement(vmouse_node* node, void* buffer, size_t length)
{
	if (length < sizeof(mouse_movement))
		return B_BUFFER_OVERFLOW;

	MutexLocker locker(sLock);
	while (node->count == 0) {
		if (node->removed)
			return B_DEV_NOT_READY;
		ConditionVariableEntry entry;
		node->readable.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_CAN_INTERRUPT);
		locker.Lock();
		if (status == B_INTERRUPTED)
			return B_INTERRUPTED;
	}
	if (node->removed)
		return B_DEV_NOT_READY;

	mouse_movement movement = node->queue[node->head];
	node->head = (node->head + 1) % kQueueSize;
	node->count--;
	locker.Unlock();

	if (movement.timestamp == 0)
		movement.timestamp = system_time();
	if (!IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;
	return user_memcpy(buffer, &movement, sizeof(movement));
}


static status_t
node_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	vmouse_node* node = (vmouse_node*)cookie;

	switch (op) {
		case MS_READ:
			return node_read_movement(node, buffer, length);

		case MS_NUM_EVENTS:
		{
			MutexLocker locker(sLock);
			if (node->removed)
				return B_DEV_NOT_READY;
			return sMode == CE_VMOUSE_QUEUED ? (status_t)node->count : 0;
		}

		case MS_GET_DEVICE_IDENTITY:
		{
			MutexLocker locker(sLock);
			if (sNodeCount < 2)
				return B_DEV_INVALID_IOCTL;
				// a single node: a mouse that cannot say (like PS/2)
			mouse_device_identity identity;
			memset(&identity, 0, sizeof(identity));
			identity.bus = 1;
			identity.device = CE_VMOUSE_DEVICE_ID;
			identity.interface = node == &sNodes[0] ? 0 : 2;
			identity.vendor = 0xce;
			identity.product = 0x1;
			locker.Unlock();
			if (length < sizeof(identity) || !IS_USER_ADDRESS(buffer))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &identity, sizeof(identity));
		}

		case MS_IS_TOUCHPAD:
		case MS_READ_TOUCHPAD:
			return B_DEV_INVALID_IOCTL;

		case MS_SET_TYPE:
		case MS_SET_MAP:
		case MS_SET_ACCEL:
		case MS_SET_CLICKSPEED:
			// the add-on's settings: it applies them itself
			return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
node_rw(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sNodeHooks = {
	node_open, node_close, node_free, node_control, node_rw,
	(device_write_hook)node_rw, NULL, NULL, NULL, NULL
};


//	#pragma mark - plugging


static void
unplug_locked(MutexLocker& locker)
{
	char paths[CE_VMOUSE_MAX_NODES][64];
	uint32 count = 0;
	for (uint32 i = 0; i < sNodeCount; i++) {
		vmouse_node& node = sNodes[i];
		if (!node.published)
			continue;
		node.removed = true;
		node.published = false;
		node.readable.NotifyAll(B_DEV_NOT_READY);
		strlcpy(paths[count++], node.path, sizeof(paths[0]));
	}
	sNodeCount = 0;

	locker.Unlock();
	for (uint32 i = 0; i < count; i++)
		devfs_unpublish_device(paths[i], false);
	locker.Lock();
}


static status_t
plug(const ce_vmouse_plug& request)
{
	if (request.nodes < 1 || request.nodes > CE_VMOUSE_MAX_NODES
		|| (request.mode != CE_VMOUSE_QUEUED
			&& request.mode != CE_VMOUSE_ON_DEMAND)) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(sLock);
	unplug_locked(locker);

	sMode = request.mode;
	for (uint32 i = 0; i < request.nodes; i++) {
		vmouse_node& node = sNodes[i];
		node.head = 0;
		node.count = 0;
		node.removed = false;
		node.published = true;
		snprintf(node.path, sizeof(node.path), CE_VMOUSE_NODE_FORMAT,
			(int)i);
	}
	sNodeCount = request.nodes;
	locker.Unlock();

	for (uint32 i = 0; i < request.nodes; i++) {
		status_t status = devfs_publish_device(sNodes[i].path, &sNodeHooks);
		if (status != B_OK) {
			dprintf("ce_vmouse: publishing %s failed: %s\n", sNodes[i].path,
				strerror(status));
			locker.Lock();
			unplug_locked(locker);
			return status;
		}
	}
	dprintf("ce_vmouse: plugged in, %" B_PRIu32 " node(s), %s\n",
		request.nodes, request.mode == CE_VMOUSE_QUEUED ? "queued"
			: "on demand");
	return B_OK;
}


//! Adds a report to its node, summing motion in on-demand mode.
static void
add_report_locked(const ce_vmouse_report& report)
{
	if (report.node >= sNodeCount)
		return;
	vmouse_node& node = sNodes[report.node];
	const mouse_movement& movement = report.movement;

	if (sMode == CE_VMOUSE_ON_DEMAND && node.count > 0) {
		mouse_movement& last
			= node.queue[(node.head + node.count - 1) % kQueueSize];
		if (last.buttons == movement.buttons && last.wheel_xdelta == 0
			&& last.wheel_ydelta == 0 && movement.wheel_xdelta == 0
			&& movement.wheel_ydelta == 0) {
			last.xdelta += movement.xdelta;
			last.ydelta += movement.ydelta;
			last.timestamp = movement.timestamp;
			last.clicks = movement.clicks;
			return;
		}
	}

	if (node.count == kQueueSize) {
		// full: the oldest goes, as in a real driver's ring
		node.head = (node.head + 1) % kQueueSize;
		node.count--;
	}
	node.queue[(node.head + node.count) % kQueueSize] = movement;
	node.count++;
	node.readable.NotifyAll();
}


//	#pragma mark - control node


static status_t
control_open(const char* name, uint32 flags, void** _cookie)
{
	if (atomic_add(&sControlOpen, 1) != 0) {
		atomic_add(&sControlOpen, -1);
		return B_BUSY;
	}
	*_cookie = NULL;
	return B_OK;
}


static status_t
control_close(void* cookie)
{
	MutexLocker locker(sLock);
	unplug_locked(locker);
	return B_OK;
}


static status_t
control_free(void* cookie)
{
	atomic_add(&sControlOpen, -1);
	return B_OK;
}


static status_t
control_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case CE_VMOUSE_PLUG:
		{
			ce_vmouse_plug request;
			if (length < sizeof(request) || !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return plug(request);
		}

		case CE_VMOUSE_UNPLUG:
		{
			MutexLocker locker(sLock);
			unplug_locked(locker);
			return B_OK;
		}

		case CE_VMOUSE_PENDING:
		{
			MutexLocker locker(sLock);
			uint32 count = 0;
			for (uint32 i = 0; i < sNodeCount; i++)
				count += sNodes[i].count;
			return (status_t)count;
		}
	}
	return B_DEV_INVALID_IOCTL;
}


static status_t
control_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
control_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	size_t length = *_length;
	*_length = 0;
	if (length % sizeof(ce_vmouse_report) != 0)
		return B_BAD_VALUE;
	if (!IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;

	const ce_vmouse_report* reports = (const ce_vmouse_report*)buffer;
	size_t count = length / sizeof(ce_vmouse_report);
	for (size_t i = 0; i < count; i++) {
		ce_vmouse_report report;
		if (user_memcpy(&report, &reports[i], sizeof(report)) != B_OK)
			return B_BAD_ADDRESS;
		MutexLocker locker(sLock);
		if (sNodeCount == 0)
			return B_DEV_NOT_READY;
		add_report_locked(report);
	}
	*_length = length;
	return B_OK;
}


static device_hooks sControlHooks = {
	control_open, control_close, control_free, control_control,
	control_read, control_write, NULL, NULL, NULL, NULL
};


//	#pragma mark - driver


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	mutex_init(&sLock, "ce_vmouse");
	for (uint32 i = 0; i < CE_VMOUSE_MAX_NODES; i++) {
		sNodes[i].published = false;
		sNodes[i].removed = false;
		sNodes[i].path[0] = '\0';
		sNodes[i].head = 0;
		sNodes[i].count = 0;
		sNodes[i].readable.Init(&sNodes[i], "ce_vmouse readable");
	}
	sNodeCount = 0;
	sControlOpen = 0;
	return B_OK;
}


void
uninit_driver()
{
	MutexLocker locker(sLock);
	unplug_locked(locker);
	locker.Unlock();
	mutex_destroy(&sLock);
}


const char**
publish_devices()
{
	return kControlNames;
}


device_hooks*
find_device(const char* name)
{
	if (strcmp(name, CE_VMOUSE_CONTROL_NAME) == 0)
		return &sControlHooks;
	return &sNodeHooks;
}
