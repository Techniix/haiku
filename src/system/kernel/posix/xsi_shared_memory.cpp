/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	System V (XSI) shared memory: shmget(), shmat(), shmdt() and shmctl()
	(<sys/shm.h>). The libroot side is
	src/system/libroot/posix/sys/xsi_shm.cpp.

	Users that matter in practice: PostgreSQL (which relies on an accurate
	shm_nattch to detect a running server), X11's MIT-SHM extension and
	toolkits built on it, and older IPC code.

	Design:
	 - A segment is an anonymous, lazily backed area in the kernel address
	   space. It only holds the segment's VM cache; no page is allocated
	   until someone touches it.
	 - shmat() clones that area into the caller's team with
	   REGION_NO_PRIVATE_MAP. vm_clone_area() marks both sides
	   B_SHARED_AREA, so fork() shares attached segments instead of copying
	   them, and exec() drops them, both as POSIX requires.
	 - shm_nattch is the number of areas mapping the segment's cache, minus
	   the kernel's own area. Counting areas instead of attach calls keeps
	   it right when a process exits or forks without calling shmdt().
	 - IPC_RMID removes the identifier and deletes the kernel area at once.
	   Attached areas keep their reference to the cache, so the memory
	   stays valid for them and is freed when the last one goes away, which
	   is the lifetime POSIX specifies, with no deferred bookkeeping.
	 - shmdt() only deletes areas created by shmat(), recognised by their
	   name ("shm <id>") and start address.

	Limits: kMaxSegments segments; a segment of at most half the physical
	memory; at most all of physical memory in segments together.
*/


#include <posix/xsi_shared_memory.h>

#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

#include <OS.h>

#include <kernel.h>
#include <lock.h>
#include <real_time_clock.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/OpenHashTable.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>


//#define TRACE_XSI_SHM
#ifdef TRACE_XSI_SHM
#	define TRACE(x...) dprintf("xsi_shm: " x)
#else
#	define TRACE(x...) do {} while (false)
#endif


static const int32 kMaxSegments = 4096;
static const char* kAttachNamePrefix = "shm ";


namespace {


struct Segment {
	Segment*		hashLink;

	int				id;
	key_t			key;
	size_t			size;		// as requested (shm_segsz)
	size_t			mappedSize;	// rounded up to whole pages

	uid_t			uid;
	gid_t			gid;
	uid_t			creatorUID;
	gid_t			creatorGID;
	mode_t			mode;

	pid_t			creatorPID;
	pid_t			lastPID;
	time_t			attachTime;
	time_t			detachTime;
	time_t			changeTime;

	area_id			kernelArea;
	VMCache*		cache;		// we hold a reference

	//!	Whether the current team may access the segment with "needed"
	//	(S_IROTH / S_IWOTH bits).
	bool HasAccess(mode_t needed) const
	{
		uid_t euid = geteuid();
		if (euid == 0)
			return true;

		mode_t granted;
		if (euid == uid || euid == creatorUID)
			granted = mode >> 6;
		else if (getegid() == gid || getegid() == creatorGID)
			granted = mode >> 3;
		else
			granted = mode;

		return (granted & needed) == needed;
	}

	//!	Owner, creator or root may change or remove the segment.
	bool MayModify() const
	{
		uid_t euid = geteuid();
		return euid == 0 || euid == uid || euid == creatorUID;
	}

	//!	Number of team areas mapping this segment.
	uint32 AttachCount() const
	{
		uint32 count = 0;
		cache->Lock();
		for (VMArea* area = cache->areas.First(); area != NULL;
				area = cache->areas.GetNext(area)) {
			count++;
		}
		cache->Unlock();
		// The kernel's own area is not an attachment.
		return count > 0 ? count - 1 : 0;
	}
};


struct SegmentHashDefinition {
	typedef int			KeyType;
	typedef Segment		ValueType;

	size_t HashKey(int id) const		{ return (size_t)id; }
	size_t Hash(Segment* segment) const	{ return HashKey(segment->id); }
	bool Compare(int id, Segment* segment) const
		{ return segment->id == id; }
	Segment*& GetLink(Segment* segment) const
		{ return segment->hashLink; }
};


typedef BOpenHashTable<SegmentHashDefinition> SegmentTable;


}	// namespace


static mutex sLock = MUTEX_INITIALIZER("xsi shared memory");
static SegmentTable* sSegments;
static int32 sSegmentCount;
static size_t sTotalPages;
static int sNextID = 1;

static size_t sMaxSegmentSize;
static size_t sMaxTotalPages;


static time_t
now_seconds()
{
	return (time_t)(real_time_clock_usecs() / 1000000);
}


