/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Parts follow FreeBSD sys/compat/linuxkpi/common/src/linux_pci.c (DMA
 * section; BSD-2-Clause, see IMPORTED.md), in particular the dmam_* devres
 * helpers, which are close to verbatim.
 *
 * The LinuxKPI DMA API (linux/dma-mapping.h, linux/dmapool.h) for Haiku.
 *
 * FreeBSD implements this on busdma. Haiku's freebsd_network busdma has no
 * bus_dmamap_load_phys() and keeps bounce state per map, so this file does
 * the (small) job directly, for what Haiku runs on today:
 *
 *  - x86/x86-64 DMA is cache-coherent and Haiku programs no IOMMU, so a bus
 *    address is the physical address and every sync is a no-op ... [V]
 *  - ... unless the range lies above the device's DMA mask (the MT7921 and
 *    MT7922 set a 32-bit mask). Then the mapping is bounced through
 *    contiguous memory below the mask, copied with Haiku's
 *    vm_memcpy_{to,from}_physical() at map, sync and unmap time.
 *
 * Bounces up to LKPI_BOUNCE_SLOT bytes (every normal frame) take a slot from
 * a per-device pool below the mask, allocated on first use; only larger
 * ones (firmware download chunks) get their own contigmalloc().
 *
 * Coherent memory is allocated below the coherent mask, so it never bounces.
 * lkpi_page.c prefers memory below 4 GiB for alloc_pages(), so page_pool RX
 * buffers normally do not bounce either.
 *
 * Callers rely on dma_map_single() of kmalloc()ed memory being physically
 * contiguous: kmalloc() above PAGE_SIZE is contigmalloc() (linux_slab.c),
 * and freebsd_network's malloc() returns power-of-two aligned blocks up to a
 * page (malloc.cpp), which therefore never cross a page boundary. [V]
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>

/* Haiku kernel, headers/private/kernel/vm/vm.h (extern "C"). */
int32_t	vm_memcpy_from_physical(void *to, uint64_t from, size_t length,
	    bool user);
int32_t	vm_memcpy_to_physical(uint64_t to, const void *from, size_t length,
	    bool user);

/* lkpi_page.c */
struct page *lkpi_alloc_pages_below(gfp_t flags, unsigned int order,
    vm_paddr_t high);

MALLOC_DEFINE(M_LKPI_DMA, "lkpidma", "LinuxKPI DMA bounce bookkeeping");

/* One bounced mapping. */
struct lkpi_bounce {
	struct lkpi_bounce	*next;	/* "large" list only */
	dma_addr_t		dma;	/* physical address of the bounce copy */
	vm_paddr_t		orig;	/* the caller's physical range */
	size_t			len;
	size_t			alloc;	/* contigmalloc size; 0: pool slot */
	int			slot;	/* pool slot index if alloc == 0 */
	void			*kva;	/* kernel address of the bounce copy */
};

#define	LKPI_BOUNCE_SLOT	4096
#define	LKPI_BOUNCE_SLOTS	512		/* 2 MiB per device */

struct linux_dma_priv {
	uint64_t		dma_mask;
	uint64_t		dma_coherent_mask;
	struct mtx		lock;
	u_int			nbounce;
	/*
	 * Outstanding bounces. A bounce address always lies in memory we
	 * allocated (pool or a contigmalloc block), never in a caller's buffer,
	 * so an address is classified in O(1): inside the pool -> pool_owner[]
	 * by slot; otherwise only the (rare) "large" list is searched. Most
	 * unmaps/syncs are of identity mappings and must stay cheap: they run
	 * per packet.
	 */
	struct lkpi_bounce	*pool_owner[LKPI_BOUNCE_SLOTS];
	struct lkpi_bounce	*large;
	/* bounce pool (lkpi_bounce_pool_get()) */
	void			*pool_kva;
	dma_addr_t		pool_dma;
	bool			pool_failed;
	int			pool_nfree;
	int			pool_free[LKPI_BOUNCE_SLOTS];
};

