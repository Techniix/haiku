/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * A Linux /proc, generated on demand.
 *
 * FreeBSD counterpart: sys/compat/linprocfs/linprocfs.c, a pseudo file system
 * mounted at /compat/linux/proc. Renguin intercepts /proc paths in the system
 * calls that take paths instead, which needs no file system add-on and no
 * mount:
 *
 * - Files: open() generates the content and returns an FD for an unlinked
 *   temporary file holding it, so reads, seeks and fstat() behave as for a
 *   regular file. Linux /proc files are snapshots taken at open time too.
 * - Directories: open() returns an FD for an empty placeholder directory and
 *   records which /proc directory it stands for (renguin_team::proc_dirs);
 *   getdents64 on that FD lists the /proc directory.
 * - Symbolic links (self, <pid>/exe, <pid>/fd/<n>, ...): readlink() answers,
 *   stat() and open() follow them.
 * - stat()/access() of any /proc path are answered from the same table.
 *
 * Paths are checked before the Linux-root translation in path.cpp: /proc in
 * a Linux userland (Alpine's has an empty /proc directory) must never be
 * looked up on disk.
 *
 * What is provided is what command-line tools read: cpuinfo, meminfo,
 * uptime, loadavg, stat, version, filesystems, mounts, the process
 * directories (stat, status, statm, cmdline, environ, comm, maps, mounts,
 * mountinfo, cgroup, exe, cwd, root, fd/, task/) and a few /proc/sys values.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <NodeMonitor.h>
#include <OS.h>
#include <fs_info.h>
#include <image.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <kernel.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <util/Random.h>
#include <real_time_clock.h>
#include <smp.h>
#include <vfs.h>


#define PROC_ROOT				"/proc"
#define PROC_SUPER_MAGIC		0x9fa0
#define PROC_DEVICE				((dev_t)0x70726f63)	// "proc"

// Where generated files and the directory placeholder live: Haiku's system
// temporary directory (/tmp is a link to it).
#define PROC_TEMP_DIR			"/boot/system/cache/tmp"
#define PROC_PLACEHOLDER		PROC_TEMP_DIR "/.renguin-proc-dir"

static const size_t kMaxProcPath = RENGUIN_PROC_PATH_MAX;
static const size_t kMaxContent = 1024 * 1024;
static const char* const kInvalidPath = "/proc/\x01";	// never exists

static int32 sTempCounter;
static char sBootID[37];


enum proc_type {
	PROC_NONE = 0,
	PROC_DIR,
	PROC_FILE,
	PROC_LINK
};

enum proc_kind {
	KIND_NONE = 0,
	KIND_ROOT,
	KIND_ROOT_FILE,		// name in node.name
	KIND_SYS,			// /proc/sys tree, relative path in node.name
	KIND_LINK_SELF,
	KIND_LINK_THREAD_SELF,
	KIND_LINK_MOUNTS,
	KIND_PID_DIR,
	KIND_PID_FILE,		// name in node.name
	KIND_PID_EXE,
	KIND_PID_CWD,
	KIND_PID_ROOT,
	KIND_PID_FD_DIR,
	KIND_PID_FD,		// fd in node.fd
	KIND_PID_TASK_DIR,
	KIND_TASK_DIR,		// tid in node.tid
	KIND_TASK_FILE		// name in node.name
};

struct proc_node {
	proc_type	type;
	proc_kind	kind;
	team_id		pid;
	thread_id	tid;
	int			fd;
	char		name[40];
	char		path[kMaxProcPath];		// canonical, "self" resolved
};


static const char* const kRootFiles[] = {
	"cpuinfo", "meminfo", "uptime", "loadavg", "stat", "version",
	"filesystems", "cmdline", "swaps", NULL
};

static const char* const kPidFiles[] = {
	"cmdline", "comm", "environ", "maps", "mounts", "mountinfo", "stat",
	"statm", "status", "cgroup", "oom_score_adj", NULL
};

static const char* const kTaskFiles[] = {
	"comm", "stat", "status", NULL
};

// The /proc/sys tree: directories end in '/'.
static const char* const kSysTree[] = {
	"kernel/", "kernel/osrelease", "kernel/ostype", "kernel/hostname",
	"kernel/version", "kernel/pid_max", "kernel/threads-max",
	"kernel/ngroups_max", "kernel/cap_last_cap", "kernel/random/",
	"kernel/random/uuid", "kernel/random/boot_id",
	"kernel/random/entropy_avail", "vm/", "vm/overcommit_memory",
	"vm/max_map_count", "fs/", "fs/file-max", "fs/nr_open", NULL
};


// #pragma mark - text buffer


struct proc_buffer {
	char*	data;
	size_t	size;
	size_t	capacity;
	bool	failed;

	proc_buffer() : data(NULL), size(0), capacity(0), failed(false) {}
	~proc_buffer() { free(data); }

	void Printf(const char* format, ...)
	{
		if (failed)
			return;
		for (int attempt = 0; attempt < 2; attempt++) {
			va_list args;
			va_start(args, format);
			int length = vsnprintf(data + size, capacity - size, format,
				args);
			va_end(args);
			if (length < 0) {
				failed = true;
				return;
			}
			if (size + length < capacity) {
				size += length;
				return;
			}
			if (!Grow(size + length + 1))
				return;
		}
	}

	void Append(const void* bytes, size_t length)
	{
		if (failed || !Grow(size + length + 1))
			return;
		memcpy(data + size, bytes, length);
		size += length;
		data[size] = '\0';
	}

	bool Grow(size_t needed)
	{
		if (needed <= capacity)
			return true;
		if (needed > kMaxContent) {
			failed = true;
			return false;
		}
		size_t newCapacity = max_c(needed, max_c(capacity * 2, (size_t)1024));
		newCapacity = min_c(newCapacity, kMaxContent);
		char* newData = (char*)realloc(data, newCapacity);
		if (newData == NULL) {
			failed = true;
			return false;
		}
		data = newData;
		capacity = newCapacity;
		return true;
	}
};


// #pragma mark - paths


static bool
parse_id(const char* text, int32* _id)
{
	if (*text < '0' || *text > '9')
		return false;
	int64 value = 0;
	for (; *text != '\0'; text++) {
		if (*text < '0' || *text > '9')
			return false;
		value = value * 10 + (*text - '0');
		if (value > INT32_MAX)
			return false;
	}
	*_id = (int32)value;
	return true;
}


/*!	Normalizes an absolute path lexically: collapses "//" and ".", applies
	"..". Returns false if the result doesn't fit.
*/
static bool
normalize(const char* path, char* out, size_t outSize)
{
	size_t length = 0;
	out[length] = '\0';
	const char* p = path;
	while (*p != '\0') {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		const char* start = p;
		while (*p != '\0' && *p != '/')
			p++;
		size_t componentLength = p - start;
		if (componentLength == 1 && start[0] == '.')
			continue;
		if (componentLength == 2 && start[0] == '.' && start[1] == '.') {
			while (length > 0 && out[length - 1] != '/')
				length--;
			if (length > 0)
				length--;
			out[length] = '\0';
			continue;
		}
		if (length + 1 + componentLength + 1 > outSize)
			return false;
		out[length++] = '/';
		memcpy(out + length, start, componentLength);
		length += componentLength;
		out[length] = '\0';
	}
	if (length == 0) {
		if (outSize < 2)
			return false;
		strcpy(out, "/");
	}
	return true;
}


static int
find_proc_dir_slot(renguin_team* data, int fd)
{
	for (int i = 0; i < (int)B_COUNT_OF(data->proc_dirs); i++) {
		if (data->proc_dirs[i].fd == fd)
			return i;
	}
	return -1;
}


/*!	The /proc directory an FD stands for, if any: it must be in the team's
	table and still be open on the placeholder directory (the number may
	have been reused).
*/
static bool
proc_dir_for_fd(int fd, char* path, size_t pathSize)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL || fd < 0)
		return false;

	// Cheap check first: this runs for every *at() call with a directory
	// FD, and most programs never open a /proc directory.
	ino_t node;
	dev_t device;
	{
		MutexLocker locker(data->lock);
		int slot = find_proc_dir_slot(data, fd);
		if (slot < 0)
			return false;
		node = data->proc_dirs[slot].node;
		device = data->proc_dirs[slot].device;
		strlcpy(path, data->proc_dirs[slot].path, pathSize);
	}

	struct stat st;
	return vfs_read_stat(fd, NULL, false, &st, false) == B_OK
		&& st.st_ino == node && st.st_dev == device;
}


//! Whether the working directory is "/" as the program sees it.
static bool
cwd_is_root()
{
	dev_t device;
	ino_t node;
	if (vfs_get_cwd(&device, &node) != B_OK)
		return false;
	renguin_team* data = renguin_current_team();
	struct stat st;
	if (data != NULL && data->have_linux_root
		&& vfs_read_stat(-1, RENGUIN_LINUX_ROOT, true, &st, true) == B_OK) {
		return st.st_dev == device && st.st_ino == node;
	}
	return vfs_read_stat(-1, "/", true, &st, true) == B_OK
		&& st.st_dev == device && st.st_ino == node;
}


