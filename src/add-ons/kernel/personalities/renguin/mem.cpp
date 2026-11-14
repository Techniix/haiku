/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux memory-management system calls: brk, mmap, munmap, mprotect, mremap,
 * msync, madvise.
 *
 * mmap translates Linux flags and protections the way Haiku's own POSIX mmap
 * does (src/system/libroot/posix/sys/mman.cpp), but calls the kernel-side VM
 * functions directly: Haiku's _user_map_file() insists that its name and
 * address arguments are user pointers. Anonymous mappings go through
 * vm_create_anonymous_area() with kernel = false, i.e. with the same checks a
 * Haiku program gets. File mappings use vm_map_file(), which looks the FD up
 * in the kernel's I/O context, so the program's FD is first duplicated into
 * the kernel context with dup_foreign_fd() and closed afterwards.
 *
 * brk maintains a single anonymous area for the program break, since Haiku has
 * no brk syscall of its own (libroot emulates brk() in userland). Compare
 * FreeBSD's linux_brk() in sys/compat/linux/linux_misc.c, which wraps the
 * native obreak.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <KernelExport.h>
#include <OS.h>

#include <fs/fd.h>
#include <kernel.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <syscalls.h>
#include <vm_defs.h>

#include <sys/mman.h>


static const char* const kMmapAreaName = "linux mmap";


static uint32
protection_to_haiku(uint64 prot)
{
	uint32 result = 0;
	if ((prot & LX_PROT_READ) != 0)
		result |= B_READ_AREA;
	if ((prot & LX_PROT_WRITE) != 0)
		result |= B_WRITE_AREA;
	if ((prot & LX_PROT_EXEC) != 0)
		result |= B_EXECUTE_AREA;
	return result;
}


static inline bool
is_user_range(addr_t address, size_t size)
{
	return size > 0 && address + size > address && IS_USER_ADDRESS(address)
		&& IS_USER_ADDRESS(address + size - 1);
}


int64
renguin_sys_brk(uint64 addr)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return 0;

	// Linux brk() returns the current break on failure or a query, never an
	// errno. Unlike the pages behind it, the break itself is not page
	// aligned: glibc's static TLS setup asks for brk + 0xd00 and expects
	// exactly that back.
	// Threads share the break (glibc's malloc serializes its own calls, but
	// nothing else has to).
	MutexLocker locker(data->lock);

	if (addr == 0 || addr < data->brk_start)
		return data->brk_current;

	addr_t newMappedEnd = ROUNDUP((addr_t)addr, B_PAGE_SIZE);
	if (newMappedEnd < addr || !IS_USER_ADDRESS(newMappedEnd - 1))
		return data->brk_current;

	team_id team = thread_get_current_thread()->team->id;

	// After fork() the child's heap area has a new ID; find it by address.
	if (data->brk_area == RENGUIN_BRK_AREA_LOOKUP) {
		data->brk_area = _user_area_for((void*)data->brk_start);
		area_info info;
		if (data->brk_area < 0
			|| get_area_info(data->brk_area, &info) != B_OK
			|| (addr_t)info.address != data->brk_start) {
			data->brk_area = -1;
			data->brk_mapped_end = data->brk_start;
			data->brk_current = data->brk_start;
		} else {
			// Another thread of the parent may have moved the break while
			// fork() was copying the areas: trust the area we got.
			data->brk_mapped_end = data->brk_start + info.size;
			if (data->brk_current > data->brk_mapped_end)
				data->brk_current = data->brk_mapped_end;
		}
	}

	if (newMappedEnd == data->brk_mapped_end) {
		// Nothing to map or unmap.
	} else if (newMappedEnd == data->brk_start) {
		// Shrunk to nothing: drop the area.
		if (data->brk_area >= 0) {
			vm_delete_area(team, data->brk_area, true);
			data->brk_area = -1;
		}
	} else if (data->brk_area < 0) {
		// First growth: create the heap area right after the executable.
		void* address = (void*)data->brk_start;
		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = address;
		virtualRestrictions.address_specification = B_EXACT_ADDRESS;
		physical_address_restrictions physicalRestrictions = {};
		area_id area = vm_create_anonymous_area(team, "linux heap",
			newMappedEnd - data->brk_start, B_NO_LOCK,
			B_READ_AREA | B_WRITE_AREA | B_OVERCOMMITTING_AREA, 0, 0,
			&virtualRestrictions, &physicalRestrictions, false, &address);
		if (area < 0)
			return data->brk_current;
		data->brk_area = area;
	} else {
		// Grow or shrink in place. This fails if another mapping is in the
		// way; Linux returns the old break then, and libc falls back to mmap.
		if (_user_resize_area(data->brk_area, newMappedEnd - data->brk_start)
				!= B_OK) {
			return data->brk_current;
		}
	}

	data->brk_mapped_end = newMappedEnd;
	data->brk_current = addr;
	return data->brk_current;
}