/* Called with priv->lock held. Returns a slot index or -1. */
static int
lkpi_bounce_pool_get(struct linux_dma_priv *priv)
{
	int i;

	if (priv->pool_kva == NULL) {
		if (priv->pool_failed)
			return (-1);
		priv->pool_kva = contigmalloc(
		    (size_t)LKPI_BOUNCE_SLOT * LKPI_BOUNCE_SLOTS, M_LKPI_DMA,
		    M_NOWAIT, 0, priv->dma_mask, PAGE_SIZE, 0);
		if (priv->pool_kva == NULL) {
			priv->pool_failed = true;
			return (-1);
		}
		priv->pool_dma = vtophys(priv->pool_kva);
		for (i = 0; i < LKPI_BOUNCE_SLOTS; i++)
			priv->pool_free[i] = LKPI_BOUNCE_SLOTS - 1 - i;
		priv->pool_nfree = LKPI_BOUNCE_SLOTS;
	}
	if (priv->pool_nfree == 0)
		return (-1);
	return (priv->pool_free[--priv->pool_nfree]);
}

static void
lkpi_bounce_pool_put(struct linux_dma_priv *priv, int slot)
{
	priv->pool_free[priv->pool_nfree++] = slot;
}

static uint64_t lkpi_dma_bounces, lkpi_dma_pool_misses, lkpi_dma_maps;
static u_int lkpi_dma_bounce_logged;

/* ---- per-device state (called from lkpi_pci.c) ------------------------- */

int
linux_pdev_dma_init(struct pci_dev *pdev)
{
	struct linux_dma_priv *priv;

	priv = malloc(sizeof(*priv), M_LKPI_DMA, M_WAITOK | M_ZERO);
	mtx_init(&priv->lock, "lkpi-dma", NULL, MTX_DEF);
	/* Linux defaults: 64-bit streaming, 32-bit coherent. */
	priv->dma_mask = DMA_BIT_MASK(64);
	priv->dma_coherent_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_priv = priv;
	return (0);
}

int
linux_pdev_dma_uninit(struct pci_dev *pdev)
{
	struct linux_dma_priv *priv = pdev->dev.dma_priv;

	if (priv == NULL)
		return (0);
	if (priv->nbounce != 0)
		dev_warn(&pdev->dev, "lkpi_dma: %u bounced mappings were never "
		    "unmapped; leaking them\n", priv->nbounce);
	else if (priv->pool_kva != NULL)
		contigfree(priv->pool_kva,
		    (size_t)LKPI_BOUNCE_SLOT * LKPI_BOUNCE_SLOTS, M_LKPI_DMA);
	mtx_destroy(&priv->lock);
	pdev->dev.dma_priv = NULL;
	free(priv, M_LKPI_DMA);
	return (0);
}

int
linux_dma_tag_init(struct device *dev, u64 dma_mask)
{
	struct linux_dma_priv *priv = dev->dma_priv;

	priv->dma_mask = dma_mask;
	return (0);
}

int
linux_dma_tag_init_coherent(struct device *dev, u64 dma_mask)
{
	struct linux_dma_priv *priv = dev->dma_priv;

	priv->dma_coherent_mask = dma_mask;
	return (0);
}

/* ---- streaming mappings ------------------------------------------------- */

/* Copy between the caller's memory and the bounce copy, for [off, off+len). */
static void
lkpi_bounce_to_device(struct lkpi_bounce *b, size_t off, size_t len)
{
	vm_memcpy_from_physical((char *)b->kva + off, b->orig + off, len, false);
}

static void
lkpi_bounce_to_cpu(struct lkpi_bounce *b, size_t off, size_t len)
{
	vm_memcpy_to_physical(b->orig + off, (char *)b->kva + off, len, false);
}

/*
 * Called with priv->lock held. Finds the bounce mapping containing dma
 * (dma_sync_single_range_*() may pass an address inside a mapping). For the
 * "large" list, *prevp is set to the link pointing at the result.
 */
static struct lkpi_bounce *
lkpi_bounce_find(struct linux_dma_priv *priv, dma_addr_t dma,
    struct lkpi_bounce ***prevp)
{
	struct lkpi_bounce **pp, *b;

	if (prevp != NULL)
		*prevp = NULL;
	if (priv->pool_kva != NULL && dma >= priv->pool_dma &&
	    dma < priv->pool_dma + (dma_addr_t)LKPI_BOUNCE_SLOT *
	    LKPI_BOUNCE_SLOTS) {
		b = priv->pool_owner[(dma - priv->pool_dma) / LKPI_BOUNCE_SLOT];
		if (b != NULL && dma < b->dma + b->len)
			return (b);
		return (NULL);
	}
	for (pp = &priv->large; (b = *pp) != NULL; pp = &b->next) {
		if (dma >= b->dma && dma < b->dma + b->len) {
			if (prevp != NULL)
				*prevp = pp;
			return (b);
		}
	}
	return (NULL);
}

