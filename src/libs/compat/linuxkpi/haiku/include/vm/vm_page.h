/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <vm/vm_page.h>. Shadows Haiku's private C++
 * kernel <vm/vm_page.h>.
 *
 * LinuxKPI defines Linux 'struct page' as FreeBSD 'struct vm_page'
 * (linux/page.h: #define page vm_page). On Haiku there is no exported page
 * array, so a vm_page here is a small descriptor owned by the page shim
 * (haiku/lkpi_page.c): one per Linux page handed out by alloc_pages(),
 * backed by a physically contiguous Haiku area. Fields beyond phys_addr are
 * private to the shim.
 */
#ifndef _LKPI_HAIKU_VM_VM_PAGE_H_
#define _LKPI_HAIKU_VM_VM_PAGE_H_

#include <vm/vm.h>

struct vm_page {
	vm_paddr_t	phys_addr;	/* FreeBSD field name, read by LinuxKPI */
	void		*lkpi_kva;	/* kernel mapping */
	area_id		lkpi_area;	/* backing area (head page only) */
	unsigned int	lkpi_order;	/* allocation order (head page only) */
	int		ref_count;
	vm_memattr_t	lkpi_memattr;
	struct vm_page	*lkpi_head;	/* head page of a compound allocation */
	unsigned long	flags;
	void		*object;	/* unused on Haiku, kept for LinuxKPI */
	vm_pindex_t	pindex;
	struct vm_page	*lkpi_pnext;	/* lkpi_page.c: pfn hash chain */
	/* LinuxKPI page_pool (PAGE_IS_LKPI_PAGE below). */
	uint64_t	dma_addr;	/* dma_addr_t; set by page_pool */
	struct page_pool *pp;
};

/*
 * LinuxKPI's page_pool (linuxkpi_page_pool.c) and skb page recycling only
 * work when struct page is LinuxKPI's own structure with dma_addr/pp; that
 * is FreeBSD work in progress, so it is off there. On Haiku struct page is
 * this structure, so turn it on. Without it page_pool_alloc_frag() returns
 * NULL and mt76 cannot fill its RX rings: the card then has nowhere to put
 * MCU replies (first hardware run: every MCU command timed out).
 */
#define PAGE_IS_LKPI_PAGE	1

struct page_pool;

#define VM_PAGE_TO_PHYS(m)	((m)->phys_addr)
#define PHYS_TO_VM_PAGE(pa)	lkpi_phys_to_vm_page(pa)

#define VM_ALLOC_NORMAL		0x0000
#define VM_ALLOC_INTERRUPT	0x0001
#define VM_ALLOC_SYSTEM		0x0002
#define VM_ALLOC_WIRED		0x0020
#define VM_ALLOC_ZERO		0x0040
#define VM_ALLOC_NOWAIT		0x8000

#ifdef __cplusplus
extern "C" {
#endif
struct vm_page	*lkpi_phys_to_vm_page(vm_paddr_t pa);
void	vm_page_wire(struct vm_page *m);
bool	vm_page_unwire_noq(struct vm_page *m);
int	vm_page_tryxbusy(struct vm_page *m);
void	vm_page_xunbusy(struct vm_page *m);
void	vm_page_reference(struct vm_page *m);
void	vm_page_dirty(struct vm_page *m);
#ifdef __cplusplus
}
#endif

#endif
