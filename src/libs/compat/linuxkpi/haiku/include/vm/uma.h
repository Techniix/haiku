/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <vm/uma.h> (Haiku's compat/vm/uma.h is empty).
 * The zone API LinuxKPI uses (linux_slab.c: kmem_cache), with FreeBSD's
 * prototypes, on Haiku's kernel object_cache (haiku/lkpi_uma.cpp):
 *   init/fini   -> object_cache constructor/destructor (cache-resident state)
 *   ctor/dtor   -> run on every uma_zalloc/uma_zfree, as in FreeBSD.
 * Generic: suitable for upstreaming into freebsd_network.
 */
#ifndef _LKPI_HAIKU_VM_UMA_H_
#define _LKPI_HAIKU_VM_UMA_H_

#include_next <vm/uma.h>
#include <sys/types.h>

typedef struct uma_zone *uma_zone_t;
typedef int (*uma_ctor)(void *mem, int size, void *arg, int flags);
typedef void (*uma_dtor)(void *mem, int size, void *arg);
typedef int (*uma_init)(void *mem, int size, int flags);
typedef void (*uma_fini)(void *mem, int size);

/* uma_zcreate() flags (FreeBSD values; only ZINIT has an effect here) */
#define UMA_ZONE_UNMANAGED	0x0001
#define UMA_ZONE_ZINIT		0x0002
#define UMA_ZONE_CONTIG		0x0004
#define UMA_ZONE_NOTOUCH	0x0008
#define UMA_ZONE_MALLOC		0x0010
#define UMA_ZONE_NOFREE		0x0020
#define UMA_ZONE_MTXCLASS	0x0040
#define UMA_ZONE_VM		0x0080
#define UMA_ZONE_NOTPAGE	0x0100
#define UMA_ZONE_SECONDARY	0x0200
#define UMA_ZONE_NOBUCKET	0x0400
#define UMA_ZONE_MAXBUCKET	0x0800
#define UMA_ZONE_CACHESPREAD	0x2000
#define UMA_ZONE_NODUMP		0x4000
#define UMA_ZONE_PCPU		0x8000
#define UMA_ZONE_FIRSTTOUCH	0x10000
#define UMA_ZONE_ROUNDROBIN	0x20000
#define UMA_ZONE_SMR		0x40000

/* Alignment is passed as a mask (alignment - 1), as in FreeBSD. */
#define UMA_ALIGN_PTR		(sizeof(void *) - 1)
#define UMA_ALIGN_LONG		(sizeof(long) - 1)
#define UMA_ALIGN_INT		(sizeof(int) - 1)
#define UMA_ALIGN_SHORT		(sizeof(short) - 1)
#define UMA_ALIGN_CHAR		(sizeof(char) - 1)
#define UMA_ALIGN_CACHE		(64 - 1)

#ifdef __cplusplus
extern "C" {
#endif
uma_zone_t	uma_zcreate(const char *name, size_t size, uma_ctor ctor,
		    uma_dtor dtor, uma_init uminit, uma_fini fini, int align,
		    uint32_t flags);
void		uma_zdestroy(uma_zone_t zone);
void		*uma_zalloc_arg(uma_zone_t zone, void *arg, int flags);
void		uma_zfree_arg(uma_zone_t zone, void *item, void *arg);
void		uma_prealloc(uma_zone_t zone, int itemcnt);
int		uma_zone_reserve(uma_zone_t zone, int nitems);
#ifdef __cplusplus
}
#endif

static __inline void *
uma_zalloc(uma_zone_t zone, int flags)
{
	return (uma_zalloc_arg(zone, NULL, flags));
}

static __inline void
uma_zfree(uma_zone_t zone, void *item)
{
	uma_zfree_arg(zone, item, NULL);
}

#endif