static bool
lkpi_dma_to_device(enum dma_data_direction dir)
{
	return (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL ||
	    dir == DMA_NONE);
}

static bool
lkpi_dma_to_cpu(enum dma_data_direction dir)
{
	return (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL ||
	    dir == DMA_NONE);
}

dma_addr_t
lkpi_dma_map_phys(struct device *dev, vm_paddr_t phys, size_t len,
    enum dma_data_direction direction, unsigned long attrs)
{
	struct linux_dma_priv *priv = dev->dma_priv;
	struct lkpi_bounce *b;
	if (priv == NULL || len == 0)
		return (0);
	__atomic_fetch_add(&lkpi_dma_maps, 1, __ATOMIC_RELAXED);
	/* Identity mapping: bus address == physical address (no IOMMU). */
	if (phys + len - 1 <= priv->dma_mask)
		return (phys);

	b = malloc(sizeof(*b), M_LKPI_DMA, M_NOWAIT | M_ZERO);
	if (b == NULL)
		return (0);
	b->slot = -1;
	if (len <= LKPI_BOUNCE_SLOT) {
		mtx_lock(&priv->lock);
		b->slot = lkpi_bounce_pool_get(priv);
		mtx_unlock(&priv->lock);
	}
	if (b->slot >= 0) {
		b->alloc = 0;
		b->kva = (char *)priv->pool_kva +
		    (size_t)b->slot * LKPI_BOUNCE_SLOT;
		b->dma = priv->pool_dma + (dma_addr_t)b->slot * LKPI_BOUNCE_SLOT;
	} else {
		b->alloc = round_page(len);
		b->kva = contigmalloc(b->alloc, M_LKPI_DMA, M_NOWAIT, 0,
		    priv->dma_mask, PAGE_SIZE, 0);
		if (b->kva == NULL) {
			free(b, M_LKPI_DMA);
			return (0);
		}
		b->dma = vtophys(b->kva);
	}
	b->orig = phys;
	b->len = len;

	if (atomic_cmpset_int(&lkpi_dma_bounce_logged, 0, 1))
		dev_info(dev, "lkpi_dma: bouncing %#llx+%zu above DMA mask %#llx "
		    "(logged once)\n", (unsigned long long)phys, len,
		    (unsigned long long)priv->dma_mask);
	/* Speed diagnostics: how often we copy, and how often the pool is
	 * exhausted (each miss is a contigmalloc(), i.e. a new area). */
	{
		uint64_t nb = __atomic_add_fetch(&lkpi_dma_bounces, 1,
		    __ATOMIC_RELAXED);

		if (b->slot < 0)
			__atomic_fetch_add(&lkpi_dma_pool_misses, 1,
			    __ATOMIC_RELAXED);
		if ((nb & 0x3fff) == 0)
			dev_info(dev, "lkpi_dma: %llu bounces, %llu pool misses, "
			    "%llu maps total\n", (unsigned long long)nb,
			    (unsigned long long)lkpi_dma_pool_misses,
			    (unsigned long long)lkpi_dma_maps);
	}

	if ((attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0 &&
	    lkpi_dma_to_device(direction))
		lkpi_bounce_to_device(b, 0, len);

	mtx_lock(&priv->lock);
	if (b->slot >= 0) {
		priv->pool_owner[b->slot] = b;
	} else {
		b->next = priv->large;
		priv->large = b;
	}
	priv->nbounce++;
	mtx_unlock(&priv->lock);
	return (b->dma);
}

dma_addr_t
linux_dma_map_phys(struct device *dev, vm_paddr_t phys, size_t len)
{
	return (lkpi_dma_map_phys(dev, phys, len, DMA_NONE, 0));
}

void
lkpi_dma_unmap(struct device *dev, dma_addr_t dma_addr, size_t len,
    enum dma_data_direction direction, unsigned long attrs)
{
	struct linux_dma_priv *priv = dev->dma_priv;
	struct lkpi_bounce *b, **pp;

	if (priv == NULL || priv->nbounce == 0)
		return;
	mtx_lock(&priv->lock);
	b = lkpi_bounce_find(priv, dma_addr, &pp);
	if (b == NULL || b->dma != dma_addr) {
		mtx_unlock(&priv->lock);
		return;		/* identity mapping: nothing to undo */
	}
	if (b->slot >= 0)
		priv->pool_owner[b->slot] = NULL;
	else
		*pp = b->next;
	priv->nbounce--;
	mtx_unlock(&priv->lock);

