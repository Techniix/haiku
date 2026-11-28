/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux root ("emul path").
 *
 * FreeBSD's Linuxulator runs Linux programs against a Linux userland in
 * /compat/linux: an absolute path a Linux program uses is looked up there
 * first, and in the real root only if it does not exist there. Older FreeBSD
 * releases did this per system call with linux_emul_convpath() and the
 * LCONVPATH*() macros (sys/compat/linux/linux_util.[ch]); current FreeBSD
 * does it inside namei() through pwd_altroot(). Renguin uses the per-call
 * model for now because it needs no change to Haiku's VFS: each handler that
 * takes a path calls renguin_get_user_path() or renguin_user_path().
 *
 * The lookup walks the path one component at a time under the root and
 * follows symbolic links itself, so that an absolute link target such as
 * Debian's /lib64/ld-linux-x86-64.so.2 -> /lib/x86_64-linux-gnu/... resolves
 * inside the Linux root rather than against Haiku's "/". Haiku's VFS would
 * otherwise resolve it against the real root and miss.
 *
 * Known gaps (see docs/research-report.md section 11): ".." from the top of
 * the Linux root stays at the top, and a relative path is not translated (it
 * is relative to the working directory, which may itself be inside the root).
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <lock.h>

#include <kernel.h>
#include <syscalls.h>
#include <vfs.h>

#include <sys/stat.h>


//#define TRACE_RENGUIN_PATH
#ifdef TRACE_RENGUIN_PATH
#	define TRACE(x...) dprintf("renguin path: " x)
#else
#	define TRACE(x...) ;
#endif


static const int kMaxSymlinks = 40;
	// Linux's MAXSYMLINKS


/*!	Appends "/" and \a length bytes of \a component to \a buffer, which holds
	\a *_length bytes.
*/
static status_t
append_component(char* buffer, size_t* _length, size_t bufferSize,
	const char* component, size_t length)
{
	if (*_length + 1 + length + 1 > bufferSize)
		return B_NAME_TOO_LONG;
	buffer[(*_length)++] = '/';
	memcpy(buffer + *_length, component, length);
	*_length += length;
	buffer[*_length] = '\0';
	return B_OK;
}


static void
remove_last_component(char* buffer, size_t* _length)
{
	while (*_length > 0 && buffer[*_length - 1] != '/')
		(*_length)--;
	if (*_length > 0)
		(*_length)--;		// the separating '/'
	buffer[*_length] = '\0';
}


static const int kMaxRestarts = 8;


