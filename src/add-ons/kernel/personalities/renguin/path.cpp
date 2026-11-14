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

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>

#include <AutoDeleter.h>

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


status_t
renguin_resolve_in_root(const char* linuxPath, uint32 flags, char* hostPath,
	size_t hostPathSize)
{
	if (linuxPath == NULL || linuxPath[0] != '/')
		return B_BAD_VALUE;

	const size_t rootLength = strlen(RENGUIN_LINUX_ROOT);
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
		memcpy(candidate, RENGUIN_LINUX_ROOT, rootLength);
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
			size_t restLength = strlen(rest);
			if (linkLength + 1 + restLength + 1 > kBufferSize)
				return B_NAME_TOO_LONG;
			memcpy(candidate, link, linkLength);
			candidate[linkLength] = '/';
			memcpy(candidate + linkLength + 1, rest, restLength + 1);
			strlcpy(pending, candidate, pendingSize);
			p = pending;

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

	if (rootLength + resolvedLength + 1 > hostPathSize)
		return B_NAME_TOO_LONG;
	memcpy(hostPath, RENGUIN_LINUX_ROOT, rootLength);
	memcpy(hostPath + rootLength, resolved, resolvedLength + 1);

	TRACE("\"%s\" -> \"%s\"\n", linuxPath, hostPath);
	return B_OK;
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
	const size_t rootLength = strlen(RENGUIN_LINUX_ROOT);
	if (strncmp(path, RENGUIN_LINUX_ROOT, rootLength) != 0)
		return;
	if (path[rootLength] == '\0') {
		strcpy(path, "/");
		return;
	}
	if (path[rootLength] != '/')
		return;
	memmove(path, path + rootLength, strlen(path + rootLength) + 1);
}


status_t
renguin_get_user_path(uint64 userPath, uint32 flags, char* path,
	size_t pathSize)
{
	ssize_t length = renguin_copy_string_in(path, userPath, pathSize);
	if (length < 0)
		return length;

	if (path[0] != '/' || !linux_root_exists())
		return B_OK;

	// Devices always come from Haiku's own /dev, as FreeBSD gets them from
	// the devfs mounted at /compat/linux/dev. Otherwise "> /dev/null" (an
	// O_CREAT open) would create a plain file inside the Linux root.
	if (strncmp(path, "/dev", 4) == 0 && (path[4] == '\0' || path[4] == '/'))
		return B_OK;

	// Already a host path into the Linux root: leave it alone.
	const size_t rootLength = strlen(RENGUIN_LINUX_ROOT);
	if (strncmp(path, RENGUIN_LINUX_ROOT, rootLength) == 0
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
	// Otherwise use the path unchanged: it refers to Haiku's own tree.
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