	if ((attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0 &&
	    lkpi_dma_to_cpu(direction))
		lkpi_bounce_to_cpu(b, 0, b->len);
	if (b->alloc == 0) {
		mtx_lock(&priv->lock);
		lkpi_bounce_pool_put(priv, b->slot);
		mtx_unlock(&priv->lock);
	} else
		contigfree(b->kva, b->alloc, M_LKPI_DMA);
	free(b, M_LKPI_DMA);
}

void
linux_dma_unmap(struct device *dev, dma_addr_t dma_addr, size_t len)
{
	lkpi_dma_unmap(dev, dma_addr, len, DMA_NONE, 0);
}

void
linuxkpi_dma_sync(struct device *dev, dma_addr_t dma_addr, size_t size,
    bus_dmasync_op_t op)
{
	struct linux_dma_priv *priv = dev->dma_priv;
	struct lkpi_bounce *b;
	size_t off;

	if (priv == NULL || priv->nbounce == 0)
		return;
	mtx_lock(&priv->lock);
	b = lkpi_bounce_find(priv, dma_addr, NULL);
	if (b != NULL) {
		off = dma_addr - b->dma;
		if (size > b->len - off)
			size = b->len - off;
		/* POST*: device is done, CPU looks; PREWRITE: CPU wrote. */
		if ((op & BUS_DMASYNC_POSTREAD) != 0)
			lkpi_bounce_to_cpu(b, off, size);
		if ((op & BUS_DMASYNC_PREWRITE) != 0)
			lkpi_bounce_to_device(b, off, size);
	}
	mtx_unlock(&priv->lock);
}

/* ---- scatter/gather ------------------------------------------------------ */

/*
 * FreeBSD keeps one busdma map for the whole list in sgl->dma_map, and
 * dma_sync_sg_*() pass only the list head. We keep the entry count there
 * instead (the field is opaque to LinuxKPI users).
 */
static inline void
lkpi_sg_set_nents(struct scatterlist *sgl, int nents)
{
	sgl->dma_map = (struct bus_dmamap *)(uintptr_t)nents;
}

static inline int
lkpi_sg_nents(struct scatterlist *sgl)
{
	return ((int)(uintptr_t)sgl->dma_map);
}

int
linux_dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
    enum dma_data_direction direction, unsigned long attrs)
{
	struct scatterlist *sg;
	int i, j;

	for_each_sg(sgl, sg, nents, i) {
		sg_dma_address(sg) = lkpi_dma_map_phys(dev, sg_phys(sg),
		    sg->length, direction, attrs);
		if (sg_dma_address(sg) == 0) {
			for_each_sg(sgl, sg, i, j)
				lkpi_dma_unmap(dev, sg_dma_address(sg),
				    sg->length, direction,
				    DMA_ATTR_SKIP_CPU_SYNC);
			return (0);
		}
		sg->dma_length = sg->length;
	}
	lkpi_sg_set_nents(sgl, nents);
	return (nents);
}

void
linux_dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sgl,
    int nents, enum dma_data_direction direction, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	if (lkpi_sg_nents(sgl) > 0)
		nents = lkpi_sg_nents(sgl);
	for_each_sg(sgl, sg, nents, i)
		lkpi_dma_unmap(dev, sg_dma_address(sg), sg->length, direction,
		    attrs);
	lkpi_sg_set_nents(sgl, 0);
}

void
lkpi_dma_sync_sg(struct device *dev, struct scatterlist *sgl,
    bus_dmasync_op_t op)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, lkpi_sg_nents(sgl), i)
		linuxkpi_dma_sync(dev, sg_dma_address(sg), sg->length, op);
}

/* ---- coherent and non-coherent allocations ------------------------------- */

static void *
lkpi_dma_alloc_below(struct device *dev, size_t size, dma_addr_t *dma_handle,
    gfp_t flag, uint64_t mask)
{
	struct page *page;

	if (dev == NULL || dev->dma_priv == NULL || size == 0) {
		*dma_handle = 0;
		return (NULL);
	}
	/* Always zeroed, physically contiguous, aligned to its size order. */
	page = lkpi_alloc_pages_below((flag & GFP_NATIVE_MASK) | M_ZERO,
	    get_order(size), mask);
	if (page == NULL) {
		*dma_handle = 0;
		return (NULL);
	}
	*dma_handle = page_to_phys(page);
	return (page_address(page));
}

