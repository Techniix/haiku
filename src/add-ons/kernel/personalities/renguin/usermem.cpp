/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Access to the Linux program's memory.
 *
 * Haiku's user_memcpy() only rejects ranges that straddle the user/kernel
 * boundary; a pointer that lies entirely in kernel space is accepted. Haiku's
 * own _user_* syscalls therefore check IS_USER_ADDRESS() themselves before
 * calling it. Every Renguin handler that touches a pointer supplied by a Linux
 * program goes through renguin_copy_in()/renguin_copy_out(), which do the same
 * checks, so a program cannot read or write kernel memory through a syscall
 * argument. This is the job FreeBSD's copyin()/copyout() do in the
 * Linuxulator.
 *
 * Scratch space: many Haiku _user_* calls take pointers that must be in user
 * space (a size_t* for readlink, a struct for an ioctl, a translated path). The
 * kernel-side equivalents that take kernel buffers are mostly static in
 * vfs.cpp. Rather than add more exports to Haiku, Renguin places such data in
 * the calling thread's own user stack, below the 128-byte System V red zone,
 * which is the same region the kernel uses to build signal frames. Nothing in
 * the program can rely on that memory, and each thread has its own stack, so
 * this is safe with threads too.
 */


#include "renguin.h"

#include <string.h>

#include <KernelExport.h>

#include <arch/x86/arch_thread.h>
#include <arch/x86/64/iframe.h>
#include <kernel.h>
#include <vm/vm.h>


static const size_t kRedZoneSize = 128;
static const size_t kMaxScratch = 64 * 1024;
static const size_t kStackGuardSize = 4 * B_PAGE_SIZE;
	// Haiku's USER_STACK_GUARD_SIZE (headers/private/system/thread_defs.h)


static inline bool
is_user_range(uint64 address, size_t size)
{
	if (size == 0)
		return true;
	if (address + size < address)
		return false;
	return IS_USER_ADDRESS((addr_t)address)
		&& IS_USER_ADDRESS((addr_t)(address + size - 1));
}


status_t
renguin_copy_in(void* to, uint64 userFrom, size_t size)
{
	if (!is_user_range(userFrom, size))
		return B_BAD_ADDRESS;
	if (user_memcpy(to, (const void*)(addr_t)userFrom, size) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


status_t
renguin_copy_out(uint64 userTo, const void* from, size_t size)
{
	if (!is_user_range(userTo, size))
		return B_BAD_ADDRESS;
	if (user_memcpy((void*)(addr_t)userTo, from, size) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


ssize_t
renguin_copy_string_in(char* to, uint64 userFrom, size_t size)
{
	if (userFrom == 0 || !IS_USER_ADDRESS((addr_t)userFrom))
		return B_BAD_ADDRESS;
	ssize_t length = user_strlcpy(to, (const char*)(addr_t)userFrom, size);
	if (length < 0)
		return B_BAD_ADDRESS;
	if ((size_t)length >= size)
		return B_NAME_TOO_LONG;
	return length;
}


void
renguin_scratch_init(renguin_scratch* scratch)
{
	iframe* frame = x86_get_user_iframe();
	scratch->next = frame != NULL ? (addr_t)frame->user_sp - kRedZoneSize : 0;
	scratch->limit = 0;
}


/*!	The lowest address scratch space may use: inside the memory area the
	stack pointer is in, above the guard pages at its bottom (Haiku's stack
	areas and glibc's mmap()ed thread stacks both keep their guard there).
	Scratch space below the stack pointer must never run past the stack
	into whatever is mapped below it: a small thread stack or an alternate
	signal stack is only a few pages.
*/
static addr_t
scratch_limit(addr_t stackPointer)
{
	area_id area = area_for((void*)stackPointer);
	area_info info;
	if (area < 0 || get_area_info(area, &info) != B_OK)
		return (addr_t)-1;
	addr_t base = (addr_t)info.address;
	addr_t limit = base + kStackGuardSize;
	return limit < base ? (addr_t)-1 : limit;
}


uint64
renguin_scratch_alloc(renguin_scratch* scratch, size_t size)
{
	if (scratch->next == 0 || size > kMaxScratch)
		return 0;

	addr_t address = (scratch->next - size) & ~(addr_t)15;
	if (!is_user_range(address, size) || address > scratch->next)
		return 0;

	if (scratch->limit == 0)
		scratch->limit = scratch_limit(scratch->next);
	if (address < scratch->limit)
		return 0;

	scratch->next = address;
	return address;
}


uint64
renguin_scratch_put(renguin_scratch* scratch, const void* data, size_t size)
{
	uint64 address = renguin_scratch_alloc(scratch, size);
	if (address == 0)
		return 0;
	if (user_memcpy((void*)(addr_t)address, data, size) != B_OK)
		return 0;
	return address;
}


/*!	Compare-and-swap on a 32-bit word of the Linux program's memory, for
	futexes. Done by the kernel (see foreign_personality.cpp): a module cannot
	open up user memory itself.
*/
status_t
renguin_user_atomic_cmpxchg(uint64 address, uint32 expected, uint32 newValue,
	uint32* _oldValue)
{
	int32 oldValue;
	status_t status = foreign_personality_user_atomic_test_and_set(
		(int32*)(addr_t)address, (int32)newValue, (int32)expected, &oldValue);
	if (status != B_OK)
		return status;
	*_oldValue = (uint32)oldValue;
	return B_OK;
}
