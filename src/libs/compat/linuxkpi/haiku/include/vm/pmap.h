/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's compat/vm/pmap.h with what LinuxKPI names.
 */
#ifndef _LKPI_HAIKU_VM_PMAP_H_
#define _LKPI_HAIKU_VM_PMAP_H_

#include_next <vm/pmap.h>
#include <vm/vm.h>

struct vm_page;

#ifdef __cplusplus
extern "C" {
#endif
void	*lkpi_phys_to_dmap(vm_paddr_t pa);
int	pmap_change_attr(void *va, vm_size_t size, int mode);
void	pmap_page_set_memattr(struct vm_page *m, vm_memattr_t ma);
void	*pmap_mapdev_attr(vm_paddr_t pa, vm_size_t size, vm_memattr_t ma);
void	pmap_unmapdev(void *va, vm_size_t size);
#ifdef __cplusplus
}
#endif

/* No usable direct map is exported by Haiku: LinuxKPI then maps pages via
 * sf_buf (see sys/sf_buf.h), which we back with the page shim mapping. */
#define PMAP_HAS_DMAP	0
#define PHYS_TO_DMAP_ADDR(pa)	((vm_offset_t)lkpi_phys_to_dmap(pa))
#define PHYS_TO_DMAP(pa)	lkpi_phys_to_dmap(pa)	/* void *, as FreeBSD amd64 now */

#endif