void *
linux_dma_alloc_coherent(struct device *dev, size_t size,
    dma_addr_t *dma_handle, gfp_t flag)
{
	struct linux_dma_priv *priv;

	if (dev == NULL || (priv = dev->dma_priv) == NULL) {
		*dma_handle = 0;
		return (NULL);
	}
	return (lkpi_dma_alloc_below(dev, size, dma_handle, flag,
	    priv->dma_coherent_mask != 0 ? priv->dma_coherent_mask :
	    BUS_SPACE_MAXADDR_32BIT));
}

void *
linuxkpi_dma_alloc_noncoherent(struct device *dev, size_t size,
    dma_addr_t *dma_handle, enum dma_data_direction direction, gfp_t gfp)
{
	struct linux_dma_priv *priv;

	if (dev == NULL || (priv = dev->dma_priv) == NULL) {
		*dma_handle = 0;
		return (NULL);
	}
	/* Below the streaming mask, so it maps 1:1 and never bounces. */
	return (lkpi_dma_alloc_below(dev, PAGE_ALIGN(size), dma_handle, gfp,
	    priv->dma_mask));
}

void
linuxkpi_dma_free_noncoherent(struct device *dev, size_t size, void *vaddr,
    dma_addr_t dma_handle, enum dma_data_direction direction)
{
	kmem_free(vaddr, PAGE_ALIGN(size));
}

void *
linuxkpi_dma_alloc_attrs(struct device *dev, size_t size,
    dma_addr_t *dma_handle, gfp_t gfp, unsigned long attrs)
{
	return (linuxkpi_dma_alloc_noncoherent(dev, size, dma_handle,
	    DMA_BIDIRECTIONAL, gfp));
}

void
linuxkpi_dma_free_attrs(struct device *dev, size_t size, void *vaddr,
    dma_addr_t dma_handle, unsigned long attrs)
{
	linuxkpi_dma_free_noncoherent(dev, size, vaddr, dma_handle,
	    DMA_BIDIRECTIONAL);
}

/* dmam_*: from FreeBSD linux_pci.c. */
struct lkpi_devres_dmam_coherent {
	size_t size;
	dma_addr_t handle;
	void *mem;
};

static void
lkpi_dmam_free_coherent(struct device *dev, void *p)
{
	struct lkpi_devres_dmam_coherent *dr;

	dr = p;
	dma_free_coherent(dev, dr->size, dr->mem, dr->handle);
}

static int
lkpi_dmam_coherent_match(struct device *dev, void *dr, void *mp)
{
	struct lkpi_devres_dmam_coherent *a, *b;

	a = dr;
	b = mp;

	if (a->mem != b->mem)
		return (0);
	if (a->size != b->size || a->handle != b->handle)
		dev_WARN(dev, "for mem %p: size %zu != %zu || handle %#jx != %#jx\n",
		    a->mem, a->size, b->size,
		    (uintmax_t)a->handle, (uintmax_t)b->handle);
	return (1);
}

void
linuxkpi_dmam_free_coherent(struct device *dev, size_t size,
    void *addr, dma_addr_t dma_handle)
{
	struct lkpi_devres_dmam_coherent match = {
		.size		= size,
		.handle		= dma_handle,
		.mem		= addr
	};
	int error;

	error = devres_destroy(dev, lkpi_dmam_free_coherent,
	    lkpi_dmam_coherent_match, &match);
	if (error != 0)
		dev_WARN(dev, "devres_destroy returned %d, size %zu addr %p "
		    "dma_handle %#jx\n", error, size, addr, (uintmax_t)dma_handle);
	dma_free_coherent(dev, size, addr, dma_handle);
}

void *
linuxkpi_dmam_alloc_coherent(struct device *dev, size_t size,
    dma_addr_t *dma_handle, gfp_t flag)
{
	struct lkpi_devres_dmam_coherent *dr;

	dr = lkpi_devres_alloc(lkpi_dmam_free_coherent,
	    sizeof(*dr), GFP_KERNEL | __GFP_ZERO);

	if (dr == NULL)
		return (NULL);

	dr->size = size;
	dr->mem = linux_dma_alloc_coherent(dev, size, dma_handle, flag);
	dr->handle = *dma_handle;
	if (dr->mem == NULL) {
		lkpi_devres_free(dr);
		return (NULL);
	}

	lkpi_devres_add(dev, dr);
	return (dr->mem);
}

/* ---- dma_pool ------------------------------------------------------------ */

/*
 * Simple pools: objects are carved from coherent pages, one free list per
 * pool. Pages are only returned on dma_pool_destroy(). (Not used by mt76;
 * kept small on purpose.)
 */
