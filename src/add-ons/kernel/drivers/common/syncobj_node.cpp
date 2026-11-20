/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	/dev/syncobj: DRM sync objects for programs that have no GPU render
	node, such as the Wayland compositor and the tests. Each open descriptor
	has its own handle table, as a render node's does; the work is done by
	the generic/syncobj/v1 module (<syncobj_module.h>).
*/


#include <Drivers.h>
#include <KernelExport.h>

#include <syncobj_module.h>


#define DEVICE_NAME "syncobj"


int32 api_version = B_CUR_DRIVER_API_VERSION;

static syncobj_module_info* sSyncObj;


static status_t
syncobj_node_open(const char* name, uint32 flags, void** _cookie)
{
	return sSyncObj->open_file((syncobj_file**)_cookie);
}


static status_t
syncobj_node_close(void* cookie)
{
	return B_OK;
}


static status_t
syncobj_node_free(void* cookie)
{
	sSyncObj->close_file((syncobj_file*)cookie);
	return B_OK;
}


static status_t
syncobj_node_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	// the DRM requests, and the sw_sync CPU timeline render nodes lack
	return sSyncObj->ioctl((syncobj_file*)cookie, op, buffer, length);
}


static status_t
syncobj_node_read(void* cookie, off_t position, void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
syncobj_node_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sDeviceHooks = {
	&syncobj_node_open,
	&syncobj_node_close,
	&syncobj_node_free,
	&syncobj_node_control,
	&syncobj_node_read,
	&syncobj_node_write,
	NULL,	// select
	NULL,	// deselect
	NULL,	// readv
	NULL	// writev
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	return get_module(B_SYNCOBJ_MODULE_NAME, (module_info**)&sSyncObj);
}


void
uninit_driver()
{
	put_module(B_SYNCOBJ_MODULE_NAME);
}


const char**
publish_devices()
{
	static const char* devices[] = { DEVICE_NAME, NULL };
	return devices;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
