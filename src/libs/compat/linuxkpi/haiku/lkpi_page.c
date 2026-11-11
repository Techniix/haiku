/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux page model for LinuxKPI on Haiku (replaces FreeBSD's
 * linux_page.c, which is built on FreeBSD's vm_page array and direct map).
 *
 * alloc_pages(gfp, order) takes 2^order physically contiguous pages from a
 * Haiku B_CONTIGUOUS area (freebsd_network's contigmalloc(), honouring
 * GFP_DMA32 as a 4 GiB physical limit) and gives each page a small
 * struct vm_page (== Linux struct page, include/vm/vm_page.h) that records
 * its physical and kernel address. Pages are found again by physical frame
 * number (pfn hash), which is all virt_to_page()/PHYS_TO_VM_PAGE() need:
 * Haiku's vtophys() (pmap_kextract) gives the physical address of any
 * kernel virtual address.
 *
 * Known limits (docs/STATUS.md): one Haiku area per allocation; memory
 * attribute changes (WC/UC) are not applied yet; vmap/user-page pinning
 * are not implemented (DRM-only).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>

#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#include <KernelExport.h>

MALLOC_DEFINE(M_LKPI_PAGE, "lkpipage", "LinuxKPI page descriptors");

/*
 * Offset bits within a page. Not PAGE_MASK: LinuxKPI's linux/page.h (pulled
 * in via linux/mm.h) redefines PAGE_MASK with Linux's meaning, ~(PAGE_SIZE-1),
 * the opposite of FreeBSD's. Using it here made every virt_to_page() lookup
 * miss (panic in page_pool_get_dma_addr on the first RX ring fill).
 */
#define LKPI_PAGE_OFFSET_MASK	((vm_paddr_t)PAGE_SIZE - 1)

#define LKPI_PAGE_HASH	4096
static struct vm_page *lkpi_page_hash[LKPI_PAGE_HASH];
static struct mtx lkpi_page_lock;

static inline u_int
lkpi_pfn_hash(vm_paddr_t pa)
{
	return ((pa >> PAGE_SHIFT) % LKPI_PAGE_HASH);
}

static void
lkpi_page_insert(struct vm_page *m)
{
	u_int h = lkpi_pfn_hash(m->phys_addr);

	m->lkpi_pnext = lkpi_page_hash[h];
	lkpi_page_hash[h] = m;
}

static void
lkpi_page_remove(struct vm_page *m)
{
	struct vm_page **p = &lkpi_page_hash[lkpi_pfn_hash(m->phys_addr)];

	for (; *p != NULL; p = &(*p)->lkpi_pnext) {
		if (*p == m) {
			*p = m->lkpi_pnext;
			return;
		}
	}
}

struct vm_page *
lkpi_phys_to_vm_page(vm_paddr_t pa)
{
	struct vm_page *m;

	pa &= ~LKPI_PAGE_OFFSET_MASK;
	mtx_lock(&lkpi_page_lock);
	for (m = lkpi_page_hash[lkpi_pfn_hash(pa)]; m != NULL; m = m->lkpi_pnext) {
		if (m->phys_addr == pa)
			break;
	}
	mtx_unlock(&lkpi_page_lock);
	return (m);
}

void *
lkpi_phys_to_dmap(vm_paddr_t pa)
{
	struct vm_page *m = lkpi_phys_to_vm_page(pa);

	/* Only pages we allocated have a known mapping (PMAP_HAS_DMAP is 0). */
	return (m != NULL ? (char *)m->lkpi_kva + (pa & LKPI_PAGE_OFFSET_MASK) : NULL);
}

/*
 * 2^order contiguous pages at or below physical address 'high'. Also used by
 * lkpi_dma.c for coherent DMA memory (freed again with kmem_free()).
 */
struct page *
lkpi_alloc_pages_below(gfp_t flags, unsigned int order, vm_paddr_t high)
{
	unsigned long npages = 1UL << order, i;
	size_t size = (size_t)PAGE_SIZE << order;
	struct vm_page *pages;
	char *va;

	va = contigmalloc(size, M_LKPI_PAGE,
	    (flags & (M_NOWAIT | M_WAITOK | M_ZERO)), 0, high, PAGE_SIZE, 0);
	if (va == NULL)
		return (NULL);

	pages = malloc(npages * sizeof(*pages), M_LKPI_PAGE,
	    ((flags & M_NOWAIT) != 0 ? M_NOWAIT : M_WAITOK) | M_ZERO);
	if (pages == NULL) {
		contigfree(va, size, M_LKPI_PAGE);
		return (NULL);
	}

	mtx_lock(&lkpi_page_lock);
	for (i = 0; i < npages; i++) {
		pages[i].lkpi_kva = va + i * PAGE_SIZE;
		pages[i].phys_addr = vtophys(pages[i].lkpi_kva);
		pages[i].lkpi_head = &pages[0];
		pages[i].lkpi_memattr = VM_MEMATTR_DEFAULT;
		pages[i].ref_count = 1;
		lkpi_page_insert(&pages[i]);
	}
	pages[0].lkpi_order = order;
	mtx_unlock(&lkpi_page_lock);
	return (&pages[0]);
}

struct page *
linux_alloc_pages(gfp_t flags, unsigned int order)
{
	struct page *page;

	if ((flags & GFP_DMA32) != 0)
		return (lkpi_alloc_pages_below(flags, order,
		    BUS_SPACE_MAXADDR_32BIT));

	/*
	 * Prefer memory below 4 GiB: devices with a 32-bit DMA mask (the
	 * MT7921/MT7922 among them) can then use pages such as page_pool RX
	 * buffers directly instead of through a bounce buffer (lkpi_dma.c).
	 */
	page = lkpi_alloc_pages_below((flags & ~M_WAITOK) | M_NOWAIT, order,
	    BUS_SPACE_MAXADDR_32BIT);
	if (page == NULL)
		page = lkpi_alloc_pages_below(flags, order, BUS_SPACE_MAXADDR);
	return (page);
}

void
linux_free_pages(struct page *page, unsigned int order)
{
	struct vm_page *head;
	unsigned long npages, i;

	if (page == NULL)
		return;
	head = page->lkpi_head;
	/*
	 * As FreeBSD's linux_free_pages(): drop one reference and free only
	 * when it was the last. put_page()/__free_page() end up here, and pages
	 * are shared (get_page(), skb fragments, page_pool RX buffers), so
	 * freeing unconditionally was a use-after-free/double free. The
	 * reference count of an allocation lives on its head page; a reference
	 * dropped through a tail page counts against the head too.
	 */
	if (!vm_page_unwire_noq(head))
		return;
	npages = 1UL << head->lkpi_order;

	mtx_lock(&lkpi_page_lock);
	for (i = 0; i < npages; i++)
		lkpi_page_remove(&head[i]);
	mtx_unlock(&lkpi_page_lock);

	contigfree(head->lkpi_kva, (size_t)PAGE_SIZE << head->lkpi_order,
	    M_LKPI_PAGE);
	free(head, M_LKPI_PAGE);
}

void *
linux_page_address(const struct page *page)
{
	return (page->lkpi_kva);
}

vm_offset_t
linux_alloc_kmem(gfp_t flags, unsigned int order)
{
	struct page *page = linux_alloc_pages(flags, order);

	return (page != NULL ? (vm_offset_t)page->lkpi_kva : 0);
}

void
linux_free_kmem(vm_offset_t addr, unsigned int order)
{
	linux_free_pages(PHYS_TO_VM_PAGE(vtophys(addr)), order);
}

void
kmem_free(void *addr, vm_size_t size)
{
	linux_free_kmem((vm_offset_t)addr, 0);
}

void
linux_release_pages(release_pages_arg arg, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		__free_page(arg.pages[i]);
}

/* FreeBSD vm_page_* the LinuxKPI headers call (get_page/put_page &c.). */
void vm_page_wire(struct vm_page *m) { atomic_add_int(&m->lkpi_head->ref_count, 1); }
bool
vm_page_unwire_noq(struct vm_page *m)
{
	return (atomic_fetchadd_int(&m->lkpi_head->ref_count, -1) == 1);
}
int vm_page_tryxbusy(struct vm_page *m) { return (1); }
void vm_page_xunbusy(struct vm_page *m) { }
void vm_page_reference(struct vm_page *m) { }
void vm_page_dirty(struct vm_page *m) { }

int
is_vmalloc_addr(const void *addr)
{
	return (0);	/* LinuxKPI's vmalloc is malloc(9) */
}

void
si_meminfo(struct sysinfo *si)
{
	system_info info;

	get_system_info(&info);
	si->totalram = info.max_pages;
	si->freeram = info.max_pages - info.used_pages;
	si->totalhigh = 0;
	si->freehigh = 0;
	si->mem_unit = PAGE_SIZE;
}

/* ---- MMIO mappings -------------------------------------------------------- */

void *
_ioremap_attr(vm_paddr_t phys_addr, unsigned long size, int attr)
{
	return (pmap_mapdev_attr(phys_addr, size, attr));
}

void
iounmap(void *addr)
{
	pmap_unmapdev(addr, 0);
}

void *
pmap_mapdev_attr(vm_paddr_t pa, vm_size_t size, vm_memattr_t ma)
{
	vm_paddr_t base = pa & ~LKPI_PAGE_OFFSET_MASK;
	uint32 mt;
	void *va;
	area_id area;

	switch (ma) {
	case VM_MEMATTR_WRITE_COMBINING:	mt = B_WRITE_COMBINING_MEMORY; break;
	case VM_MEMATTR_WRITE_BACK:		mt = B_WRITE_BACK_MEMORY; break;
	default:				mt = B_UNCACHED_MEMORY; break;
	}
	area = map_physical_memory("lkpi ioremap", base,
	    roundup2(size + (pa - base), PAGE_SIZE), B_ANY_KERNEL_ADDRESS | mt,
	    B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &va);
	if (area < 0)
		return (NULL);
	return ((char *)va + (pa - base));
}

void
pmap_unmapdev(void *va, vm_size_t size)
{
	area_id area = area_for(va);

	if (area >= 0)
		delete_area(area);
}

int
pmap_change_attr(void *va, vm_size_t size, int mode)
{
	return (0);	/* TODO(stage d+): Haiku vm_set_area_memory_type() */
}

void
pmap_page_set_memattr(struct vm_page *m, vm_memattr_t ma)
{
	m->lkpi_memattr = ma;
}

int
lkpi_set_pages_attr(struct page *page, int numpages, vm_memattr_t ma)
{
	int i;

	for (i = 0; i < numpages; i++)
		pmap_page_set_memattr(&page[i], ma);
	return (0);
}

/* ---- page_frag (FreeBSD linux_page.c, unchanged logic) ------------------- */

void *
linuxkpi_page_frag_alloc(struct page_frag_cache *pfc, size_t fragsz, gfp_t gfp)
{
	struct page *pages;

	if (fragsz == 0 || fragsz > PAGE_SIZE)
		return (NULL);
	pages = alloc_pages(gfp, 0);
	if (pages == NULL)
		return (NULL);
	pfc->va = linux_page_address(pages);
	pfc->pagecnt_bias = 0;
	return (pfc->va);
}

void
linuxkpi_page_frag_free(void *addr)
{
	linux_free_pages(virt_to_page(addr), 0);
}

void
linuxkpi__page_frag_cache_drain(struct page *page, size_t count __unused)
{
	linux_free_pages(page, 0);
}

static void
lkpi_page_init(void *arg)
{
	mtx_init(&lkpi_page_lock, "lkpi pages", NULL, MTX_DEF);
}
SYSINIT(lkpi_page, SI_SUB_DRIVERS, SI_ORDER_FIRST, lkpi_page_init, NULL);

static void
lkpi_page_uninit(void *arg)
{
	mtx_destroy(&lkpi_page_lock);
}
SYSUNINIT(lkpi_page, SI_SUB_DRIVERS, SI_ORDER_FIRST, lkpi_page_uninit, NULL);