int64
renguin_sys_mmap(uint64 addr, uint64 length, uint64 prot, uint64 flags,
	int64 fd, int64 offset)
{
	if (length == 0 || (offset % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;
	if ((prot & ~(uint64)(LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC
			| LX_PROT_GROWSDOWN | LX_PROT_GROWSUP)) != 0) {
		return -LX_EINVAL;
	}

	size_t size = PAGE_ALIGN((size_t)length);
	if (size < length)
		return -LX_ENOMEM;

	bool shared;
	switch (flags & LX_MAP_TYPE) {
		case LX_MAP_SHARED:
		case LX_MAP_SHARED_VALIDATE:
			shared = true;
			break;
		case LX_MAP_PRIVATE:
			shared = false;
			break;
		default:
			return -LX_EINVAL;
	}
	bool anonymous = (flags & LX_MAP_ANONYMOUS) != 0;
	if (!anonymous && fd < 0)
		return -LX_EBADF;

	// Address placement.
	bool fixed = (flags & LX_MAP_FIXED) != 0;
	bool noReplace = !fixed && (flags & LX_MAP_FIXED_NOREPLACE) != 0;
	uint32 addressSpec;
	void* address = (void*)(addr_t)addr;
	if (fixed || noReplace) {
		if ((addr % B_PAGE_SIZE) != 0)
			return -LX_EINVAL;
		if (!is_user_range((addr_t)addr, size))
			return -LX_ENOMEM;
		addressSpec = B_EXACT_ADDRESS;
	} else if (addr != 0 && is_user_range(ROUNDDOWN((addr_t)addr, B_PAGE_SIZE),
			size)) {
		// A hint. Linux treats it loosely; B_BASE_ADDRESS means "at or
		// above", and we retry anywhere if that fails.
		address = (void*)ROUNDDOWN((addr_t)addr, B_PAGE_SIZE);
		addressSpec = B_BASE_ADDRESS;
	} else {
		address = NULL;
		addressSpec = B_RANDOMIZED_ANY_ADDRESS;
	}

	uint32 protection = protection_to_haiku(prot);
	// Linux overcommits: programs reserve large ranges (ld.so, thread stacks,
	// language runtimes) and touch little of them. Commit on use, as Linux
	// does, for private and inaccessible mappings.
	if ((flags & LX_MAP_NORESERVE) != 0 || (!shared && anonymous)
		|| protection == 0) {
		protection |= B_OVERCOMMITTING_AREA;
	}

	team_id team = thread_get_current_thread()->team->id;
	area_id area;

	if (anonymous) {
		if (shared)
			protection |= B_SHARED_AREA;
		uint32 areaFlags = fixed ? CREATE_AREA_UNMAP_ADDRESS_RANGE : 0;

		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = address;
		virtualRestrictions.address_specification = addressSpec;
		physical_address_restrictions physicalRestrictions = {};
		area = vm_create_anonymous_area(team, kMmapAreaName, size, B_NO_LOCK,
			protection, areaFlags, 0, &virtualRestrictions,
			&physicalRestrictions, false, &address);
		if (area < 0 && addressSpec == B_BASE_ADDRESS) {
			virtualRestrictions.address = NULL;
			virtualRestrictions.address_specification
				= B_RANDOMIZED_ANY_ADDRESS;
			area = vm_create_anonymous_area(team, kMmapAreaName, size,
				B_NO_LOCK, protection, areaFlags, 0, &virtualRestrictions,
				&physicalRestrictions, false, &address);
		}
	} else {
		// vm_map_file() works on kernel FDs (it is the kernel's own entry
		// point), so give the kernel its own reference to the open file.
		int kernelFD = dup_foreign_fd(team, (int)fd, true);
		if (kernelFD < 0)
			return -LX_EBADF;

		// MAP_FIXED replaces what is there. Unmap with the program's own
		// rights first, so that it cannot replace kernel-protected areas;
		// vm_map_file()'s own unmapping runs with kernel rights.
		if (fixed) {
			status_t status = _user_unmap_memory(address, size);
			if (status != B_OK) {
				_kern_close(kernelFD);
				return renguin_encode(status);
			}
		}

		uint32 mapping = shared ? REGION_NO_PRIVATE_MAP : REGION_PRIVATE_MAP;
		area = vm_map_file(team, kMmapAreaName, &address, addressSpec, size,
			protection, mapping, false, kernelFD, offset);
		if (area < 0 && addressSpec == B_BASE_ADDRESS) {
			address = NULL;
			area = vm_map_file(team, kMmapAreaName, &address,
				B_RANDOMIZED_ANY_ADDRESS, size, protection, mapping, false,
				kernelFD, offset);
		}
		_kern_close(kernelFD);
	}

	if (area < 0) {
		if (noReplace)
			return -LX_EEXIST;
		return renguin_encode(area);
	}

	return (int64)(addr_t)address;
}


int64
renguin_sys_munmap(uint64 addr, uint64 length)
{
	if (length == 0 || (addr % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;
	size_t size = PAGE_ALIGN((size_t)length);
	if (!is_user_range((addr_t)addr, size))
		return -LX_EINVAL;

	return renguin_encode(_user_unmap_memory((void*)(addr_t)addr, size));
}


int64
renguin_sys_mprotect(uint64 addr, uint64 length, uint64 prot)
{
	if ((addr % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;
	if (length == 0)
		return 0;
	// PROT_GROWSDOWN/UP: Haiku stacks do not grow; ignore the flags.
	if ((prot & ~(uint64)(LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC
			| LX_PROT_GROWSDOWN | LX_PROT_GROWSUP)) != 0) {
		return -LX_EINVAL;
	}

	status_t status = _user_set_memory_protection((void*)(addr_t)addr,
		(size_t)length, protection_to_haiku(prot));
	if (status == B_NO_MEMORY)
		return -LX_ENOMEM;		// part of the range is not mapped
	if (status == B_NOT_ALLOWED)
		return -LX_EACCES;
	return renguin_encode(status);
}


/*!	mremap: only shrinking in place, as in FreeBSD's linux_mremap(). glibc's
	realloc() falls back to allocate-copy-free when growing fails.
*/
int64
renguin_sys_mremap(uint64 oldAddr, uint64 oldSize, uint64 newSize,
	uint64 flags, uint64 newAddr)
{
	(void)newAddr;
	if ((oldAddr % B_PAGE_SIZE) != 0
		|| (flags & ~(uint64)(LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED)) != 0) {
		return -LX_EINVAL;
	}
	if ((flags & LX_MREMAP_FIXED) != 0)
		return -LX_EINVAL;

	oldSize = PAGE_ALIGN(oldSize);
	newSize = PAGE_ALIGN(newSize);
	if (newSize == 0)
		return -LX_EINVAL;

	if (newSize < oldSize) {
		status_t status = _user_unmap_memory(
			(void*)(addr_t)(oldAddr + newSize), oldSize - newSize);
		if (status != B_OK)
			return renguin_encode(status);
		return oldAddr;
	}
	if (newSize == oldSize)
		return oldAddr;

	return -LX_ENOMEM;
}


int64
renguin_sys_msync(uint64 addr, uint64 length, uint64 flags)
{
	if ((addr % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;
	if ((flags & ~(uint64)(LX_MS_ASYNC | LX_MS_INVALIDATE | LX_MS_SYNC)) != 0
		|| ((flags & LX_MS_ASYNC) != 0 && (flags & LX_MS_SYNC) != 0)) {
		return -LX_EINVAL;
	}
	if (length == 0)
		return 0;

	uint32 haikuFlags = 0;
	if ((flags & LX_MS_ASYNC) != 0)
		haikuFlags |= MS_ASYNC;
	if ((flags & LX_MS_SYNC) != 0)
		haikuFlags |= MS_SYNC;
	if ((flags & LX_MS_INVALIDATE) != 0)
		haikuFlags |= MS_INVALIDATE;
	if (haikuFlags == 0)
		haikuFlags = MS_ASYNC;

	status_t status = _user_sync_memory((void*)(addr_t)addr,
		PAGE_ALIGN((size_t)length), haikuFlags);
	if (status == B_NO_MEMORY)
		return -LX_ENOMEM;
	return renguin_encode(status);
}


/*!	madvise: accepted as a hint and ignored.
	TODO: MADV_DONTNEED on private anonymous memory must make the pages read
	back as zero (jemalloc and Go rely on it). FreeBSD emulates this in
	linux_madvise_dontneed(); Haiku needs the equivalent on its VMCache.
*/
int64
renguin_sys_madvise(uint64 addr, uint64 length, uint64 advice)
{
	(void)length;
	(void)advice;
	if ((addr % B_PAGE_SIZE) != 0)
		return -LX_EINVAL;
	return 0;
}