static Segment*
find_by_key(key_t key)
{
	for (SegmentTable::Iterator it = sSegments->GetIterator(); it.HasNext();) {
		Segment* segment = it.Next();
		if (segment->key == key)
			return segment;
	}
	return NULL;
}


static int
next_id()
{
	// IDs are not reused soon after removal, so that a stale identifier
	// fails with EINVAL instead of naming somebody else's segment.
	while (true) {
		int id = sNextID++;
		if (sNextID <= 0)
			sNextID = 1;
		if (sSegments->Lookup(id) == NULL)
			return id;
	}
}


//!	Frees a segment that is no longer in the table. sLock must be held.
static void
destroy_segment(Segment* segment)
{
	// Attached areas keep the cache alive; the memory goes with the last.
	vm_delete_area(VMAddressSpace::KernelID(), segment->kernelArea, true);
	segment->cache->ReleaseRef();
	sTotalPages -= segment->mappedSize / B_PAGE_SIZE;
	sSegmentCount--;
	delete segment;
}


static status_t
create_segment(key_t key, size_t size, int flags, Segment*& _segment)
{
	if (size < 1 || size > sMaxSegmentSize)
		return B_BAD_VALUE;
	if (sSegmentCount >= kMaxSegments)
		return ENOSPC;

	size_t mappedSize = ROUNDUP(size, B_PAGE_SIZE);
	if (sTotalPages + mappedSize / B_PAGE_SIZE > sMaxTotalPages)
		return ENOSPC;

	Segment* segment = new(std::nothrow) Segment;
	if (segment == NULL)
		return B_NO_MEMORY;

	int id = next_id();
	char name[B_OS_NAME_LENGTH];
	snprintf(name, sizeof(name), "shm segment %d", id);

	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	void* address;
	area_id area = create_area_etc(VMAddressSpace::KernelID(), name,
		mappedSize, B_NO_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&addressRestrictions, &physicalRestrictions, &address);
	if (area < 0) {
		delete segment;
		return area;
	}

	// Keep a reference to the cache, to count the areas attached to it.
	VMArea* vmArea = VMAreas::Lookup(area);
	VMCache* cache = vm_area_get_locked_cache(vmArea);
	cache->Unlock();
		// the reference vm_area_get_locked_cache() acquired stays ours

	time_t now = now_seconds();
	segment->id = id;
	segment->key = key;
	segment->size = size;
	segment->mappedSize = mappedSize;
	segment->uid = segment->creatorUID = geteuid();
	segment->gid = segment->creatorGID = getegid();
	segment->mode = flags & 0777;
	segment->creatorPID = team_get_current_team_id();
	segment->lastPID = 0;
	segment->attachTime = 0;
	segment->detachTime = 0;
	segment->changeTime = now;
	segment->kernelArea = area;
	segment->cache = cache;

	sSegments->Insert(segment);
	sSegmentCount++;
	sTotalPages += mappedSize / B_PAGE_SIZE;

	TRACE("created %d (key %d, %zu bytes)\n", id, (int)key, size);
	_segment = segment;
	return B_OK;
}


//	#pragma mark - kernel private


void
xsi_shm_init()
{
	sSegments = new(std::nothrow) SegmentTable;
	if (sSegments == NULL || sSegments->Init() != B_OK)
		panic("xsi_shm_init(): no memory for the segment table");

	size_t physicalBytes = (size_t)vm_page_num_pages() * B_PAGE_SIZE;
	sMaxSegmentSize = physicalBytes / 2;
	sMaxTotalPages = vm_page_num_pages();
}


//	#pragma mark - syscalls


int
_user_xsi_shmget(key_t key, size_t size, int flags)
{
	MutexLocker locker(sLock);

	if (key != IPC_PRIVATE) {
		Segment* segment = find_by_key(key);
		if (segment != NULL) {
			if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL))
				return EEXIST;
			if (size > segment->size)
				return B_BAD_VALUE;
			// The low mode bits of "flags" are the requested access.
			mode_t needed = 0;
			if ((flags & 0444) != 0)
				needed |= S_IROTH;
			if ((flags & 0222) != 0)
				needed |= S_IWOTH;
			if (!segment->HasAccess(needed))
				return EACCES;
			return segment->id;
		}
		if ((flags & IPC_CREAT) == 0)
			return ENOENT;
	}

	Segment* segment;
	status_t status = create_segment(key, size, flags, segment);
	if (status != B_OK)
		return status;
	return segment->id;
}


