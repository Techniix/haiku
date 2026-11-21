/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Maps Renguin's vDSO (vdso/vdso.c, embedded as VDSO.h) into a Linux
 * process at exec and names it in the auxiliary vector (AT_SYSINFO_EHDR),
 * so glibc and musl read the clocks and the CPU number without a system
 * call.
 *
 * The image is one read-only, executable segment at address 0
 * (vdso/vdso-layout.ld), so it is copied as it is into a one-page area
 * named "[vdso]" (the name /proc/self/maps shows, which some programs
 * look for). Its data block (struct renguin_vdso_data, found by its magic)
 * gets the team's commpage address, where the vDSO finds Haiku's
 * real_time_data, and which time-stamp instructions the CPU has. fork()
 * copies the area with the rest of the address space; the commpage is at
 * the same address in the child.
 *
 * Switch: "vdso false" in the renguin driver settings (the Renguin
 * Manager's "Fast clocks (vDSO)") leaves it out; the C libraries then make
 * the system calls, as before.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <string.h>

#include <arch_cpu.h>
#include <commpage.h>
#include <driver_settings.h>
#include <team.h>
#include <vm/vm.h>

#include "VDSO.h"


#define VDSO_FEATURE_RDTSCP		0x1
#define VDSO_FEATURE_RDPID		0x2

// matches struct vdso_data in vdso/vdso.c
struct renguin_vdso_data {
	char	magic[16];
	uint64	commpage;
	uint32	features;
	uint32	reserved;
};

static const char kVdsoMagic[16] = { 'R', 'e', 'n', 'g', 'u', 'i', 'n', 'V',
	'd', 's', 'o', 'D', 'a', 't', 'a', '!' };

static int32 sVdsoDataOffset = -1;		// -2: not found, no vDSO


//! Where the data block is in the image, once.
static int32
vdso_data_offset()
{
	if (sVdsoDataOffset != -1)
		return sVdsoDataOffset;
	int32 offset = -2;
	for (size_t i = 0; i + sizeof(renguin_vdso_data) <= kRenguinVdsoSize;
			i += 8) {
		if (memcmp(kRenguinVdso + i, kVdsoMagic, sizeof(kVdsoMagic)) == 0) {
			offset = (int32)i;
			break;
		}
	}
	if (offset < 0)
		dprintf("renguin: vDSO data block not found, vDSO left out\n");
	sVdsoDataOffset = offset;
	return offset;
}


static bool
vdso_enabled()
{
	bool enabled = true;
	if (void* handle = load_driver_settings("renguin")) {
		enabled = get_driver_boolean_parameter(handle, "vdso", true, true);
		unload_driver_settings(handle);
	}
	return enabled;
}


static uint32
vdso_features()
{
	uint32 features = 0;
	if (x86_check_feature(IA32_FEATURE_AMD_EXT_RDTSCP, FEATURE_EXT_AMD))
		features |= VDSO_FEATURE_RDTSCP;
	if (x86_check_feature(IA32_FEATURE_RDPID, FEATURE_7_ECX))
		features |= VDSO_FEATURE_RDPID;
	return features;
}


/*!	Maps the vDSO into \a team (the current team, during exec). Returns its
	address, or 0 when it is switched off or could not be mapped (the
	program then makes system calls, which always work).
*/
addr_t
renguin_map_vdso(Team* team)
{
	int32 dataOffset = vdso_data_offset();
	if (dataOffset < 0 || !vdso_enabled())
		return 0;

	// The kernel maps the commpage only for native programs, after the
	// personalities had their turn (team.cpp); exec has removed the old
	// one, and team->commpage_address still names it. Map it for the vDSO.
	area_id commPage = clone_commpage_area(team->id, &team->commpage_address);
	if (commPage < 0) {
		dprintf("renguin: commpage for the vDSO: %s\n", strerror(commPage));
		return 0;
	}

	size_t size = ROUNDUP(kRenguinVdsoSize, B_PAGE_SIZE);
	void* address = NULL;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address_specification = B_RANDOMIZED_ANY_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	// Made writable to fill it in, then read-only and executable: x86 has
	// one write permission for user and kernel, so Haiku refuses a
	// user-read-only area the kernel may write (arch_vm_supports_protection).
	area_id area = vm_create_anonymous_area(team->id, "[vdso]", size,
		B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA
			| B_KERNEL_WRITE_AREA,
		0, 0, &addressRestrictions, &physicalRestrictions, false, &address);
	if (area < 0) {
		dprintf("renguin: vDSO area: %s\n", strerror(area));
		return 0;
	}

	renguin_vdso_data vdsoData;
	memcpy(&vdsoData, kRenguinVdso + dataOffset, sizeof(vdsoData));
	vdsoData.commpage = (uint64)(addr_t)team->commpage_address;
	vdsoData.features = vdso_features();
	if (user_memcpy(address, kRenguinVdso, kRenguinVdsoSize) != B_OK
		|| user_memcpy((uint8*)address + dataOffset, &vdsoData,
			sizeof(vdsoData)) != B_OK) {
		vm_delete_area(team->id, area, true);
		return 0;
	}
	status_t status = vm_set_area_protection(area,
		B_READ_AREA | B_EXECUTE_AREA | B_KERNEL_READ_AREA, true);
	if (status != B_OK) {
		dprintf("renguin: vDSO protection: %s\n", strerror(status));
		vm_delete_area(team->id, area, true);
		return 0;
	}
	return (addr_t)address;
}