/*!	Walks \a linuxPath one component at a time, following links itself.
	With \a inRoot, under the Linux root (absolute link targets restart at
	the Linux root); otherwise through Haiku's own tree, where an absolute
	link target is resolved again as a new Linux path (Linux root first),
	so that a link outside the root, such as one in Haiku's /tmp, can lead
	into /proc or into the root.
*/
static status_t
resolve_walk(const char* linuxPath, uint32 flags, char* hostPath,
	size_t hostPathSize, bool inRoot, int depth, const char* root = NULL)
{
	if (linuxPath == NULL || linuxPath[0] != '/')
		return B_BAD_VALUE;
	if (depth > kMaxRestarts)
		return B_LINK_LIMIT;

	const char* prefix = inRoot
		? (root != NULL ? root : renguin_root_prefix()) : "";
	const size_t rootLength = strlen(prefix);
	const size_t kBufferSize = B_PATH_NAME_LENGTH;

	// resolved: the Linux-side path resolved so far ("" == "/").
	// pending: what still has to be walked. candidate: host path to test.
	// link: symlink target. All are kernel heap buffers; the kernel stack is
	// too small for several path buffers.
	char* buffers = (char*)malloc(kBufferSize * 5);
	if (buffers == NULL)
		return B_NO_MEMORY;
	MemoryDeleter buffersDeleter(buffers);
	char* resolved = buffers;
	char* pending = buffers + kBufferSize;			// 2 * kBufferSize
	char* candidate = buffers + kBufferSize * 3;
	char* link = buffers + kBufferSize * 4;
	const size_t pendingSize = kBufferSize * 2;

	size_t resolvedLength = 0;
	resolved[0] = '\0';
	if (strlcpy(pending, linuxPath, pendingSize) >= pendingSize)
		return B_NAME_TOO_LONG;

	int links = 0;
	const char* p = pending;
	while (true) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		const char* component = p;
		while (*p != '\0' && *p != '/')
			p++;
		size_t componentLength = p - component;

		const char* rest = p;
		while (*rest == '/')
			rest++;
		bool isLast = *rest == '\0';

		if (componentLength == 1 && component[0] == '.')
			continue;
		if (componentLength == 2 && component[0] == '.' && component[1] == '.') {
			remove_last_component(resolved, &resolvedLength);
			continue;
		}

		// /proc is generated (procfs.cpp), not looked up on disk: hand the
		// rest of the path back for the caller to pass there. This is how
		// a link such as /etc/mtab -> ../proc/self/mounts reaches it.
		if (resolvedLength == 0 && componentLength == 4
			&& memcmp(component, "proc", 4) == 0) {
			int written = *rest != '\0'
				? snprintf(hostPath, hostPathSize, "/proc/%s", rest)
				: snprintf(hostPath, hostPathSize, "/proc");
			if (written < 0 || (size_t)written >= hostPathSize)
				return B_NAME_TOO_LONG;
			return RENGUIN_STATUS_IN_PROC;
		}

		// candidate = root + resolved + "/" + component
		if (rootLength + resolvedLength + 1 + componentLength + 1
				> kBufferSize) {
			return B_NAME_TOO_LONG;
		}
		memcpy(candidate, prefix, rootLength);
		memcpy(candidate + rootLength, resolved, resolvedLength);
		size_t candidateLength = rootLength + resolvedLength;
		candidate[candidateLength++] = '/';
		memcpy(candidate + candidateLength, component, componentLength);
		candidateLength += componentLength;
		candidate[candidateLength] = '\0';

		struct stat st;
		status_t status = vfs_read_stat(-1, candidate, false, &st, true);
		if (status != B_OK) {
			if (status == B_ENTRY_NOT_FOUND && isLast
				&& (flags & RENGUIN_PATH_MAY_CREATE) != 0) {
				status = append_component(resolved, &resolvedLength,
					kBufferSize, component, componentLength);
				if (status != B_OK)
					return status;
				break;
			}
			return status;
		}

		if (S_ISLNK(st.st_mode)
			&& (!isLast || (flags & RENGUIN_PATH_FOLLOW_LEAF) != 0)) {
			if (++links > kMaxSymlinks)
				return B_LINK_LIMIT;

			size_t linkLength = kBufferSize - 1;
			status = _kern_read_link(-1, candidate, link, &linkLength);
			if (status != B_OK)
				return status;
			if (linkLength >= kBufferSize - 1)
				return B_NAME_TOO_LONG;
			link[linkLength] = '\0';

			// New pending = link target + "/" + rest. Build it in candidate
			// (no longer needed) and copy back, since rest points into
			// pending.
			// (no separator for an empty rest: a link at the end of the
			// path leaves no trailing slash behind)
			size_t restLength = strlen(rest);
			if (linkLength + 1 + restLength + 1 > kBufferSize)
				return B_NAME_TOO_LONG;
			memcpy(candidate, link, linkLength);
			if (restLength > 0) {
				candidate[linkLength] = '/';
				memcpy(candidate + linkLength + 1, rest, restLength + 1);
			} else
				candidate[linkLength] = '\0';
			strlcpy(pending, candidate, pendingSize);
			p = pending;

			// A target on Haiku's own boot volume (/boot/system/...,
			// /boot/home/...) is a host path: the Linux root uses such
			// links to share files with Haiku, such as /etc/resolv.conf ->
			// /boot/system/settings/network/resolv.conf. Linux roots keep
			// kernels in /boot, but never these two directories.
			if (inRoot && (strncmp(link, "/boot/system/", 13) == 0
				|| strncmp(link, "/boot/home/", 11) == 0)) {
				size_t hostLength = strlen(candidate);
				while (hostLength > 1 && candidate[hostLength - 1] == '/')
					candidate[--hostLength] = '\0';
				if (strlcpy(hostPath, candidate, hostPathSize)
						>= hostPathSize) {
					return B_NAME_TOO_LONG;
				}
				TRACE("\"%s\" -> host \"%s\"\n", linuxPath, hostPath);
				return B_OK;
			}

			// Outside the Linux root, an absolute target is a new Linux
			// path: resolved as any other (Linux root first).
			// (pending holds the target and the rest of the path now)
			if (!inRoot && link[0] == '/') {
				status = resolve_walk(pending, flags, hostPath,
					hostPathSize, true, depth + 1, root);
				if (status == B_OK || status == RENGUIN_STATUS_IN_PROC)
					return status;
				return resolve_walk(pending, flags, hostPath, hostPathSize,
					false, depth + 1, root);
			}

			// An absolute target restarts at the Linux root; a relative one
			// is relative to the directory holding the link.
			if (link[0] == '/') {
				resolvedLength = 0;
				resolved[0] = '\0';
			}
			continue;
		}

		if (!isLast && !S_ISDIR(st.st_mode))
			return B_NOT_A_DIRECTORY;

		status = append_component(resolved, &resolvedLength, kBufferSize,
			component, componentLength);
		if (status != B_OK)
			return status;
	}

	if (rootLength + resolvedLength + 2 > hostPathSize)
		return B_NAME_TOO_LONG;
	memcpy(hostPath, prefix, rootLength);
	memcpy(hostPath + rootLength, resolved, resolvedLength + 1);
	if (rootLength + resolvedLength == 0)
		strcpy(hostPath, "/");

	TRACE("\"%s\" -> \"%s\"\n", linuxPath, hostPath);
	return B_OK;
}