bool
renguin_proc_path(int dirfd, const char* rawPath, char* procPath,
	size_t procPathSize)
{
	if (rawPath[0] == '\0')
		return false;		// empty paths: the callers' AT_EMPTY_PATH rules

	bool fromProcDir = false;
	char base[kMaxProcPath];
	if (rawPath[0] == '/') {
		if (strncmp(rawPath, "/proc", 5) != 0
			&& strncmp(rawPath, "//", 2) != 0
			&& strncmp(rawPath, "/.", 2) != 0) {
			return false;		// the common case, quickly
		}
	} else if (dirfd == LX_AT_FDCWD) {
		// "proc/..." from the root directory ("cd / && cat proc/uptime")
		const char* p = rawPath;
		while (p[0] == '.' && p[1] == '/')
			p += 2;
		if (strncmp(p, "proc", 4) != 0 || (p[4] != '\0' && p[4] != '/')
			|| !cwd_is_root()) {
			return false;
		}
	} else {
		if (!proc_dir_for_fd(dirfd, base, sizeof(base)))
			return false;
		fromProcDir = true;
	}

	// Heap buffers: this runs deep in many system calls.
	char* joined = (char*)malloc(B_PATH_NAME_LENGTH * 2);
	if (joined == NULL)
		return false;
	MemoryDeleter joinedDeleter(joined);
	char* normalized = joined + B_PATH_NAME_LENGTH;

	int length;
	if (rawPath[0] == '/')
		length = snprintf(joined, B_PATH_NAME_LENGTH, "%s", rawPath);
	else if (!fromProcDir)
		length = snprintf(joined, B_PATH_NAME_LENGTH, "/%s", rawPath);
	else
		length = snprintf(joined, B_PATH_NAME_LENGTH, "%s/%s", base, rawPath);

	bool ok = length > 0 && length < B_PATH_NAME_LENGTH
		&& normalize(joined, normalized, B_PATH_NAME_LENGTH)
		&& (strcmp(normalized, PROC_ROOT) == 0
			|| strncmp(normalized, PROC_ROOT "/", 6) == 0);
	// A trailing "/", "/." or "/.." makes the last component a directory
	// that is looked through (lstat("/proc/self/") is the directory).
	size_t rawLength = strlen(rawPath);
	bool trailing = rawPath[rawLength - 1] == '/'
		|| (rawLength >= 2 && strcmp(rawPath + rawLength - 2, "/.") == 0)
		|| (rawLength >= 3 && strcmp(rawPath + rawLength - 3, "/..") == 0)
		|| strcmp(rawPath, ".") == 0 || strcmp(rawPath, "..") == 0;
	if (ok && strlen(normalized) + (trailing ? 1 : 0) < procPathSize) {
		strcpy(procPath, normalized);
		if (trailing)
			strcat(procPath, "/");
		return true;
	}

	if (fromProcDir) {
		// Relative to a /proc directory FD but leading elsewhere (or too
		// long): never resolve it against the placeholder directory the FD
		// is really open on. Not found.
		strlcpy(procPath, kInvalidPath, procPathSize);
		return true;
	}
	return false;
}


static bool
team_exists(team_id id)
{
	team_info info;
	return get_team_info(id, &info) == B_OK;
}


static bool
thread_in_team(team_id team, thread_id tid)
{
	thread_info info;
	return get_thread_info(tid, &info) == B_OK && info.team == team;
}


static bool
in_list(const char* const* list, const char* name)
{
	for (int i = 0; list[i] != NULL; i++) {
		if (strcmp(list[i], name) == 0)
			return true;
	}
	return false;
}


/*!	Looks up a /proc path. "self" and "thread-self" are followed wherever
	they appear except as the last component with \a followLeaf false; other
	links are reported as links (the caller follows them).
*/
static bool
lookup(const char* procPath, bool followLeaf, proc_node* node)
{
	memset(node, 0, sizeof(*node));
	node->fd = -1;

	// A trailing slash (see renguin_proc_path()): follow the last
	// component, and it must be a directory.
	size_t pathLength = strlen(procPath);
	bool mustBeDirectory = pathLength > 1 && procPath[pathLength - 1] == '/';
	if (mustBeDirectory) {
		char trimmed[kMaxProcPath];
		strlcpy(trimmed, procPath, min_c(sizeof(trimmed), pathLength));
		if (!lookup(trimmed, true, node))
			return false;
		return node->type == PROC_DIR;
	}

	// Split into components after "/proc".
	char rest[kMaxProcPath * 2];
	strlcpy(rest, procPath + strlen(PROC_ROOT), sizeof(rest));
	char* components[8];
	int count = 0;
	char* save = NULL;
	for (char* c = strtok_r(rest, "/", &save); c != NULL;
			c = strtok_r(NULL, "/", &save)) {
		if (count == (int)B_COUNT_OF(components))
			return false;
		components[count++] = c;
	}

	Thread* thread = thread_get_current_thread();
	team_id self = thread->team->id;

	// Resolve self and thread-self.
	char selfText[16], tidText[16];
	snprintf(selfText, sizeof(selfText), "%" B_PRId32, self);
	snprintf(tidText, sizeof(tidText), "%" B_PRId32, thread->id);
	char* expanded[10];
	int expandedCount = 0;
	for (int i = 0; i < count; i++) {
		bool leaf = i == count - 1;
		if (i == 0 && strcmp(components[0], "self") == 0
			&& (!leaf || followLeaf)) {
			expanded[expandedCount++] = selfText;
		} else if (i == 0 && strcmp(components[0], "thread-self") == 0
			&& (!leaf || followLeaf)) {
			expanded[expandedCount++] = selfText;
			expanded[expandedCount++] = (char*)"task";
			expanded[expandedCount++] = tidText;
		} else if (i == 0 && strcmp(components[0], "mounts") == 0
			&& (!leaf || followLeaf)) {
			expanded[expandedCount++] = selfText;
			expanded[expandedCount++] = (char*)"mounts";
		} else
			expanded[expandedCount++] = components[i];
		if (expandedCount > 8)
			return false;
	}

	// Canonical path.
	strcpy(node->path, PROC_ROOT);
	for (int i = 0; i < expandedCount; i++) {
		if (strlcat(node->path, "/", sizeof(node->path)) >= sizeof(node->path)
			|| strlcat(node->path, expanded[i], sizeof(node->path))
				>= sizeof(node->path)) {
			return false;
		}
	}

	if (expandedCount == 0) {
		node->type = PROC_DIR;
		node->kind = KIND_ROOT;
		return true;
	}

	const char* first = expanded[0];
	if (expandedCount == 1) {
		if (strcmp(first, "self") == 0) {
			node->type = PROC_LINK;
			node->kind = KIND_LINK_SELF;
			return true;
		}
		if (strcmp(first, "thread-self") == 0) {
			node->type = PROC_LINK;
			node->kind = KIND_LINK_THREAD_SELF;
			return true;
		}
		if (strcmp(first, "mounts") == 0) {
			node->type = PROC_LINK;
			node->kind = KIND_LINK_MOUNTS;
			return true;
		}
		if (in_list(kRootFiles, first)) {
			node->type = PROC_FILE;
			node->kind = KIND_ROOT_FILE;
			strlcpy(node->name, first, sizeof(node->name));
			return true;
		}
	}

	if (strcmp(first, "sys") == 0) {
		char relative[kMaxProcPath] = "";
		for (int i = 1; i < expandedCount; i++) {
			if (i > 1)
				strlcat(relative, "/", sizeof(relative));
			strlcat(relative, expanded[i], sizeof(relative));
		}
		node->kind = KIND_SYS;
		strlcpy(node->name, relative, sizeof(node->name));
		if (relative[0] == '\0') {
			node->type = PROC_DIR;
			return true;
		}
		size_t length = strlen(relative);
		for (int i = 0; kSysTree[i] != NULL; i++) {
			const char* entry = kSysTree[i];
			size_t entryLength = strlen(entry);
			if (entry[entryLength - 1] == '/') {
				if (entryLength - 1 == length
					&& strncmp(entry, relative, length) == 0) {
					node->type = PROC_DIR;
					return true;
				}
			} else if (strcmp(entry, relative) == 0) {
				node->type = PROC_FILE;
				return true;
			}
		}
		return false;
	}

	int32 pid;
	if (!parse_id(first, &pid) || pid <= 0 || !team_exists(pid))
		return false;
	node->pid = pid;

	if (expandedCount == 1) {
		node->type = PROC_DIR;
		node->kind = KIND_PID_DIR;
		return true;
	}

	const char* second = expanded[1];
	if (expandedCount == 2) {
		if (in_list(kPidFiles, second)) {
			node->type = PROC_FILE;
			node->kind = KIND_PID_FILE;
			strlcpy(node->name, second, sizeof(node->name));
			return true;
		}
		node->type = PROC_LINK;
		if (strcmp(second, "exe") == 0)
			node->kind = KIND_PID_EXE;
		else if (strcmp(second, "cwd") == 0)
			node->kind = KIND_PID_CWD;
		else if (strcmp(second, "root") == 0)
			node->kind = KIND_PID_ROOT;
		else {
			node->type = PROC_DIR;
			if (strcmp(second, "fd") == 0)
				node->kind = KIND_PID_FD_DIR;
			else if (strcmp(second, "task") == 0)
				node->kind = KIND_PID_TASK_DIR;
			else
				return false;
		}
		return true;
	}

	if (strcmp(second, "fd") == 0 && expandedCount == 3) {
		int32 fd;
		if (!parse_id(expanded[2], &fd))
			return false;
		// Only our own descriptors can be checked cheaply; for other teams
		// the entry exists if the team lists it.
		if (pid == self) {
			struct stat st;
			if (vfs_read_stat(fd, NULL, false, &st, false) != B_OK)
				return false;
		} else {
			bool found = false;
			uint32 cookie = 0;
			fd_info info;
			while (_kern_get_next_fd_info(pid, &cookie, &info, sizeof(info))
					== B_OK) {
				if (info.number == fd) {
					found = true;
					break;
				}
			}
			if (!found)
				return false;
		}
		node->type = PROC_LINK;
		node->kind = KIND_PID_FD;
		node->fd = fd;
		return true;
	}

	if (strcmp(second, "task") == 0) {
		int32 tid;
		if (!parse_id(expanded[2], &tid) || tid <= 0
			|| !thread_in_team(pid, tid))
			return false;
		node->tid = tid;
		if (expandedCount == 3) {
			node->type = PROC_DIR;
			node->kind = KIND_TASK_DIR;
			return true;
		}
		if (expandedCount == 4 && in_list(kTaskFiles, expanded[3])) {
			node->type = PROC_FILE;
			node->kind = KIND_TASK_FILE;
			strlcpy(node->name, expanded[3], sizeof(node->name));
			return true;
		}
	}
	return false;
}