struct lkpi_pool_page {
	struct lkpi_pool_page	*next;
	void			*kva;
	dma_addr_t		dma;
	size_t			bytes;
};

struct lkpi_pool_obj {
	struct lkpi_pool_obj	*next;
	dma_addr_t		dma;
};

struct dma_pool {
	struct device		*dev;
	size_t			size;	/* object stride */
	size_t			page_bytes;
	size_t			boundary;
	struct mtx		lock;
	struct lkpi_pool_page	*pages;
	struct lkpi_pool_obj	*free;
};

struct dma_pool *
linux_dma_pool_create(char *name, struct device *dev, size_t size,
    size_t align, size_t boundary)
{
	struct dma_pool *pool;

	if (align == 0)
		align = 1;
	if ((align & (align - 1)) != 0)
		return (NULL);
	size = roundup2(MAX(size, sizeof(struct lkpi_pool_obj)), align);
	if (boundary != 0 && size > boundary)
		return (NULL);

	pool = malloc(sizeof(*pool), M_LKPI_DMA, M_WAITOK | M_ZERO);
	pool->dev = dev;
	pool->size = size;
	pool->boundary = boundary;
	pool->page_bytes = MAX(PAGE_SIZE, roundup_pow_of_two(size));
	mtx_init(&pool->lock, "lkpi-dmapool", NULL, MTX_DEF);
	return (pool);
}

static int
lkpi_dma_pool_grow(struct dma_pool *pool, gfp_t mem_flags)
{
	struct lkpi_pool_page *pg;
	struct lkpi_pool_obj *obj;
	size_t off;

	pg = malloc(sizeof(*pg), M_LKPI_DMA, M_NOWAIT | M_ZERO);
	if (pg == NULL)
		return (-ENOMEM);
	pg->bytes = pool->page_bytes;
	pg->kva = linux_dma_alloc_coherent(pool->dev, pg->bytes, &pg->dma,
	    mem_flags);
	if (pg->kva == NULL) {
		free(pg, M_LKPI_DMA);
		return (-ENOMEM);
	}
	for (off = 0; off + pool->size <= pg->bytes; off += pool->size) {
		/* Objects must not cross the pool's boundary. */
		if (pool->boundary != 0 && (off / pool->boundary) !=
		    ((off + pool->size - 1) / pool->boundary)) {
			off = roundup2(off, pool->boundary) - pool->size;
			continue;
		}
		obj = (struct lkpi_pool_obj *)((char *)pg->kva + off);
		obj->dma = pg->dma + off;
		obj->next = pool->free;
		pool->free = obj;
	}
	pg->next = pool->pages;
	pool->pages = pg;
	return (0);
}

void *
linux_dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
    dma_addr_t *handle)
{
	struct lkpi_pool_obj *obj;

	mtx_lock(&pool->lock);
	if (pool->free == NULL && lkpi_dma_pool_grow(pool, mem_flags) != 0) {
		mtx_unlock(&pool->lock);
		*handle = 0;
		return (NULL);
	}
	obj = pool->free;
	pool->free = obj->next;
	mtx_unlock(&pool->lock);

	*handle = obj->dma;
	if ((mem_flags & M_ZERO) != 0)
		memset(obj, 0, pool->size);
	return (obj);
}

void
linux_dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma_addr)
{
	struct lkpi_pool_obj *obj = vaddr;

	if (vaddr == NULL)
		return;
	obj->dma = dma_addr;
	mtx_lock(&pool->lock);
	obj->next = pool->free;
	pool->free = obj;
	mtx_unlock(&pool->lock);
}

void
linux_dma_pool_destroy(struct dma_pool *pool)
{
	struct lkpi_pool_page *pg;

	if (pool == NULL)
		return;
	while ((pg = pool->pages) != NULL) {
		pool->pages = pg->next;
		dma_free_coherent(pool->dev, pg->bytes, pg->kva, pg->dma);
		free(pg, M_LKPI_DMA);
	}
	mtx_destroy(&pool->lock);
	free(pool, M_LKPI_DMA);
}

void
lkpi_dmam_pool_destroy(struct device *dev, void *p)
{
	struct dma_pool **pp;

	pp = p;
	linux_dma_pool_destroy(*pp);
}

/* For the read-only status interface (lkpi_control.c). */
void
lkpi_dma_get_stats(uint64_t *maps, uint64_t *bounces, uint64_t *pool_misses)
{
	*maps = lkpi_dma_maps;
	*bounces = lkpi_dma_bounces;
	*pool_misses = lkpi_dma_pool_misses;
}
