/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * FreeBSD UMA zones (include/vm/uma.h) on Haiku's kernel object_cache.
 * Built in the "shim" profile (Haiku errno; FreeBSD malloc(9) flags).
 */
#include <KernelExport.h>
#include <slab/Slab.h>

#include <sys/param.h>
#include <sys/malloc.h>
#include <vm/uma.h>

#include <new>
#include <string.h>

struct uma_zone {
	object_cache*	cache;
	size_t		size;
	uma_ctor	ctor;
	uma_dtor	dtor;
	uma_init	init;
	uma_fini	fini;
	uint32_t	flags;
	char		name[32];
};


static status_t
uma_cache_construct(void* cookie, void* object)
{
	uma_zone* zone = (uma_zone*)cookie;

	if ((zone->flags & UMA_ZONE_ZINIT) != 0)
		memset(object, 0, zone->size);
	if (zone->init != NULL && zone->init(object, zone->size, M_WAITOK) != 0)
		return B_NO_MEMORY;
	return B_OK;
}


static void
uma_cache_destruct(void* cookie, void* object)
{
	uma_zone* zone = (uma_zone*)cookie;

	if (zone->fini != NULL)
		zone->fini(object, zone->size);
}


extern "C" uma_zone_t
uma_zcreate(const char* name, size_t size, uma_ctor ctor, uma_dtor dtor,
	uma_init uminit, uma_fini fini, int align, uint32_t flags)
{
	uma_zone* zone = new(std::nothrow) uma_zone;
	if (zone == NULL)
		return NULL;

	zone->size = size;
	zone->ctor = ctor;
	zone->dtor = dtor;
	zone->init = uminit;
	zone->fini = fini;
	zone->flags = flags;
	strlcpy(zone->name, name != NULL ? name : "lkpi uma", sizeof(zone->name));

	/* align is a mask in FreeBSD (alignment - 1) */
	zone->cache = create_object_cache_etc(zone->name, size, align + 1,
		0, 0, 0, 0, zone, uma_cache_construct, uma_cache_destruct, NULL);
	if (zone->cache == NULL) {
		delete zone;
		return NULL;
	}
	return zone;
}


extern "C" void
uma_zdestroy(uma_zone_t zone)
{
	if (zone == NULL)
		return;
	delete_object_cache(zone->cache);
	delete zone;
}


extern "C" void*
uma_zalloc_arg(uma_zone_t zone, void* arg, int flags)
{
	void* item = object_cache_alloc(zone->cache,
		(flags & M_NOWAIT) != 0 ? CACHE_DONT_WAIT_FOR_MEMORY : 0);
	if (item == NULL)
		return NULL;

	if ((flags & M_ZERO) != 0)
		memset(item, 0, zone->size);
	if (zone->ctor != NULL && zone->ctor(item, zone->size, arg, flags) != 0) {
		object_cache_free(zone->cache, item, 0);
		return NULL;
	}
	return item;
}


extern "C" void
uma_zfree_arg(uma_zone_t zone, void* item, void* arg)
{
	if (item == NULL)
		return;
	if (zone->dtor != NULL)
		zone->dtor(item, zone->size, arg);
	object_cache_free(zone->cache, item, 0);
}


extern "C" void
uma_prealloc(uma_zone_t zone, int itemcnt)
{
	object_cache_reserve(zone->cache, itemcnt, 0);
}


extern "C" int
uma_zone_reserve(uma_zone_t zone, int nitems)
{
	return object_cache_set_minimum_reserve(zone->cache, nitems) == B_OK
		? 0 : 1;
}