// #pragma mark - link targets


//! The Linux path of a team's executable.
static bool
team_executable(team_id pid, char* path, size_t size)
{
	// Our own team: the path renguin_load() recorded. Another team's data
	// can be freed under us (by an exec), so for others the registered
	// application image is used; Renguin registers one for Linux programs.
	if (pid == thread_get_current_thread()->team->id) {
		renguin_team* data = renguin_current_team();
		if (data != NULL && data->exe_path[0] != '\0') {
			strlcpy(path, data->exe_path, size);
			renguin_host_to_linux_path(path);
			return true;
		}
	}

	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(pid, &cookie, &info) == B_OK) {
		if (info.type == B_APP_IMAGE) {
			strlcpy(path, info.name, size);
			renguin_host_to_linux_path(path);
			return true;
		}
	}
	return false;
}


/*!	A character device's path, found by looking in the device directories
	terminals live in. Haiku's file systems can't name an arbitrary node
	(only directories have a known parent), so this is a search.
*/
static bool
find_device_path(dev_t device, ino_t node, char* path, size_t size)
{
	static const char* const kDirectories[] = {
		"/dev/tt", "/dev/pt", "/dev", NULL
	};
	char buffer[sizeof(struct dirent) + B_FILE_NAME_LENGTH];
	for (int i = 0; kDirectories[i] != NULL; i++) {
		int dir = _kern_open_dir(-1, kDirectories[i]);
		if (dir < 0)
			continue;
		bool found = false;
		while (!found) {
			ssize_t count = _kern_read_dir(dir, (struct dirent*)buffer,
				sizeof(buffer), 1);
			if (count <= 0)
				break;
			struct dirent* entry = (struct dirent*)buffer;
			if (entry->d_ino != node || entry->d_dev != device)
				continue;
			snprintf(path, size, "%s/%s", kDirectories[i], entry->d_name);
			found = true;
		}
		_kern_close(dir);
		if (found)
			return true;
	}
	return false;
}


//! What readlink("/proc/<pid>/fd/<n>") says.
static void
describe_fd(team_id pid, int fd, char* path, size_t size)
{
	struct stat st;
	status_t status = B_ERROR;
	if (pid == thread_get_current_thread()->team->id) {
		status = vfs_read_stat(fd, NULL, false, &st, false);
	} else {
		uint32 cookie = 0;
		fd_info info;
		while (_kern_get_next_fd_info(pid, &cookie, &info, sizeof(info))
				== B_OK) {
			if (info.number == fd) {
				status = vfs_stat_node_ref(info.device, info.node, &st);
				break;
			}
		}
	}
	if (status != B_OK) {
		strlcpy(path, "anon_inode:[unknown]", size);
		return;
	}

	if (S_ISDIR(st.st_mode)
		&& vfs_entry_ref_to_path(st.st_dev, st.st_ino, NULL, false, path,
			size) == B_OK) {
		renguin_host_to_linux_path(path);
		return;
	}
	if (S_ISFIFO(st.st_mode)) {
		snprintf(path, size, "pipe:[%" B_PRIdINO "]", st.st_ino);
		return;
	}
	if (S_ISSOCK(st.st_mode)) {
		snprintf(path, size, "socket:[%" B_PRIdINO "]", st.st_ino);
		return;
	}
	if (S_ISCHR(st.st_mode)
		&& find_device_path(st.st_dev, st.st_ino, path, size)) {
		return;
	}
	// A regular file: Haiku can't name it from the descriptor.
	snprintf(path, size, "anon_inode:[%" B_PRIdDEV ":%" B_PRIdINO "]",
		st.st_dev, st.st_ino);
}


static int64
link_target(const proc_node& node, char* target, size_t size)
{
	Thread* thread = thread_get_current_thread();
	switch (node.kind) {
		case KIND_LINK_SELF:
			snprintf(target, size, "%" B_PRId32, thread->team->id);
			return 0;
		case KIND_LINK_THREAD_SELF:
			snprintf(target, size, "%" B_PRId32 "/task/%" B_PRId32,
				thread->team->id, thread->id);
			return 0;
		case KIND_LINK_MOUNTS:
			strlcpy(target, "self/mounts", size);
			return 0;
		case KIND_PID_EXE:
			return team_executable(node.pid, target, size) ? 0 : -LX_ENOENT;
		case KIND_PID_ROOT:
			strlcpy(target, "/", size);
			return 0;
		case KIND_PID_CWD:
		{
			if (node.pid != thread->team->id)
				return -LX_EACCES;
			dev_t device;
			ino_t inode;
			if (vfs_get_cwd(&device, &inode) != B_OK
				|| vfs_entry_ref_to_path(device, inode, NULL, false, target,
					size) != B_OK) {
				return -LX_ENOENT;
			}
			renguin_host_to_linux_path(target);
			return 0;
		}
		case KIND_PID_FD:
			describe_fd(node.pid, node.fd, target, size);
			return 0;
		default:
			return -LX_EINVAL;
	}
}


// #pragma mark - content


static void
print_fixed(proc_buffer& out, bigtime_t micros)
{
	// seconds with two decimals, without floating point
	if (micros < 0)
		micros = 0;
	bigtime_t hundredths = micros / 10000;
	out.Printf("%" B_PRId64 ".%02" B_PRId64, hundredths / 100,
		hundredths % 100);
}


static void
cpuid(uint32 leaf, uint32 subleaf, uint32 regs[4])
{
	__asm__ volatile("cpuid"
		: "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
		: "a"(leaf), "c"(subleaf));
}


struct cpu_flag {
	uint8		leaf;		// 1: leaf 1, 7: leaf 7, 0x81: 0x80000001
	uint8		reg;		// 1 ebx, 2 ecx, 3 edx
	uint8		bit;
	const char*	name;
};

static const cpu_flag kCPUFlags[] = {
	{ 1, 3, 0, "fpu" }, { 1, 3, 1, "vme" }, { 1, 3, 2, "de" },
	{ 1, 3, 3, "pse" }, { 1, 3, 4, "tsc" }, { 1, 3, 5, "msr" },
	{ 1, 3, 6, "pae" }, { 1, 3, 7, "mce" }, { 1, 3, 8, "cx8" },
	{ 1, 3, 9, "apic" }, { 1, 3, 11, "sep" }, { 1, 3, 12, "mtrr" },
	{ 1, 3, 13, "pge" }, { 1, 3, 14, "mca" }, { 1, 3, 15, "cmov" },
	{ 1, 3, 16, "pat" }, { 1, 3, 17, "pse36" }, { 1, 3, 19, "clflush" },
	{ 1, 3, 23, "mmx" }, { 1, 3, 24, "fxsr" }, { 1, 3, 25, "sse" },
	{ 1, 3, 26, "sse2" }, { 1, 3, 28, "ht" },
	{ 0x81, 3, 11, "syscall" }, { 0x81, 3, 20, "nx" },
	{ 0x81, 3, 27, "rdtscp" }, { 0x81, 3, 29, "lm" },
	{ 1, 2, 0, "pni" }, { 1, 2, 1, "pclmulqdq" }, { 1, 2, 9, "ssse3" },
	{ 1, 2, 12, "fma" }, { 1, 2, 13, "cx16" }, { 1, 2, 19, "sse4_1" },
	{ 1, 2, 20, "sse4_2" }, { 1, 2, 22, "movbe" }, { 1, 2, 23, "popcnt" },
	{ 1, 2, 25, "aes" }, { 1, 2, 26, "xsave" }, { 1, 2, 28, "avx" },
	{ 1, 2, 29, "f16c" }, { 1, 2, 30, "rdrand" }, { 1, 2, 31, "hypervisor" },
	{ 0x81, 2, 0, "lahf_lm" }, { 0x81, 2, 5, "abm" },
	{ 7, 1, 3, "bmi1" }, { 7, 1, 5, "avx2" }, { 7, 1, 8, "bmi2" },
	{ 7, 1, 9, "erms" }, { 7, 1, 16, "avx512f" }, { 7, 1, 17, "avx512dq" },
	{ 7, 1, 18, "rdseed" }, { 7, 1, 19, "adx" }, { 7, 1, 29, "sha_ni" },
	{ 7, 1, 30, "avx512bw" }, { 7, 1, 31, "avx512vl" },
};


