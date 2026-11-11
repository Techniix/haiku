/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Hardware enumeration through the device manager's generic syscalls, the
 * same read-only interface listdev and the Devices preference use
 * (src/bin/listdev/dm_wrapper.c).
 */


#include "LinuxKPIManage.h"

#include <device_manager.h>
#include <device_manager_defs.h>
#include <generic_syscall_defs.h>
#include <syscalls.h>

#include <string.h>


namespace LKPI {


static status_t
dm_call(uint32 op, void* buffer, size_t size)
{
	return _kern_generic_syscall(DEVICE_MANAGER_SYSCALLS, op, buffer, size);
}


static void
read_node(device_node_cookie node, BObjectList<PCIDevice, true>& devices)
{
	char data[256];
	device_attr_info attr;
	attr.cookie = 0;
	attr.node_cookie = node;
	attr.value.raw.data = data;
	attr.value.raw.length = sizeof(data);

	char bus[64] = "";
	PCIDevice dev = {};
	bool haveVendor = false, haveDevice = false;

	while (dm_call(DM_GET_NEXT_ATTRIBUTE, &attr, sizeof(attr)) == B_OK) {
		if (strcmp(attr.name, B_DEVICE_BUS) == 0
			&& attr.type == B_STRING_TYPE) {
			strlcpy(bus, attr.value.string, sizeof(bus));
		} else if (strcmp(attr.name, B_DEVICE_VENDOR_ID) == 0
			&& attr.type == B_UINT16_TYPE) {
			dev.vendor = attr.value.ui16;
			haveVendor = true;
		} else if (strcmp(attr.name, B_DEVICE_ID) == 0
			&& attr.type == B_UINT16_TYPE) {
			dev.device = attr.value.ui16;
			haveDevice = true;
		} else if (strcmp(attr.name, B_DEVICE_TYPE) == 0
			&& attr.type == B_UINT16_TYPE) {
			dev.classBase = (uint8)attr.value.ui16;
		} else if (strcmp(attr.name, B_DEVICE_SUB_TYPE) == 0
			&& attr.type == B_UINT16_TYPE) {
			dev.classSub = (uint8)attr.value.ui16;
		} else if (strcmp(attr.name, B_DEVICE_INTERFACE) == 0
			&& attr.type == B_UINT16_TYPE) {
			dev.classInterface = (uint8)attr.value.ui16;
		}
		attr.value.raw.data = data;
		attr.value.raw.length = sizeof(data);
	}

	if (strcmp(bus, "pci") == 0 && haveVendor && haveDevice)
		devices.AddItem(new PCIDevice(dev));
}


static void
walk(device_node_cookie node, BObjectList<PCIDevice, true>& devices,
	int depth)
{
	if (depth > 32)
		return;
	read_node(node, devices);

	device_node_cookie child = node;
	if (dm_call(DM_GET_CHILD, &child, sizeof(child)) != B_OK)
		return;
	do {
		walk(child, devices, depth + 1);
	} while (dm_call(DM_GET_NEXT_CHILD, &child, sizeof(child)) == B_OK);
}


status_t
ScanPCIDevices(BObjectList<PCIDevice, true>& devices)
{
	devices.MakeEmpty();

	uint32 version = 0;
	status_t status = dm_call(B_SYSCALL_INFO, &version, sizeof(version));
	if (status != B_OK)
		return status;

	device_node_cookie root;
	status = dm_call(DM_GET_ROOT, &root, sizeof(root));
	if (status != B_OK)
		return status;
	walk(root, devices, 0);
	return B_OK;
}


const char*
PCIClassName(uint8 base, uint8 sub)
{
	switch (base) {
		case 0x01: return "Storage controller";
		case 0x02:
			return sub == 0x80 ? "Network controller (wireless)"
				: "Network controller";
		case 0x03: return "Display controller";
		case 0x04: return "Multimedia controller";
		case 0x05: return "Memory controller";
		case 0x06: return "Bridge";
		case 0x07: return "Communication controller";
		case 0x08: return "System peripheral";
		case 0x09: return "Input device";
		case 0x0c: return "Serial bus controller";
		case 0x0d: return "Wireless controller";
		case 0x10: return "Encryption controller";
		case 0x11: return "Signal processing controller";
		case 0x12: return "Processing accelerator";
	}
	return "Other device";
}


}	// namespace LKPI
