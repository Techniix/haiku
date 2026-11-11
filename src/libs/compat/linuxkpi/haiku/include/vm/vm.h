/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's freebsd_network compat/vm/vm.h with the
 * FreeBSD VM types LinuxKPI's headers name. LinuxKPI never shares these with
 * Haiku's freebsd_network; they back the page shim (haiku/lkpi_page.c).
 */
#ifndef _LKPI_HAIKU_VM_VM_H_
#define _LKPI_HAIKU_VM_VM_H_

#include_next <vm/vm.h>

typedef size_t		vm_size_t;
typedef uint64_t	vm_pindex_t;
typedef int64_t		vm_ooffset_t;
typedef int		vm_memattr_t;
typedef unsigned char	vm_prot_t;
typedef struct vm_object *vm_object_t;
typedef struct vm_map	*vm_map_t;
typedef struct vm_page	*vm_page_t;

#ifndef VM_PROT_NONE
#define VM_PROT_NONE		((vm_prot_t)0x00)
#define VM_PROT_READ		((vm_prot_t)0x01)
#define VM_PROT_WRITE		((vm_prot_t)0x02)
#define VM_PROT_EXECUTE		((vm_prot_t)0x04)
#define VM_PROT_ALL		(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)
#define VM_PROT_RW		(VM_PROT_READ | VM_PROT_WRITE)
#define VM_PROT_DEFAULT		VM_PROT_ALL
#endif

/* x86 PAT memory attributes, values as FreeBSD x86/include/vm.h. Mapped to
 * Haiku B_MTR_* in lkpi_page.c. */
#define VM_MEMATTR_UNCACHEABLE		0x00
#define VM_MEMATTR_WRITE_COMBINING	0x01
#define VM_MEMATTR_WRITE_THROUGH	0x04
#define VM_MEMATTR_WRITE_PROTECTED	0x05
#define VM_MEMATTR_WRITE_BACK		0x06
#define VM_MEMATTR_WEAK_UNCACHEABLE	0x07
#define VM_MEMATTR_DEFAULT		VM_MEMATTR_WRITE_BACK

#endif