status_t
renguin_resolve_in_root(const char* linuxPath, uint32 flags, char* hostPath,
	size_t hostPathSize)
{
	return resolve_walk(linuxPath, flags, hostPath, hostPathSize, true, 0);
}


/*!	batch 14: as renguin_resolve_in_root(), under \a root (a host path;
	NULL or "": the Linux root), for the loader, which runs before the
	team's new data (and so its chroot() root) is in place.
*/
status_t
renguin_resolve_in_root_of(const char* root, const char* linuxPath,
	uint32 flags, char* hostPath, size_t hostPathSize)
{
	return resolve_walk(linuxPath, flags, hostPath, hostPathSize, true, 0,
		root != NULL && root[0] != '\0' ? root : RENGUIN_LINUX_ROOT);
}


static bool
linux_root_exists()
{
	renguin_team* data = renguin_current_team();
	return data != NULL && data->have_linux_root;
}


/*!	Strips the Linux root from the front of \a path if it is there, so that a
	program inside the Linux root sees "/etc" rather than the host path.
*/
void
renguin_host_to_linux_path(char* path)
{
	const char* root = renguin_root_prefix();
	const size_t rootLength = strlen(root);
	if (strncmp(path, root, rootLength) != 0)
		return;
	if (path[rootLength] == '\0') {
		strcpy(path, "/");
		return;
	}
	if (path[rootLength] != '/')
		return;
	memmove(path, path + rootLength, strlen(path + rootLength) + 1);
}