status_t
_user_xsi_shmat(int id, const void* address, int flags, void** _address)
{
	if (_address == NULL || !IS_USER_ADDRESS(_address))
		return B_BAD_ADDRESS;

	uint32 addressSpec = B_RANDOMIZED_ANY_ADDRESS;
	void* target = NULL;
	if (address != NULL) {
		addr_t base = (addr_t)address;
		if ((flags & SHM_RND) != 0)
			base = ROUNDDOWN(base, SHMLBA);
		else if (base % SHMLBA != 0)
			return B_BAD_VALUE;
		if (!IS_USER_ADDRESS(base))
			return B_BAD_VALUE;
		target = (void*)base;
		addressSpec = B_EXACT_ADDRESS;
	}

	MutexLocker locker(sLock);

	Segment* segment = sSegments->Lookup(id);
	if (segment == NULL)
		return B_BAD_VALUE;

	bool readOnly = (flags & SHM_RDONLY) != 0;
	if (!segment->HasAccess(readOnly ? S_IROTH : S_IROTH | S_IWOTH))
		return EACCES;

	char name[B_OS_NAME_LENGTH];
	snprintf(name, sizeof(name), "%s%d", kAttachNamePrefix, id);

	uint32 protection = B_READ_AREA | (readOnly ? 0 : B_WRITE_AREA);
	area_id area = vm_clone_area(team_get_current_team_id(), name, &target,
		addressSpec, protection, REGION_NO_PRIVATE_MAP, segment->kernelArea,
		true);
	if (area < 0)
		return area == B_NO_MEMORY ? ENOMEM : B_BAD_VALUE;

	if (user_memcpy(_address, &target, sizeof(void*)) != B_OK) {
		vm_delete_area(team_get_current_team_id(), area, true);
		return B_BAD_ADDRESS;
	}

	segment->lastPID = team_get_current_team_id();
	segment->attachTime = now_seconds();
	TRACE("attached %d at %p in team %" B_PRId32 "\n", id, target,
		team_get_current_team_id());
	return B_OK;
}


status_t
_user_xsi_shmdt(const void* address)
{
	team_id team = team_get_current_team_id();
	if (!IS_USER_ADDRESS(address))
		return B_BAD_VALUE;
	area_id area = area_for((void*)address);
		// a user address resolves in the current team
	if (area < 0)
		return B_BAD_VALUE;

	area_info info;
	if (get_area_info(area, &info) != B_OK || info.address != address
		|| info.team != team
		|| strncmp(info.name, kAttachNamePrefix, strlen(kAttachNamePrefix))
			!= 0) {
		return B_BAD_VALUE;
	}
	int id = (int)strtol(info.name + strlen(kAttachNamePrefix), NULL, 10);

	status_t status = vm_delete_area(team, area, true);
	if (status != B_OK)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);
	if (Segment* segment = sSegments->Lookup(id)) {
		segment->lastPID = team;
		segment->detachTime = now_seconds();
	}
	return B_OK;
}


status_t
_user_xsi_shmctl(int id, int command, struct shmid_ds* userBuffer)
{
	MutexLocker locker(sLock);

	Segment* segment = sSegments->Lookup(id);
	if (segment == NULL)
		return B_BAD_VALUE;

	switch (command) {
		case IPC_STAT:
		{
			if (userBuffer == NULL || !IS_USER_ADDRESS(userBuffer))
				return B_BAD_ADDRESS;
			if (!segment->HasAccess(S_IROTH))
				return EACCES;

			struct shmid_ds result;
			memset(&result, 0, sizeof(result));
			result.shm_perm.key = segment->key;
			result.shm_perm.uid = segment->uid;
			result.shm_perm.gid = segment->gid;
			result.shm_perm.cuid = segment->creatorUID;
			result.shm_perm.cgid = segment->creatorGID;
			result.shm_perm.mode = segment->mode;
			result.shm_segsz = segment->size;
			result.shm_lpid = segment->lastPID;
			result.shm_cpid = segment->creatorPID;
			result.shm_nattch = segment->AttachCount();
			result.shm_atime = segment->attachTime;
			result.shm_dtime = segment->detachTime;
			result.shm_ctime = segment->changeTime;

			if (user_memcpy(userBuffer, &result, sizeof(result)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case IPC_SET:
		{
			if (userBuffer == NULL || !IS_USER_ADDRESS(userBuffer))
				return B_BAD_ADDRESS;
			if (!segment->MayModify())
				return EPERM;

			struct shmid_ds request;
			if (user_memcpy(&request, userBuffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;

			segment->uid = request.shm_perm.uid;
			segment->gid = request.shm_perm.gid;
			segment->mode = request.shm_perm.mode & 0777;
			segment->changeTime = now_seconds();
			return B_OK;
		}

		case IPC_RMID:
		{
			if (!segment->MayModify())
				return EPERM;

			TRACE("removing %d (%" B_PRIu32 " attached)\n", id,
				segment->AttachCount());
			sSegments->Remove(segment);
			destroy_segment(segment);
			return B_OK;
		}

		default:
			return B_BAD_VALUE;
	}
}