static void
generate_cpuinfo(proc_buffer& out)
{
	uint32 regs[4];
	cpuid(0, 0, regs);
	uint32 maxLeaf = regs[0];
	char vendor[13];
	memcpy(vendor, &regs[1], 4);
	memcpy(vendor + 4, &regs[3], 4);
	memcpy(vendor + 8, &regs[2], 4);
	vendor[12] = '\0';

	uint32 leaf1[4] = {}, leaf7[4] = {}, ext1[4] = {};
	cpuid(1, 0, leaf1);
	if (maxLeaf >= 7)
		cpuid(7, 0, leaf7);
	cpuid(0x80000000, 0, regs);
	uint32 maxExtended = regs[0];
	if (maxExtended >= 0x80000001)
		cpuid(0x80000001, 0, ext1);

	char brand[49] = "";
	if (maxExtended >= 0x80000004) {
		for (uint32 i = 0; i < 3; i++) {
			cpuid(0x80000002 + i, 0, regs);
			memcpy(brand + i * 16, regs, 16);
		}
		brand[48] = '\0';
	}
	const char* model = brand;
	while (*model == ' ')
		model++;

	uint32 family = (leaf1[0] >> 8) & 0xf;
	uint32 modelNumber = (leaf1[0] >> 4) & 0xf;
	if (family == 0xf)
		family += (leaf1[0] >> 20) & 0xff;
	if (family == 0x6 || family >= 0xf)
		modelNumber += ((leaf1[0] >> 16) & 0xf) << 4;
	uint32 stepping = leaf1[0] & 0xf;

	char flags[640] = "";
	for (size_t i = 0; i < B_COUNT_OF(kCPUFlags); i++) {
		const cpu_flag& flag = kCPUFlags[i];
		const uint32* source = flag.leaf == 1 ? leaf1
			: flag.leaf == 7 ? leaf7 : ext1;
		if ((source[flag.reg] & (1u << flag.bit)) == 0)
			continue;
		if (flags[0] != '\0')
			strlcat(flags, " ", sizeof(flags));
		strlcat(flags, flag.name, sizeof(flags));
	}

	int32 cpuCount = smp_get_num_cpus();
	cpu_info* infos = (cpu_info*)calloc(cpuCount, sizeof(cpu_info));
	if (infos != NULL)
		get_cpu_info(0, cpuCount, infos);

	for (int32 i = 0; i < cpuCount; i++) {
		uint64 mhz = infos != NULL ? infos[i].current_frequency / 1000000 : 0;
		out.Printf("processor\t: %" B_PRId32 "\n"
			"vendor_id\t: %s\n"
			"cpu family\t: %" B_PRIu32 "\n"
			"model\t\t: %" B_PRIu32 "\n"
			"model name\t: %s\n"
			"stepping\t: %" B_PRIu32 "\n"
			"cpu MHz\t\t: %" B_PRIu64 ".000\n"
			"physical id\t: 0\n"
			"siblings\t: %" B_PRId32 "\n"
			"core id\t\t: %" B_PRId32 "\n"
			"cpu cores\t: %" B_PRId32 "\n"
			"fpu\t\t: yes\n"
			"fpu_exception\t: yes\n"
			"cpuid level\t: %" B_PRIu32 "\n"
			"wp\t\t: yes\n"
			"flags\t\t: %s\n"
			"bogomips\t: %" B_PRIu64 ".00\n"
			"clflush size\t: 64\n"
			"cache_alignment\t: 64\n"
			"address sizes\t: 48 bits physical, 48 bits virtual\n"
			"power management:\n\n",
			i, vendor, family, modelNumber, model[0] != '\0' ? model : vendor,
			stepping, mhz, cpuCount, i, cpuCount, maxLeaf, flags, mhz * 2);
	}
	free(infos);
}


static void
generate_meminfo(proc_buffer& out)
{
	system_info info;
	get_system_info(&info);
	uint64 pageKB = B_PAGE_SIZE / 1024;
	uint64 total = info.max_pages * pageKB;
	uint64 free = info.free_memory / 1024;
	uint64 cached = info.cached_pages * pageKB;
	uint64 available = min_c(total, free + cached);
	uint64 swapTotal = info.max_swap_pages * pageKB;
	uint64 swapFree = info.free_swap_pages * pageKB;
	out.Printf("MemTotal:       %8" B_PRIu64 " kB\n"
		"MemFree:        %8" B_PRIu64 " kB\n"
		"MemAvailable:   %8" B_PRIu64 " kB\n"
		"Buffers:        %8" B_PRIu64 " kB\n"
		"Cached:         %8" B_PRIu64 " kB\n"
		"SwapCached:            0 kB\n"
		"Active:                0 kB\n"
		"Inactive:              0 kB\n"
		"SwapTotal:      %8" B_PRIu64 " kB\n"
		"SwapFree:       %8" B_PRIu64 " kB\n"
		"Dirty:                 0 kB\n"
		"Shmem:                 0 kB\n"
		"Slab:                  0 kB\n"
		"SReclaimable:          0 kB\n"
		"CommitLimit:    %8" B_PRIu64 " kB\n"
		"Committed_AS:   %8" B_PRIu64 " kB\n"
		"VmallocTotal:   34359738367 kB\n"
		"HugePages_Total:       0\n"
		"HugePages_Free:        0\n"
		"Hugepagesize:       2048 kB\n",
		total, free, available, info.block_cache_pages * pageKB, cached,
		swapTotal, swapFree, total + swapTotal,
		info.needed_memory / 1024);
}


static bigtime_t
idle_time(int32 cpuCount, bigtime_t uptime, bigtime_t* _active)
{
	bigtime_t active = 0;
	cpu_info* infos = (cpu_info*)calloc(cpuCount, sizeof(cpu_info));
	if (infos != NULL && get_cpu_info(0, cpuCount, infos) == B_OK) {
		for (int32 i = 0; i < cpuCount; i++)
			active += infos[i].active_time;
	}
	free(infos);
	if (_active != NULL)
		*_active = active;
	return max_c(uptime * cpuCount - active, (bigtime_t)0);
}


static void
generate_stat(proc_buffer& out)
{
	int32 cpuCount = smp_get_num_cpus();
	bigtime_t uptime = system_time();
	cpu_info* infos = (cpu_info*)calloc(cpuCount, sizeof(cpu_info));
	if (infos == NULL || get_cpu_info(0, cpuCount, infos) != B_OK) {
		free(infos);
		infos = NULL;
	}

	// USER_HZ (100) ticks; Haiku only knows busy and idle time per CPU,
	// which is shown as "user" and "idle".
	bigtime_t totalActive = 0;
	for (int32 i = 0; infos != NULL && i < cpuCount; i++)
		totalActive += infos[i].active_time;
	bigtime_t totalIdle = max_c(uptime * cpuCount - totalActive,
		(bigtime_t)0);
	out.Printf("cpu  %" B_PRId64 " 0 0 %" B_PRId64 " 0 0 0 0 0 0\n",
		totalActive / 10000, totalIdle / 10000);
	for (int32 i = 0; i < cpuCount; i++) {
		bigtime_t active = infos != NULL ? infos[i].active_time : 0;
		bigtime_t idle = max_c(uptime - active, (bigtime_t)0);
		out.Printf("cpu%" B_PRId32 " %" B_PRId64 " 0 0 %" B_PRId64
			" 0 0 0 0 0 0\n", i, active / 10000, idle / 10000);
	}
	free(infos);

	system_info info;
	get_system_info(&info);
	out.Printf("intr 0\nctxt 0\nbtime %" B_PRId64 "\nprocesses %" B_PRIu32
		"\nprocs_running 1\nprocs_blocked 0\nsoftirq 0\n",
		info.boot_time / 1000000, info.used_teams);
}


//! Linux's escaping of mount table fields (seq_escape: space, tab,
//! newline, backslash as octal). Haiku volumes are often named with spaces.
static void
escape_field(const char* in, char* out, size_t outSize)
{
	size_t length = 0;
	for (; *in != '\0' && length + 5 < outSize; in++) {
		if (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\\') {
			snprintf(out + length, 5, "\\%03o", (uint8)*in);
			length += 4;
		} else
			out[length++] = *in;
	}
	out[length] = '\0';
}


/*!	Volumes as /proc/mounts and mountinfo show them. The Linux root comes
	first, as "/", on the volume it lives on; then every Haiku volume at its
	Haiku mount point (paths outside the Linux root resolve to Haiku's own).
*/
static void
generate_mounts(proc_buffer& out, bool mountinfo)
{
	// mount point, escaped mount point, escaped source
	char* buffers = (char*)malloc(B_PATH_NAME_LENGTH * 6);
	if (buffers == NULL) {
		out.failed = true;
		return;
	}
	MemoryDeleter buffersDeleter(buffers);
	char* mountPoint = buffers;
	char* escapedPoint = buffers + B_PATH_NAME_LENGTH;
	char* escapedSource = buffers + B_PATH_NAME_LENGTH * 5;
	const size_t escapedPointSize = B_PATH_NAME_LENGTH * 4;
	fs_info* info = (fs_info*)malloc(sizeof(fs_info));
	if (info == NULL) {
		out.failed = true;
		return;
	}
	MemoryDeleter infoDeleter(info);

	struct stat rootStat;
	bool haveRoot = vfs_read_stat(-1, RENGUIN_LINUX_ROOT, true, &rootStat,
		true) == B_OK;

	int32 id = 1;
	if (haveRoot && _kern_read_fs_info(rootStat.st_dev, info) == B_OK) {
		escape_field(info->device_name[0] != '\0'
			? info->device_name : info->fsh_name, escapedSource,
			B_PATH_NAME_LENGTH);
		if (mountinfo) {
			out.Printf("%" B_PRId32 " 0 0:%" B_PRIdDEV " / / rw - %s %s rw\n",
				id, info->dev, info->fsh_name, escapedSource);
		} else
			out.Printf("%s / %s rw 0 0\n", escapedSource, info->fsh_name);
		id++;
	}

	int32 cookie = 0;
	dev_t device;
	while ((device = _kern_next_device(&cookie)) >= 0) {
		if (_kern_read_fs_info(device, info) != B_OK)
			continue;

		dev_t mountPointDevice;
		ino_t mountPointNode;
		if (vfs_get_mount_point(device, &mountPointDevice, &mountPointNode)
				!= B_OK
			|| vfs_entry_ref_to_path(mountPointDevice, mountPointNode, NULL,
				true, mountPoint, B_PATH_NAME_LENGTH) != B_OK) {
			strcpy(mountPoint, "/");
			if (haveRoot)
				continue;		// "/" is the Linux root, listed above
		}

		escape_field(mountPoint, escapedPoint, escapedPointSize);
		escape_field(info->device_name[0] != '\0'
			? info->device_name : info->fsh_name, escapedSource,
			B_PATH_NAME_LENGTH);
		const char* options
			= (info->flags & B_FS_IS_READONLY) != 0 ? "ro" : "rw";
		if (mountinfo) {
			out.Printf("%" B_PRId32 " 1 0:%" B_PRIdDEV " / %s %s - %s %s %s\n",
				id++, device, escapedPoint, options, info->fsh_name,
				escapedSource, options);
		} else {
			out.Printf("%s %s %s %s 0 0\n", escapedSource, escapedPoint,
				info->fsh_name, options);
		}
	}
	if (mountinfo) {
		out.Printf("%" B_PRId32 " 1 0:%d / /proc rw - proc proc rw\n", id,
			(int)PROC_DEVICE);
	} else
		out.Printf("proc /proc proc rw 0 0\n");
}