/*!	Renga (2026-11-14): Linux pseudo-terminals. Linux programs find their
	pty slaves as /dev/pts/<n> (devpts) and the master as /dev/ptmx; Haiku's
	pty driver publishes /dev/ptmx too, the masters as /dev/pt/<l><h> and
	the slaves as /dev/tt/<l><h>, with <l> 'p' + n / 16 and <h> n % 16 in
	hex (libroot's ptsname()). These two translate between them.
*/
bool
renguin_linux_pts_to_host(const char* path, char* hostPath, size_t size)
{
	if (strcmp(path, "/dev/pts") == 0 || strcmp(path, "/dev/pts/") == 0) {
		strlcpy(hostPath, "/dev/tt", size);
		return true;
	}
	if (strncmp(path, "/dev/pts/", 9) != 0)
		return false;
	const char* number = path + 9;
	if (*number < '0' || *number > '9')
		return false;
	uint32 index = 0;
	for (; *number >= '0' && *number <= '9'; number++)
		index = index * 10 + (*number - '0');
	if (*number != '\0' || index >= 16 * 26)
		return false;
	snprintf(hostPath, size, "/dev/tt/%c%" B_PRIx32, (char)('p' + index / 16),
		index % 16);
	return true;
}


void
renguin_host_to_linux_device(char* path, size_t size)
{
	bool slave = strncmp(path, "/dev/tt/", 8) == 0;
	bool master = strncmp(path, "/dev/pt/", 8) == 0;
	if (!slave && !master)
		return;
	const char* name = path + 8;
	if (name[0] < 'p' || name[0] > 'z' || name[1] == '\0' || name[2] != '\0')
		return;
	char digit = name[1];
	uint32 low;
	if (digit >= '0' && digit <= '9')
		low = digit - '0';
	else if (digit >= 'a' && digit <= 'f')
		low = digit - 'a' + 10;
	else
		return;
	uint32 index = (name[0] - 'p') * 16 + low;
	if (slave)
		snprintf(path, size, "/dev/pts/%" B_PRIu32, index);
	else
		strlcpy(path, "/dev/ptmx", size);
}


status_t
renguin_get_user_path(uint64 userPath, uint32 flags, char* path,
	size_t pathSize)
{
	ssize_t length = renguin_copy_string_in(path, userPath, pathSize);
	if (length < 0)
		return length;

	return renguin_translate_path(path, pathSize, flags);
}


status_t
renguin_translate_path(char* path, size_t pathSize, uint32 flags)
{
	if (path[0] != '/' || !linux_root_exists())
		return B_OK;

	// Devices always come from Haiku's own /dev, as FreeBSD gets them from
	// the devfs mounted at /compat/linux/dev. Otherwise "> /dev/null" (an
	// O_CREAT open) would create a plain file inside the Linux root.
	if (strncmp(path, "/dev", 4) == 0 && (path[4] == '\0' || path[4] == '/')) {
		// devpts: /dev/pts/<n> is Haiku's /dev/tt/<l><h>
		char host[32];
		if (renguin_linux_pts_to_host(path, host, sizeof(host)))
			strlcpy(path, host, pathSize);
		else if (strcmp(path, "/dev/uinput") == 0) {
			// Linux has the uinput node in both places; Renga's gamepad
			// core publishes /dev/input/uinput (F-42)
			strlcpy(path, "/dev/input/uinput", pathSize);
		}
		return B_OK;
	}

	// Already a host path into the Linux root: leave it alone.
	const char* root = renguin_root_prefix();
	const size_t rootLength = strlen(root);
	if (strncmp(path, root, rootLength) == 0
		&& (path[rootLength] == '\0' || path[rootLength] == '/')) {
		return B_OK;
	}

	char* translated = (char*)malloc(B_PATH_NAME_LENGTH);
	if (translated == NULL)
		return B_NO_MEMORY;
	MemoryDeleter translatedDeleter(translated);

	status_t status = renguin_resolve_in_root(path, flags, translated,
		B_PATH_NAME_LENGTH);
	if (status == B_OK || status == RENGUIN_STATUS_IN_PROC) {
		strlcpy(path, translated, pathSize);
		return status;
	}

	// Otherwise the path refers to Haiku's own tree, which Haiku's VFS
	// resolves, unless a link on the way leads where only Renguin can
	// follow it: into /proc, or into the Linux root (a link in Haiku's
	// /tmp to /proc/self/mounts). Haiku's tree is walked here only when
	// Haiku cannot resolve the path and its first directory is not the
	// Linux root's own (a library search in /lib missing a file is the
	// common case, and gains nothing from a walk); the result is taken only
	// if it leads into /proc or the Linux root, so other paths keep their
	// name (a socket bound at /tmp/x is reported as /tmp/x, not under
	// /boot/system/cache/tmp, where Haiku's /tmp link points).
	struct stat st;
	if (vfs_read_stat(-1, path, (flags & RENGUIN_PATH_FOLLOW_LEAF) != 0, &st,
			true) == B_OK) {
		return B_OK;
	}
	const char* first = path + 1;
	size_t firstLength = strcspn(first, "/");
	if (firstLength == 0 || (size_t)snprintf(translated, B_PATH_NAME_LENGTH,
			"%s/%.*s", root, (int)firstLength, first)
				>= B_PATH_NAME_LENGTH
		|| vfs_read_stat(-1, translated, false, &st, true) == B_OK) {
		return B_OK;
	}
	status = resolve_walk(path, flags, translated, B_PATH_NAME_LENGTH, false,
		0);
	if (status == RENGUIN_STATUS_IN_PROC
		|| (status == B_OK
			&& strncmp(translated, root, rootLength) == 0
			&& (translated[rootLength] == '\0'
				|| translated[rootLength] == '/'))) {
		strlcpy(path, translated, pathSize);
		return status;
	}
	return B_OK;
}


uint64
renguin_user_path(renguin_scratch* scratch, uint64 userPath, uint32 flags,
	status_t* _status)
{
	*_status = B_OK;
	if (userPath == 0 || !linux_root_exists())
		return userPath;

	// Only absolute paths are translated; peek at the first byte.
	char first;
	if (renguin_copy_in(&first, userPath, 1) != B_OK) {
		*_status = B_BAD_ADDRESS;
		return 0;
	}
	if (first != '/')
		return userPath;

	char* path = (char*)malloc(B_PATH_NAME_LENGTH);
	if (path == NULL) {
		*_status = B_NO_MEMORY;
		return 0;
	}
	MemoryDeleter pathDeleter(path);

	status_t status = renguin_get_user_path(userPath, flags, path,
		B_PATH_NAME_LENGTH);
	if (status != B_OK && status != RENGUIN_STATUS_IN_PROC) {
		*_status = status;
		return 0;
	}

	uint64 copy = renguin_scratch_put(scratch, path, strlen(path) + 1);
	*_status = copy == 0 ? B_BAD_ADDRESS : status;
	return copy;
}


// #pragma mark - chroot (batch 14)


/*!	The current team's "/" as a host path: the directory chroot() chose,
	else the Linux root.
*/
const char*
renguin_root_prefix()
{
	renguin_team* data = renguin_current_team();
	if (data != NULL && data->root[0] != '\0')
		return data->root;
	return RENGUIN_LINUX_ROOT;
}


/*!	Roots handed from a team's execve() to the new program's load:
	renguin_load() makes the team's data anew, after the kernel has freed
	the old data (foreign_personality_exec()). One entry per team with a
	pending root, on a list without a fixed size, so that no team's root
	can be dropped to make room for another's (a dropped root would let
	the new program see the whole Linux root).

	An entry lives only while its team is in an execve(), and cannot
	outlive the team:
	- renguin_exec_keep_root() makes it, before _user_exec();
	- renguin_probe() marks it claimed when the new program is a Linux one
	  (renguin_exec_claim_root());
	- renguin_load() takes it;
	- renguin_team_released() frees an unclaimed one: the kernel calls it
	  when the exec turns out native (the team leaves the personality, and
	  ~Team will not call it again), and when the team dies;
	- a failed execve() frees it;
	- as a last guard, renguin_exec_keep_root() frees entries of teams that
	  no longer exist.
	A native program therefore runs, and starts Linux programs, outside
	the chroot: Haiku's own VFS knows nothing of it.
*/
struct exec_root {
	exec_root*	next;
	team_id		team;
	bool		claimed;
	char		root[B_PATH_NAME_LENGTH];
};