static const char*
comm_from_args(const char* args, char* comm, size_t size)
{
	// argv[0]'s base name, up to 15 characters (Linux's TASK_COMM_LEN - 1)
	char first[64];
	strlcpy(first, args, sizeof(first));
	char* space = strchr(first, ' ');
	if (space != NULL)
		*space = '\0';
	const char* base = strrchr(first, '/');
	base = base != NULL ? base + 1 : first;
	strlcpy(comm, base, min_c(size, (size_t)16));
	return comm;
}


static void
team_memory(team_id pid, uint64* _virtual, uint64* _resident)
{
	uint64 virtualSize = 0, resident = 0;
	area_info area;
	ssize_t cookie = 0;
	while (get_next_area_info(pid, &cookie, &area) == B_OK) {
		virtualSize += area.size;
		resident += area.ram_size;
	}
	*_virtual = virtualSize;
	*_resident = resident;
}


static void
generate_pid_stat(proc_buffer& out, team_id pid, thread_id tid)
{
	team_info info;
	if (get_team_info(pid, &info) != B_OK)
		return;
	char comm[16];
	if (tid > 0) {
		thread_info threadInfo;
		if (get_thread_info(tid, &threadInfo) == B_OK)
			strlcpy(comm, threadInfo.name, sizeof(comm));
		else
			comm_from_args(info.args, comm, sizeof(comm));
	} else
		comm_from_args(info.args, comm, sizeof(comm));

	team_usage_info usage = {};
	get_team_usage_info(pid, B_TEAM_USAGE_SELF, &usage);
	team_usage_info children = {};
	get_team_usage_info(pid, B_TEAM_USAGE_CHILDREN, &children);
	uint64 virtualSize, resident;
	team_memory(pid, &virtualSize, &resident);

	system_info system;
	get_system_info(&system);
	bigtime_t start = info.start_time - system.boot_time;
	if (start < 0)
		start = 0;
	char state = pid == thread_get_current_thread()->team->id ? 'R' : 'S';

	renguin_team* data = NULL;
	addr_t argStart = 0, argEnd = 0, envStart = 0, envEnd = 0;
	if (pid == thread_get_current_thread()->team->id) {
		data = renguin_current_team();
		if (data != NULL) {
			argStart = data->arg_start;
			argEnd = data->arg_end;
			envStart = data->env_start;
			envEnd = data->env_end;
		}
	}

	out.Printf("%" B_PRId32 " (%s) %c %" B_PRId32 " %" B_PRId32 " %" B_PRId32
		" 0 -1 4194304 0 0 0 0 %" B_PRId64 " %" B_PRId64 " %" B_PRId64 " %"
		B_PRId64 " 20 0 %" B_PRId32 " 0 %" B_PRId64 " %" B_PRIu64 " %"
		B_PRIu64 " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0"
		" 0 0 0 0 %" B_PRIuADDR " %" B_PRIuADDR " %" B_PRIuADDR " %" B_PRIuADDR
		" 0\n",
		tid > 0 ? tid : pid, comm, state, info.parent, info.group_id,
		info.session_id, usage.user_time / 10000, usage.kernel_time / 10000,
		children.user_time / 10000, children.kernel_time / 10000,
		info.thread_count, start / 10000, virtualSize,
		resident / B_PAGE_SIZE, argStart, argEnd, envStart, envEnd);
}


static void
generate_pid_status(proc_buffer& out, team_id pid, thread_id tid)
{
	team_info info;
	if (get_team_info(pid, &info) != B_OK)
		return;
	char comm[16];
	comm_from_args(info.args, comm, sizeof(comm));
	if (tid > 0) {
		thread_info threadInfo;
		if (get_thread_info(tid, &threadInfo) == B_OK)
			strlcpy(comm, threadInfo.name, sizeof(comm));
	}
	uint64 virtualSize, resident;
	team_memory(pid, &virtualSize, &resident);
	bool self = pid == thread_get_current_thread()->team->id;

	out.Printf("Name:\t%s\n"
		"Umask:\t%04" B_PRIo32 "\n"
		"State:\t%s\n"
		"Tgid:\t%" B_PRId32 "\n"
		"Ngid:\t0\n"
		"Pid:\t%" B_PRId32 "\n"
		"PPid:\t%" B_PRId32 "\n"
		"TracerPid:\t0\n"
		"Uid:\t%u\t%u\t%u\t%u\n"
		"Gid:\t%u\t%u\t%u\t%u\n"
		"FDSize:\t64\n"
		"Groups:\t\n"
		"VmPeak:\t%8" B_PRIu64 " kB\n"
		"VmSize:\t%8" B_PRIu64 " kB\n"
		"VmRSS:\t%8" B_PRIu64 " kB\n"
		"Threads:\t%" B_PRId32 "\n"
		"SigQ:\t0/0\n"
		"SigPnd:\t0000000000000000\n"
		"ShdPnd:\t0000000000000000\n"
		"Cpus_allowed_list:\t0-%" B_PRId32 "\n"
		"voluntary_ctxt_switches:\t0\n"
		"nonvoluntary_ctxt_switches:\t0\n",
		comm, self ? renguin_current_umask() : 022,
		self ? "R (running)" : "S (sleeping)", pid, tid > 0 ? tid : pid,
		info.parent, (unsigned)info.real_uid, (unsigned)info.uid,
		(unsigned)info.uid, (unsigned)info.uid, (unsigned)info.real_gid,
		(unsigned)info.gid, (unsigned)info.gid, (unsigned)info.gid,
		virtualSize / 1024, virtualSize / 1024, resident / 1024,
		info.thread_count, smp_get_num_cpus() - 1);
}


/*!	/proc/<pid>/maps from the team's areas, in address order. glibc's
	pthread_getattr_np() reads it to find the main thread's stack.
*/
static void
generate_maps(proc_buffer& out, team_id pid)
{
	const int32 kMaxAreas = 4096;
	area_info* areas = (area_info*)malloc(sizeof(area_info) * 64);
	int32 capacity = 64, count = 0;
	if (areas == NULL)
		return;
	area_info info;
	ssize_t cookie = 0;
	while (count < kMaxAreas && get_next_area_info(pid, &cookie, &info)
			== B_OK) {
		if (count == capacity) {
			area_info* bigger = (area_info*)realloc(areas,
				sizeof(area_info) * capacity * 2);
			if (bigger == NULL)
				break;
			areas = bigger;
			capacity *= 2;
		}
		areas[count++] = info;
	}
	// insertion sort by address (lists are short)
	for (int32 i = 1; i < count; i++) {
		area_info current = areas[i];
		int32 j = i - 1;
		while (j >= 0 && (addr_t)areas[j].address > (addr_t)current.address) {
			areas[j + 1] = areas[j];
			j--;
		}
		areas[j + 1] = current;
	}

	renguin_team* data = pid == thread_get_current_thread()->team->id
		? renguin_current_team() : NULL;

	for (int32 i = 0; i < count; i++) {
		const area_info& area = areas[i];
		addr_t start = (addr_t)area.address;
		const char* name = area.name;
		if (data != NULL && start == data->brk_start)
			name = "[heap]";
		out.Printf("%012" B_PRIxADDR "-%012" B_PRIxADDR " %c%c%cp 00000000"
			" 00:00 0                          %s\n",
			start, start + area.size,
			(area.protection & B_READ_AREA) != 0 ? 'r' : '-',
			(area.protection & B_WRITE_AREA) != 0 ? 'w' : '-',
			(area.protection & B_EXECUTE_AREA) != 0 ? 'x' : '-', name);
	}
	free(areas);
}


//! cmdline/environ: our own from memory, others' from what Haiku keeps.
static void
generate_strings(proc_buffer& out, team_id pid, bool environment)
{
	if (pid == thread_get_current_thread()->team->id) {
		renguin_team* data = renguin_current_team();
		addr_t start = data != NULL
			? (environment ? data->env_start : data->arg_start) : 0;
		addr_t end = data != NULL
			? (environment ? data->env_end : data->arg_end) : 0;
		if (start != 0 && end > start && end - start <= kMaxContent) {
			size_t length = end - start;
			char* copy = (char*)malloc(length);
			if (copy != NULL
				&& renguin_copy_in(copy, start, length) == B_OK) {
				out.Append(copy, length);
			}
			free(copy);
			return;
		}
	}
	if (environment)
		return;
	team_info info;
	if (get_team_info(pid, &info) != B_OK)
		return;
	// Haiku keeps the arguments joined by spaces (and shortened).
	size_t length = strnlen(info.args, sizeof(info.args));
	for (size_t i = 0; i < length; i++) {
		if (info.args[i] == ' ')
			info.args[i] = '\0';
	}
	out.Append(info.args, length);
	out.Append("", 1);
}


static void
random_uuid(char* text)
{
	uint8 bytes[16];
	for (size_t i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8)secure_random_value();
	bytes[6] = (bytes[6] & 0x0f) | 0x40;	// version 4
	bytes[8] = (bytes[8] & 0x3f) | 0x80;	// RFC 4122 variant
	sprintf(text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		"%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3],
		bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
		bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}


static void
generate_sys(proc_buffer& out, const char* name)
{
	if (strcmp(name, "kernel/osrelease") == 0)
		out.Printf("%s\n", RENGUIN_LINUX_RELEASE);
	else if (strcmp(name, "kernel/ostype") == 0)
		out.Printf("Linux\n");
	else if (strcmp(name, "kernel/hostname") == 0)
		out.Printf("%s\n", RENGUIN_LINUX_HOSTNAME);
	else if (strcmp(name, "kernel/version") == 0)
		out.Printf("%s\n", RENGUIN_LINUX_VERSION);
	else if (strcmp(name, "kernel/pid_max") == 0)
		out.Printf("4194304\n");
	else if (strcmp(name, "kernel/threads-max") == 0) {
		system_info info;
		get_system_info(&info);
		out.Printf("%" B_PRIu32 "\n", info.max_threads);
	} else if (strcmp(name, "kernel/ngroups_max") == 0)
		out.Printf("65536\n");
	else if (strcmp(name, "kernel/cap_last_cap") == 0)
		out.Printf("40\n");
	else if (strcmp(name, "kernel/random/uuid") == 0) {
		char uuid[37];
		random_uuid(uuid);
		out.Printf("%s\n", uuid);
	} else if (strcmp(name, "kernel/random/boot_id") == 0)
		out.Printf("%s\n", sBootID);
	else if (strcmp(name, "kernel/random/entropy_avail") == 0)
		out.Printf("256\n");
	else if (strcmp(name, "vm/overcommit_memory") == 0)
		out.Printf("0\n");
	else if (strcmp(name, "vm/max_map_count") == 0)
		out.Printf("65530\n");
	else if (strcmp(name, "fs/file-max") == 0)
		out.Printf("9223372036854775807\n");
	else if (strcmp(name, "fs/nr_open") == 0)
		out.Printf("1048576\n");
}


static void
generate(const proc_node& node, proc_buffer& out)
{
	switch (node.kind) {
		case KIND_ROOT_FILE:
		{
			const char* name = node.name;
			if (strcmp(name, "cpuinfo") == 0)
				generate_cpuinfo(out);
			else if (strcmp(name, "meminfo") == 0)
				generate_meminfo(out);
			else if (strcmp(name, "uptime") == 0) {
				bigtime_t uptime = system_time();
				int32 cpus = smp_get_num_cpus();
				print_fixed(out, uptime);
				out.Printf(" ");
				print_fixed(out, idle_time(cpus, uptime, NULL));
				out.Printf("\n");
			} else if (strcmp(name, "loadavg") == 0) {
				system_info info;
				get_system_info(&info);
				out.Printf("0.00 0.00 0.00 1/%" B_PRIu32 " %" B_PRId32 "\n",
					info.used_threads, thread_get_current_thread()->team->id);
			} else if (strcmp(name, "stat") == 0)
				generate_stat(out);
			else if (strcmp(name, "version") == 0) {
				out.Printf("Linux version %s (renguin@haiku) (Haiku kernel) "
					"%s\n", RENGUIN_LINUX_RELEASE, RENGUIN_LINUX_VERSION);
			} else if (strcmp(name, "filesystems") == 0) {
				out.Printf("nodev\tproc\nnodev\tdevfs\nnodev\trootfs\n"
					"nodev\tpackagefs\n\tbfs\n\tfat\n\text2\n\tiso9660\n"
					"\tntfs\n\tudf\n");
			} else if (strcmp(name, "cmdline") == 0)
				out.Printf("BOOT_IMAGE=haiku\n");
			else if (strcmp(name, "swaps") == 0) {
				out.Printf("Filename\t\t\t\tType\t\tSize\t\tUsed\t\t"
					"Priority\n");
			}
			break;
		}
		case KIND_SYS:
			generate_sys(out, node.name);
			break;
		case KIND_PID_FILE:
		case KIND_TASK_FILE:
		{
			const char* name = node.name;
			thread_id tid = node.kind == KIND_TASK_FILE ? node.tid : -1;
			if (strcmp(name, "stat") == 0)
				generate_pid_stat(out, node.pid, tid);
			else if (strcmp(name, "status") == 0)
				generate_pid_status(out, node.pid, tid);
			else if (strcmp(name, "statm") == 0) {
				uint64 virtualSize, resident;
				team_memory(node.pid, &virtualSize, &resident);
				out.Printf("%" B_PRIu64 " %" B_PRIu64 " 0 0 0 0 0\n",
					virtualSize / B_PAGE_SIZE, resident / B_PAGE_SIZE);
			} else if (strcmp(name, "comm") == 0) {
				char comm[16];
				thread_info threadInfo;
				team_info info;
				if (tid > 0 && get_thread_info(tid, &threadInfo) == B_OK)
					strlcpy(comm, threadInfo.name, sizeof(comm));
				else if (get_team_info(node.pid, &info) == B_OK)
					comm_from_args(info.args, comm, sizeof(comm));
				else
					comm[0] = '\0';
				out.Printf("%s\n", comm);
			} else if (strcmp(name, "cmdline") == 0)
				generate_strings(out, node.pid, false);
			else if (strcmp(name, "environ") == 0)
				generate_strings(out, node.pid, true);
			else if (strcmp(name, "maps") == 0)
				generate_maps(out, node.pid);
			else if (strcmp(name, "mounts") == 0)
				generate_mounts(out, false);
			else if (strcmp(name, "mountinfo") == 0)
				generate_mounts(out, true);
			else if (strcmp(name, "cgroup") == 0)
				out.Printf("0::/\n");
			else if (strcmp(name, "oom_score_adj") == 0)
				out.Printf("0\n");
			break;
		}
		default:
			break;
	}
}


// #pragma mark - directory listings


struct proc_entry {
	char	name[40];
	uint8	type;		// LX_DT_*
};

struct entry_list {
	proc_entry*	entries;
	int32		count;
	int32		capacity;

	entry_list() : entries(NULL), count(0), capacity(0) {}
	~entry_list() { free(entries); }

	void Add(const char* name, uint8 type)
	{
		if (count == capacity) {
			int32 newCapacity = max_c(capacity * 2, (int32)32);
			proc_entry* bigger = (proc_entry*)realloc(entries,
				sizeof(proc_entry) * newCapacity);
			if (bigger == NULL)
				return;
			entries = bigger;
			capacity = newCapacity;
		}
		strlcpy(entries[count].name, name, sizeof(entries[count].name));
		entries[count].type = type;
		count++;
	}
};


static void
list_directory(const proc_node& node, entry_list& list)
{
	list.Add(".", LX_DT_DIR);
	list.Add("..", LX_DT_DIR);
	char number[16];

	switch (node.kind) {
		case KIND_ROOT:
		{
			for (int i = 0; kRootFiles[i] != NULL; i++)
				list.Add(kRootFiles[i], LX_DT_REG);
			list.Add("self", LX_DT_LNK);
			list.Add("thread-self", LX_DT_LNK);
			list.Add("mounts", LX_DT_LNK);
			list.Add("sys", LX_DT_DIR);
			int32 cookie = 0;
			team_info info;
			while (get_next_team_info(&cookie, &info) == B_OK) {
				snprintf(number, sizeof(number), "%" B_PRId32, info.team);
				list.Add(number, LX_DT_DIR);
			}
			break;
		}
		case KIND_SYS:
		{
			// direct children of node.name in kSysTree
			size_t prefixLength = strlen(node.name);
			for (int i = 0; kSysTree[i] != NULL; i++) {
				const char* entry = kSysTree[i];
				if (prefixLength > 0) {
					if (strncmp(entry, node.name, prefixLength) != 0
						|| entry[prefixLength] != '/') {
						continue;
					}
					entry += prefixLength + 1;
				}
				size_t length = strlen(entry);
				if (length == 0)
					continue;
				const char* slash = strchr(entry, '/');
				if (slash != NULL && slash != entry + length - 1)
					continue;		// deeper
				char name[40];
				strlcpy(name, entry, min_c(sizeof(name),
					(size_t)(slash != NULL ? slash - entry + 1 : length + 1)));
				list.Add(name, slash != NULL ? LX_DT_DIR : LX_DT_REG);
			}
			break;
		}
		case KIND_PID_DIR:
			for (int i = 0; kPidFiles[i] != NULL; i++)
				list.Add(kPidFiles[i], LX_DT_REG);
			list.Add("exe", LX_DT_LNK);
			list.Add("cwd", LX_DT_LNK);
			list.Add("root", LX_DT_LNK);
			list.Add("fd", LX_DT_DIR);
			list.Add("task", LX_DT_DIR);
			break;
		case KIND_PID_FD_DIR:
		{
			uint32 cookie = 0;
			fd_info info;
			while (_kern_get_next_fd_info(node.pid, &cookie, &info,
					sizeof(info)) == B_OK) {
				snprintf(number, sizeof(number), "%d", info.number);
				list.Add(number, LX_DT_LNK);
			}
			break;
		}
		case KIND_PID_TASK_DIR:
		{
			int32 cookie = 0;
			thread_info info;
			while (get_next_thread_info(node.pid, &cookie, &info) == B_OK) {
				snprintf(number, sizeof(number), "%" B_PRId32, info.thread);
				list.Add(number, LX_DT_DIR);
			}
			break;
		}
		case KIND_TASK_DIR:
			for (int i = 0; kTaskFiles[i] != NULL; i++)
				list.Add(kTaskFiles[i], LX_DT_REG);
			break;
		default:
			break;
	}
}


// #pragma mark - temporary files


/*!	Puts \a size bytes into a new, already unlinked temporary file and
	returns an FD for it, at offset 0: read-only unless \a writable.
*/
static int64
open_content(const char* content, size_t size, bool writable, bool closeOnExec)
{
	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userPath = renguin_scratch_alloc(&scratch, 96);
	if (userPath == 0)
		return -LX_EFAULT;

	// A unique name; a leftover from before a crash may hold one.
	int fd = B_FILE_EXISTS;
	for (int attempt = 0; attempt < 8 && fd == B_FILE_EXISTS; attempt++) {
		char path[96];
		snprintf(path, sizeof(path), PROC_TEMP_DIR "/.renguin-proc-%" B_PRId32
			"-%" B_PRId32, thread_get_current_thread()->team->id,
			atomic_add(&sTempCounter, 1));
		if (renguin_copy_out(userPath, path, strlen(path) + 1) != B_OK)
			return -LX_EFAULT;
		fd = _user_open(-1, (const char*)(addr_t)userPath,
			O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	}
	if (fd < 0)
		return renguin_encode(fd);

	// Copy through the scratch space, a piece at a time (smaller pieces if
	// the stack has little room).
	size_t chunk = 8192;
	uint64 userBuffer = 0;
	while (chunk >= 256
		&& (userBuffer = renguin_scratch_alloc(&scratch, chunk)) == 0) {
		chunk /= 4;
	}
	status_t error = userBuffer == 0 ? B_BAD_ADDRESS : B_OK;
	for (size_t offset = 0; error == B_OK && offset < size; offset += chunk) {
		size_t length = min_c(chunk, size - offset);
		ssize_t written = B_BAD_ADDRESS;
		if (renguin_copy_out(userBuffer, content + offset, length) == B_OK) {
			written = _user_write(fd, -1, (const void*)(addr_t)userBuffer,
				length);
		}
		if (written != (ssize_t)length)
			error = written < 0 ? (status_t)written : B_IO_ERROR;
	}

	// Programs get a read-only FD, as for Linux's /proc files: writes fail.
	int result = fd;
	if (error == B_OK && !writable) {
		int readOnly = _user_open(-1, (const char*)(addr_t)userPath,
			O_RDONLY | (closeOnExec ? O_CLOEXEC : 0), 0);
		if (readOnly < 0)
			error = readOnly;
		else
			result = readOnly;
	}
	_user_unlink(-1, (const char*)(addr_t)userPath);
	if (error != B_OK) {
		_user_close(fd);
		return renguin_encode(error);
	}
	if (result != fd)
		_user_close(fd);
	else {
		if (!closeOnExec)
			_user_fcntl(fd, F_SETFD, 0);
		_user_seek(fd, 0, SEEK_SET);
	}
	return result;
}


static int64
open_placeholder_dir(const char* procPath, bool closeOnExec)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return -LX_ENOENT;

	renguin_scratch scratch;
	renguin_scratch_init(&scratch);
	uint64 userPath = renguin_scratch_put(&scratch, PROC_PLACEHOLDER,
		sizeof(PROC_PLACEHOLDER));
	if (userPath == 0)
		return -LX_EFAULT;
	_user_create_dir(-1, (const char*)(addr_t)userPath, 0555);
		// fails harmlessly once it exists
	int fd = _user_open_dir(-1, (const char*)(addr_t)userPath);
	if (fd < 0)
		return renguin_encode(fd);
	if (closeOnExec)
		_user_fcntl(fd, F_SETFD, FD_CLOEXEC);

	struct stat st;
	if (vfs_read_stat(fd, NULL, false, &st, false) != B_OK) {
		_user_close(fd);
		return -LX_EIO;
	}

	MutexLocker locker(data->lock);
	int slot = find_proc_dir_slot(data, fd);
	if (slot < 0)
		slot = find_proc_dir_slot(data, -1);
	for (int i = 0; slot < 0 && i < (int)B_COUNT_OF(data->proc_dirs); i++) {
		// Table full: reuse an entry whose FD was closed behind our back
		// (close-on-exec, close_range()).
		struct stat entryStat;
		if (vfs_read_stat(data->proc_dirs[i].fd, NULL, false, &entryStat,
				false) != B_OK
			|| entryStat.st_ino != data->proc_dirs[i].node
			|| entryStat.st_dev != data->proc_dirs[i].device) {
			slot = i;
		}
	}
	if (slot < 0) {
		_user_close(fd);
		return -LX_EMFILE;
	}
	data->proc_dirs[slot].fd = fd;
	data->proc_dirs[slot].node = st.st_ino;
	data->proc_dirs[slot].device = st.st_dev;
	data->proc_dirs[slot].position = 0;
	strlcpy(data->proc_dirs[slot].path, procPath,
		sizeof(data->proc_dirs[slot].path));
	return fd;
}


// #pragma mark - entry points


void
renguin_procfs_init()
{
	random_uuid(sBootID);
}


void
renguin_proc_init_team(renguin_team* data)
{
	for (size_t i = 0; i < B_COUNT_OF(data->proc_dirs); i++)
		data->proc_dirs[i].fd = -1;
}


static void
fill_stat(const proc_node& node, struct stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev = PROC_DEVICE;
	// a stable inode number per path
	uint64 hash = 1469598103934665603ULL;
	for (const char* p = node.path; *p != '\0'; p++)
		hash = (hash ^ (uint8)*p) * 1099511628211ULL;
	st->st_ino = (ino_t)(hash & 0x7fffffffffffffffULL);
	switch (node.type) {
		case PROC_DIR:
			st->st_mode = S_IFDIR | 0555;
			st->st_nlink = 2;
			break;
		case PROC_LINK:
			st->st_mode = S_IFLNK | 0777;
			st->st_nlink = 1;
			break;
		default:
			st->st_mode = S_IFREG
				| (node.kind == KIND_PID_FILE
					&& strcmp(node.name, "oom_score_adj") == 0 ? 0644 : 0444);
			st->st_nlink = 1;
			break;
	}
	team_info info;
	if (node.pid > 0 && get_team_info(node.pid, &info) == B_OK) {
		st->st_uid = info.uid;
		st->st_gid = info.gid;
	}
	st->st_blksize = 1024;
	bigtime_t now = real_time_clock_usecs();
	st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec
		= st->st_crtim.tv_sec = now / 1000000;
}


/*!	For the links that lead out of /proc into the file system (exe, cwd,
	root): the host path of the target, in a heap buffer the caller frees.
*/
static int64
link_host_path(const proc_node& node, char** _host)
{
	char* buffers = (char*)malloc(B_PATH_NAME_LENGTH * 2);
	if (buffers == NULL)
		return -LX_ENOMEM;
	char* host = buffers;
	char* target = buffers + B_PATH_NAME_LENGTH;
	int64 result = link_target(node, target, B_PATH_NAME_LENGTH);
	if (result != 0) {
		free(buffers);
		return result;
	}
	if (renguin_resolve_in_root(target, RENGUIN_PATH_FOLLOW_LEAF, host,
			B_PATH_NAME_LENGTH) != B_OK) {
		strlcpy(host, target, B_PATH_NAME_LENGTH);
	}
	*_host = host;
	return 0;
}


static bool
leads_out(const proc_node& node)
{
	return node.kind == KIND_PID_EXE || node.kind == KIND_PID_CWD
		|| node.kind == KIND_PID_ROOT;
}


//! stat() of what /proc/<pid>/fd/<n> refers to.
static status_t
stat_fd_target(const proc_node& node, struct stat* st)
{
	if (node.pid == thread_get_current_thread()->team->id)
		return vfs_read_stat(node.fd, NULL, false, st, false);
	uint32 cookie = 0;
	fd_info info;
	while (_kern_get_next_fd_info(node.pid, &cookie, &info, sizeof(info))
			== B_OK) {
		if (info.number == node.fd)
			return vfs_stat_node_ref(info.device, info.node, st);
	}
	return B_ENTRY_NOT_FOUND;
}


int64
renguin_proc_stat(const char* procPath, bool followLeaf, struct stat* st)
{
	proc_node node;
	if (!lookup(procPath, followLeaf, &node))
		return -LX_ENOENT;

	if (node.type == PROC_LINK && followLeaf) {
		// exe, cwd, root and fd/<n> lead out of /proc.
		if (node.kind == KIND_PID_FD) {
			status_t status = stat_fd_target(node, st);
			return status == B_OK ? 0 : renguin_encode(status);
		}
		if (leads_out(node)) {
			char* host;
			int64 result = link_host_path(node, &host);
			if (result != 0)
				return result;
			status_t status = vfs_read_stat(-1, host, true, st, true);
			free(host);
			return status == B_OK ? 0 : renguin_encode(status);
		}
	}
	fill_stat(node, st);
	return 0;
}


int64
renguin_proc_readlink(const char* procPath, char* target, size_t size)
{
	proc_node node;
	if (!lookup(procPath, false, &node))
		return -LX_ENOENT;
	if (node.type != PROC_LINK)
		return -LX_EINVAL;
	int64 result = link_target(node, target, size);
	if (result != 0)
		return result;
	return strlen(target);
}


int64
renguin_proc_open(const char* procPath, uint64 flags)
{
	bool closeOnExec = (flags & LX_O_CLOEXEC) != 0;
	bool follow = (flags & LX_O_NOFOLLOW) == 0;
	uint64 access = flags & LX_O_ACCMODE;

	proc_node node;
	if (!lookup(procPath, follow, &node)) {
		return (flags & LX_O_CREAT) != 0 ? -LX_EACCES : -LX_ENOENT;
	}

	if (node.type == PROC_LINK) {
		if (!follow)
			return -LX_ELOOP;
		if (node.kind == KIND_PID_FD
			&& node.pid == thread_get_current_thread()->team->id) {
			// Linux reopens the file; a duplicate is the closest Haiku has
			// (it shares the file position and the access mode).
			int fd = _user_dup(node.fd);
			if (fd >= 0 && closeOnExec)
				_user_fcntl(fd, F_SETFD, FD_CLOEXEC);
			if (fd >= 0)
				renguin_proc_dup(node.fd, fd);
			return fd >= 0 ? fd : renguin_encode(fd);
		}
		if (leads_out(node)) {
			if (access != LX_O_RDONLY)
				return node.kind == KIND_PID_EXE ? -LX_EACCES : -LX_EISDIR;
			char* host;
			int64 result = link_host_path(node, &host);
			if (result != 0)
				return result;
			renguin_scratch scratch;
			renguin_scratch_init(&scratch);
			uint64 userPath = renguin_scratch_put(&scratch, host,
				strlen(host) + 1);
			free(host);
			if (userPath == 0)
				return -LX_EFAULT;
			int fd = node.kind == KIND_PID_EXE
				? _user_open(-1, (const char*)(addr_t)userPath,
					O_RDONLY | (closeOnExec ? O_CLOEXEC : 0), 0)
				: _user_open_dir(-1, (const char*)(addr_t)userPath);
			if (fd >= 0 && closeOnExec && node.kind != KIND_PID_EXE)
				_user_fcntl(fd, F_SETFD, FD_CLOEXEC);
			return fd >= 0 ? fd : renguin_encode(fd);
		}
		return -LX_ENOENT;
	}

	if (node.type == PROC_DIR) {
		if (access != LX_O_RDONLY)
			return -LX_EISDIR;
		return open_placeholder_dir(node.path, closeOnExec);
	}

	if ((flags & LX_O_DIRECTORY) != 0)
		return -LX_ENOTDIR;
	bool writable = node.kind == KIND_PID_FILE
		&& strcmp(node.name, "oom_score_adj") == 0;
	if (access != LX_O_RDONLY && !writable)
		return -LX_EACCES;

	proc_buffer content;
	generate(node, content);
	if (content.failed)
		return -LX_ENOMEM;
	return open_content(content.data != NULL ? content.data : "",
		content.size, writable, closeOnExec);
}


int64
renguin_proc_access(const char* procPath, int mode)
{
	proc_node node;
	if (!lookup(procPath, true, &node))
		return -LX_ENOENT;

	if (node.type == PROC_LINK) {
		// exe, cwd, root: the target's permissions decide.
		if (leads_out(node)) {
			char* host;
			int64 result = link_host_path(node, &host);
			if (result != 0)
				return result;
			status_t status = _kern_access(-1, host, mode, false);
			free(host);
			return renguin_encode(status);
		}
		// fd/<n>: an open descriptor is accessible as it was opened.
		return 0;
	}
	if ((mode & 2) != 0 && !(node.kind == KIND_PID_FILE
			&& strcmp(node.name, "oom_score_adj") == 0)) {
		return -LX_EACCES;
	}
	if ((mode & 1) != 0 && node.type != PROC_DIR)
		return -LX_EACCES;
	return 0;
}


bool
renguin_proc_getdents(int fd, uint64 buffer, uint64 count, int64* _result)
{
	char procPath[kMaxProcPath];
	if (!proc_dir_for_fd(fd, procPath, sizeof(procPath)))
		return false;

	proc_node node;
	if (!lookup(procPath, true, &node) || node.type != PROC_DIR) {
		*_result = 0;		// gone (the process exited): empty
		return true;
	}

	entry_list list;
	list_directory(node, list);

	renguin_team* data = renguin_current_team();
	uint32 position;
	{
		MutexLocker locker(data->lock);
		int slot = find_proc_dir_slot(data, fd);
		position = slot >= 0 ? data->proc_dirs[slot].position : 0;
	}

	uint8 record[sizeof(linux_dirent64) + 48];
	uint64 written = 0;
	while ((int32)position < list.count) {
		const proc_entry& entry = list.entries[position];
		size_t nameLength = strlen(entry.name);
		size_t length = (offsetof(linux_dirent64, d_name) + nameLength + 1
			+ 7) & ~(size_t)7;
		if (written + length > count)
			break;
		memset(record, 0, sizeof(record));
		linux_dirent64* dirent = (linux_dirent64*)record;
		proc_node child;
		char childPath[kMaxProcPath];
		snprintf(childPath, sizeof(childPath), "%s/%s", procPath, entry.name);
		uint64 hash = 1469598103934665603ULL;
		for (const char* p = childPath; *p != '\0'; p++)
			hash = (hash ^ (uint8)*p) * 1099511628211ULL;
		(void)child;
		dirent->d_ino = hash & 0x7fffffffffffffffULL;
		dirent->d_off = position + 1;
		dirent->d_reclen = (uint16)length;
		dirent->d_type = entry.type;
		memcpy(dirent->d_name, entry.name, nameLength + 1);
		if (renguin_copy_out(buffer + written, record, length) != B_OK) {
			*_result = -LX_EFAULT;
			return true;
		}
		written += length;
		position++;
	}
	if (written == 0 && (int32)position < list.count) {
		*_result = -LX_EINVAL;		// buffer too small for one entry
		return true;
	}

	{
		MutexLocker locker(data->lock);
		int slot = find_proc_dir_slot(data, fd);
		if (slot >= 0)
			data->proc_dirs[slot].position = position;
	}
	*_result = written;
	return true;
}


bool
renguin_proc_lseek(int fd, int64 offset, int whence, int64* _result)
{
	char procPath[kMaxProcPath];
	if (!proc_dir_for_fd(fd, procPath, sizeof(procPath)))
		return false;
	// Positions are entry indexes: rewinddir(), and seekdir() to a d_off
	// we handed out.
	renguin_team* data = renguin_current_team();
	MutexLocker locker(data->lock);
	int slot = find_proc_dir_slot(data, fd);
	if (slot < 0) {
		*_result = -LX_EBADF;
		return true;
	}
	int64 position;
	if (whence == SEEK_SET)
		position = offset;
	else if (whence == SEEK_CUR)
		position = data->proc_dirs[slot].position + offset;
	else
		position = -1;
	if (position < 0 || position > INT32_MAX) {
		*_result = -LX_EINVAL;
		return true;
	}
	data->proc_dirs[slot].position = (uint32)position;
	*_result = position;
	return true;
}


void
renguin_proc_closed(int fd)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL)
		return;
	MutexLocker locker(data->lock);
	int slot = find_proc_dir_slot(data, fd);
	if (slot >= 0)
		data->proc_dirs[slot].fd = -1;
}