static mutex sExecRootLock = MUTEX_INITIALIZER("renguin exec roots");
static exec_root* sExecRoots = NULL;
static int32 sExecRootCount = 0;


//! Unlinks and returns the team's entry; the caller holds sExecRootLock.
static exec_root*
unlink_exec_root_locked(team_id team)
{
	for (exec_root** link = &sExecRoots; *link != NULL;
			link = &(*link)->next) {
		exec_root* entry = *link;
		if (entry->team == team) {
			*link = entry->next;
			sExecRootCount--;
			return entry;
		}
	}
	return NULL;
}


//! Frees the entries of teams that are gone; the caller holds the lock.
static void
prune_exec_roots_locked()
{
	exec_root** link = &sExecRoots;
	while (*link != NULL) {
		exec_root* entry = *link;
		team_info info;
		if (get_team_info(entry->team, &info) != B_OK) {
			*link = entry->next;
			sExecRootCount--;
			free(entry);
		} else
			link = &entry->next;
	}
}


/*!	Records \a root (NULL or "": none) as the root \a team's next program
	gets. Fails with B_NO_MEMORY rather than lose a root: execve() then
	fails, the program keeps running in its root.
*/
status_t
renguin_exec_keep_root(team_id team, const char* root)
{
	exec_root* entry = NULL;
	if (root != NULL && root[0] != '\0') {
		entry = (exec_root*)malloc(sizeof(exec_root));
		if (entry == NULL)
			return B_NO_MEMORY;
		entry->team = team;
		entry->claimed = false;
		strlcpy(entry->root, root, sizeof(entry->root));
	}

	exec_root* old;
	{
		MutexLocker locker(sExecRootLock);
		old = unlink_exec_root_locked(team);
		prune_exec_roots_locked();
		if (entry != NULL) {
			entry->next = sExecRoots;
			sExecRoots = entry;
			sExecRootCount++;
		}
	}
	free(old);
	return B_OK;
}


//! renguin_probe() claimed \a team's new program: renguin_load() follows.
void
renguin_exec_claim_root(team_id team)
{
	MutexLocker locker(sExecRootLock);
	for (exec_root* entry = sExecRoots; entry != NULL; entry = entry->next) {
		if (entry->team == team) {
			entry->claimed = true;
			break;
		}
	}
}


/*!	Moves \a team's pending root into \a root (left unchanged when there is
	none). Returns whether there was one.
*/
bool
renguin_exec_take_root(team_id team, char* root, size_t size)
{
	exec_root* entry;
	{
		MutexLocker locker(sExecRootLock);
		entry = unlink_exec_root_locked(team);
	}
	if (entry == NULL)
		return false;
	strlcpy(root, entry->root, size);
	free(entry);
	return true;
}


/*!	The team's personality data is released: its entry goes, unless the
	exec is loading a Linux program (claimed) and the team is alive.
*/
void
renguin_exec_release_root(team_id team, bool dying)
{
	exec_root* entry = NULL;
	{
		MutexLocker locker(sExecRootLock);
		for (exec_root* candidate = sExecRoots; candidate != NULL;
				candidate = candidate->next) {
			if (candidate->team == team) {
				if (dying || !candidate->claimed)
					entry = unlink_exec_root_locked(team);
				break;
			}
		}
	}
	free(entry);
}