bool
renguin_proc_fstat(int fd, struct stat* st)
{
	char procPath[kMaxProcPath];
	if (!proc_dir_for_fd(fd, procPath, sizeof(procPath)))
		return false;
	proc_node node;
	if (!lookup(procPath, true, &node)) {
		// The process is gone; still a directory.
		memset(&node, 0, sizeof(node));
		node.type = PROC_DIR;
		strlcpy(node.path, procPath, sizeof(node.path));
	}
	fill_stat(node, st);
	return true;
}


void
renguin_proc_dup(int oldfd, int newfd)
{
	renguin_team* data = renguin_current_team();
	if (data == NULL || newfd < 0 || oldfd == newfd)
		return;
	char procPath[kMaxProcPath];
	bool isProcDir = proc_dir_for_fd(oldfd, procPath, sizeof(procPath));

	MutexLocker locker(data->lock);
	int slot = find_proc_dir_slot(data, newfd);
	if (slot >= 0)
		data->proc_dirs[slot].fd = -1;		// newfd was closed by the dup
	if (!isProcDir)
		return;
	int from = find_proc_dir_slot(data, oldfd);
	int to = find_proc_dir_slot(data, -1);
	if (from < 0 || to < 0)
		return;
	data->proc_dirs[to] = data->proc_dirs[from];
	data->proc_dirs[to].fd = newfd;
}


bool
renguin_proc_is_dir_fd(int fd)
{
	char procPath[kMaxProcPath];
	return proc_dir_for_fd(fd, procPath, sizeof(procPath));
}


int32
renguin_proc_super_magic()
{
	return PROC_SUPER_MAGIC;
}


int64
renguin_user_proc_path(int dirfd, uint64 userPath, char* procPath,
	size_t procPathSize)
{
	char* raw = (char*)malloc(B_PATH_NAME_LENGTH);
	if (raw == NULL)
		return -LX_ENOMEM;
	ssize_t length = renguin_copy_string_in(raw, userPath, B_PATH_NAME_LENGTH);
	int64 result;
	if (length < 0)
		result = renguin_encode(length);
	else
		result = renguin_proc_path(dirfd, raw, procPath, procPathSize) ? 1 : 0;
	free(raw);
	return result;
}