//! How many entries there are (/proc/self/status "RengaExecRoots", tests).
int32
renguin_exec_root_count()
{
	MutexLocker locker(sExecRootLock);
	return sExecRootCount;
}


/*!	chroot(): the directory becomes the team's "/" for every later
	absolute path lookup (resolve_walk() walks under it, ".." stops at it,
	absolute link targets restart at it).

	It is a view of the file system for the bwrap shim (Flatpak step 1),
	not a sandbox; docs/design/flatpak-and-steam-under-renguin.md lists
	what it does not contain:
	- Absolute link targets under /boot/system/ and /boot/home/ are host
	  paths, as in the Linux root (the shim's binds are such links).
	- An absolute path that does not exist in the root is a host path, as
	  in the Linux root (renguin_translate_path()).
	- Relative paths are Haiku's: ".." from a working directory at the
	  root leaves it, and a working directory or a descriptor opened
	  outside the root (before chroot(), or with fchdir() to it) stays
	  usable, as on Linux.

	The root is kept as a canonical host path string, not as a vnode
	reference: nothing is pinned, nothing has to be released. fork()
	copies it with the team data, execve() hands it to a new Linux program
	(renguin_exec_keep_root()), team exit frees it with the team data. If
	the directory is removed, lookups under it fail.

	Linux asks for CAP_SYS_CHROOT. Renguin lets any program do it, since
	the bwrap shim runs as the user and a Haiku team can open any host
	path anyway; as on Linux the caller needs search permission on the
	directory (EACCES). A directory whose host path is the Linux root (or
	Haiku's "/") ends the chroot; chroot("/") inside one changes nothing.
*/
int64
renguin_sys_chroot(uint64 userPath)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return -LX_EPERM;
	char* buffers = (char*)malloc(B_PATH_NAME_LENGTH * 2);
	if (buffers == NULL)
		return -LX_ENOMEM;
	MemoryDeleter buffersDeleter(buffers);
	char* path = buffers;
	char* canonical = buffers + B_PATH_NAME_LENGTH;

	status_t status = renguin_get_user_path(userPath,
		RENGUIN_PATH_FOLLOW_LEAF, path, B_PATH_NAME_LENGTH);
	if (status == RENGUIN_STATUS_IN_PROC)
		return -LX_EINVAL;
	if (status != B_OK)
		return renguin_encode(status);
	if (path[0] == '\0')
		return -LX_ENOENT;

	// The canonical host path, without links or "..", so that the prefix
	// tests in this file compare like with like. A relative path is
	// relative to the team's working directory (kernel = false: the
	// team's I/O context, not the kernel's).
	status = vfs_normalize_path(path, canonical, B_PATH_NAME_LENGTH, true,
		false);
	if (status != B_OK)
		return renguin_encode(status);
	struct stat st;
	status = vfs_read_stat(-1, canonical, true, &st, false);
	if (status != B_OK)
		return renguin_encode(status);
	if (!S_ISDIR(st.st_mode))
		return -LX_ENOTDIR;
	// search permission, checked with the caller's effective IDs
	if (_kern_access(-1, canonical, X_OK, true) != B_OK)
		return -LX_EACCES;

	size_t length = strlen(canonical);
	while (length > 1 && canonical[length - 1] == '/')
		canonical[--length] = '\0';

	// Under the team lock: fork() copies the team data under it.
	// (Lookups in other threads read the root without it. The buffer's
	// last byte is never written by strlcpy(), so a concurrent reader sees
	// a terminated string, at worst a mix of the old and new root.)
	MutexLocker locker(data->lock);
	if (strcmp(canonical, RENGUIN_LINUX_ROOT) == 0
		|| strcmp(canonical, "/") == 0) {
		data->root[0] = '\0';
	} else
		strlcpy(data->root, canonical, sizeof(data->root));
	// Path translation is on from now on, also in a program that started
	// without a Linux root (the root is a directory that exists).
	data->have_linux_root = true;
	return 0;
}
