/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux extended attributes on Haiku's file attributes (setxattr,
 * getxattr, listxattr, removexattr and their l- and f- forms): a file's
 * BFS attribute NAME is the Linux attribute "user.NAME", with the same
 * bytes. So Linux tools see and keep Haiku's attributes (cp -a, rsync -X,
 * tar --xattrs, getfattr/setfattr copy a file's MIME type, icons and
 * Tracker data with it), and what a Linux program stores in user.* is a
 * normal attribute that Tracker, query and native programs see.
 *
 * Types: Linux attributes have none. Writing keeps the type of an
 * attribute that exists; a new one is B_RAW_TYPE, except for the names
 * Haiku gives a type to: BEOS:TYPE, BEOS:APP_SIG and BEOS:PREF_APP are
 * B_MIME_STRING_TYPE. Other namespaces (security., system., trusted.)
 * are EOPNOTSUPP, as on a Linux file system without them, and so is every
 * call on a volume without attributes.
 *
 * Switch: "xattrs false" in the renguin driver settings (Renguin
 * Manager, "Haiku attributes as Linux extended attributes") gives the old
 * behaviour, EOPNOTSUPP for everything.
 */


#include "renguin.h"
#include "linux_defs.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <AutoDeleter.h>
#include <StorageDefs.h>
#include <TypeConstants.h>

#include <syscalls.h>
#include <util/AutoLock.h>
#include <vfs.h>


#define LX_ENODATA			61
#define LX_E2BIG_XATTR		7

#define LX_XATTR_CREATE		0x1
#define LX_XATTR_REPLACE	0x2

#define LX_XATTR_NAME_MAX	255
#define LX_XATTR_SIZE_MAX	65536
#define LX_XATTR_LIST_MAX	65536

static const char kUserPrefix[] = "user.";
static const size_t kUserPrefixLength = sizeof(kUserPrefix) - 1;

static bool sXattrsEnabled = true;


void
renguin_xattr_settings(bool enabled)
{
	sXattrsEnabled = enabled;
}


/*!	Copies the Linux attribute name in and checks its namespace. Returns 0
	and the attribute's Haiku name in \a name (without "user."), or a
	negative Linux errno.
*/
static int64
attribute_name(uint64 userName, char* name, size_t size)
{
	char linuxName[LX_XATTR_NAME_MAX + 2];
	ssize_t length = renguin_copy_string_in(linuxName, userName,
		sizeof(linuxName));
	if (length == B_BAD_ADDRESS)
		return -LX_EFAULT;
	if (length < 0 || length > LX_XATTR_NAME_MAX)
		return -LX_ERANGE;
	if (length == 0)
		return -LX_ERANGE;
	if (strncmp(linuxName, kUserPrefix, kUserPrefixLength) != 0)
		return -LX_EOPNOTSUPP;
	if ((size_t)length == kUserPrefixLength)
		return -LX_EINVAL;
	strlcpy(name, linuxName + kUserPrefixLength, size);
	return 0;
}


//! The Haiku type of a new attribute \a name.
static uint32
new_attribute_type(const char* name)
{
	if (strcmp(name, "BEOS:TYPE") == 0 || strcmp(name, "BEOS:APP_SIG") == 0
		|| strcmp(name, "BEOS:PREF_APP") == 0) {
		return B_MIME_STRING_TYPE;
	}
	return B_RAW_TYPE;
}


struct xattr_call {
	renguin_scratch	scratch;
	int				fd;			// the file's descriptor, or -1
	uint64			path;		// user copy of the host path, or 0
	int				traverse;	// 0 or O_NOTRAVERSE
	char			name[B_ATTR_NAME_LENGTH];
	uint64			userName;	// user copy of name
};


/*!	Prepares the file part of a call: the descriptor, or the translated
	path in user memory. Returns 0 or a negative Linux errno.
*/
static int64
prepare_target(xattr_call& call, xattr_target target, uint64 file)
{
	renguin_scratch_init(&call.scratch);
	call.fd = -1;
	call.path = 0;
	call.traverse = target == XATTR_LINK ? O_NOTRAVERSE : 0;
	if (target == XATTR_FD) {
		call.fd = (int)file;
		return 0;
	}
	status_t status;
	call.path = renguin_user_path(&call.scratch, file,
		target == XATTR_LINK ? 0 : RENGUIN_PATH_FOLLOW_LEAF, &status);
	if (status == RENGUIN_STATUS_IN_PROC)
		return -LX_EOPNOTSUPP;		// /proc files have no attributes
	if (status != B_OK)
		return renguin_encode(status);
	return 0;
}


static int64
prepare_name(xattr_call& call, uint64 userName)
{
	int64 error = attribute_name(userName, call.name, sizeof(call.name));
	if (error != 0)
		return error;
	call.userName = renguin_scratch_put(&call.scratch, call.name,
		strlen(call.name) + 1);
	return call.userName == 0 ? -LX_EFAULT : 0;
}


/*!	A missing attribute is ENODATA, a missing file ENOENT; Haiku says
	B_ENTRY_NOT_FOUND for both.
*/
static int64
missing_error(xattr_call& call, status_t status)
{
	if (status != B_ENTRY_NOT_FOUND)
		return renguin_encode(status);
	if (call.fd >= 0)
		return -LX_ENODATA;
	uint64 userStat = renguin_scratch_alloc(&call.scratch, sizeof(struct stat));
	if (userStat == 0)
		return -LX_EFAULT;
	status_t statStatus = _user_read_stat(-1, (const char*)(addr_t)call.path,
		call.traverse == 0, (struct stat*)(addr_t)userStat,
		sizeof(struct stat));
	return statStatus == B_OK ? -LX_ENODATA : renguin_encode(statStatus);
}


/*!	Opens the attribute (\a openMode O_RDONLY, O_WRONLY with O_CREAT/O_TRUNC
	and \a type). Returns its descriptor or a Haiku error.
*/
static int
open_attribute(xattr_call& call, uint32 type, int openMode)
{
	return _user_open_attr(call.fd, (const char*)(addr_t)call.path,
		(const char*)(addr_t)call.userName, type, openMode | call.traverse);
}


//! The attribute's size and type (from its open descriptor).
static status_t
attribute_stat(xattr_call& call, int attribute, off_t* _size, uint32* _type)
{
	uint64 userStat = renguin_scratch_alloc(&call.scratch, sizeof(struct stat));
	if (userStat == 0)
		return B_BAD_ADDRESS;
	status_t status = _user_read_stat(attribute, NULL, false,
		(struct stat*)(addr_t)userStat, sizeof(struct stat));
	if (status != B_OK)
		return status;
	struct stat st;
	if (user_memcpy(&st, (void*)(addr_t)userStat, sizeof(st)) != B_OK)
		return B_BAD_ADDRESS;
	*_size = st.st_size;
	*_type = st.st_type;
	return B_OK;
}


int64
renguin_sys_getxattr(xattr_target target, uint64 file, uint64 userName,
	uint64 value, uint64 size)
{
	if (!sXattrsEnabled)
		return -LX_EOPNOTSUPP;
	xattr_call call;
	int64 error = prepare_target(call, target, file);
	if (error == 0)
		error = prepare_name(call, userName);
	if (error != 0)
		return error;

	int attribute = open_attribute(call, 0, O_RDONLY);
	if (attribute < 0)
		return missing_error(call, attribute);
	off_t length;
	uint32 type;
	status_t status = attribute_stat(call, attribute, &length, &type);
	int64 result;
	if (status != B_OK)
		result = renguin_encode(status);
	else if (length > LX_XATTR_SIZE_MAX)
		result = -LX_E2BIG_XATTR;
	else if (size == 0)
		result = length;		// the size the caller needs
	else if ((uint64)length > size)
		result = -LX_ERANGE;
	else {
		// straight into the program's buffer
		ssize_t bytes = _user_read(attribute, 0, (void*)(addr_t)value,
			(size_t)length);
		result = bytes < 0 ? renguin_encode(bytes) : bytes;
	}
	_user_close(attribute);
	return result;
}


int64
renguin_sys_setxattr(xattr_target target, uint64 file, uint64 userName,
	uint64 value, uint64 size, uint64 flags)
{
	if (!sXattrsEnabled)
		return -LX_EOPNOTSUPP;
	if ((flags & ~(uint64)(LX_XATTR_CREATE | LX_XATTR_REPLACE)) != 0)
		return -LX_EINVAL;
	if (size > LX_XATTR_SIZE_MAX)
		return -LX_E2BIG_XATTR;
	xattr_call call;
	int64 error = prepare_target(call, target, file);
	if (error == 0)
		error = prepare_name(call, userName);
	if (error != 0)
		return error;

	// keep the type of an attribute that exists
	uint32 type = new_attribute_type(call.name);
	bool exists = false;
	int attribute = open_attribute(call, 0, O_RDONLY);
	if (attribute >= 0) {
		off_t length;
		uint32 existingType;
		if (attribute_stat(call, attribute, &length, &existingType) == B_OK)
			type = existingType;
		exists = true;
		_user_close(attribute);
	} else if (attribute != B_ENTRY_NOT_FOUND)
		return renguin_encode(attribute);
	else {
		int64 missing = missing_error(call, attribute);
		if (missing != -LX_ENODATA)
			return missing;
	}
	if (exists && (flags & LX_XATTR_CREATE) != 0)
		return -LX_EEXIST;
	if (!exists && (flags & LX_XATTR_REPLACE) != 0)
		return -LX_ENODATA;

	attribute = open_attribute(call, type, O_WRONLY | O_CREAT | O_TRUNC);
	if (attribute < 0)
		return renguin_encode(attribute);
	int64 result = 0;
	if (size > 0) {
		ssize_t written = _user_write(attribute, 0,
			(const void*)(addr_t)value, (size_t)size);
		if (written < 0)
			result = renguin_encode(written);
		else if ((uint64)written != size)
			result = -28 /* ENOSPC */;
	}
	_user_close(attribute);
	return result;
}


int64
renguin_sys_removexattr(xattr_target target, uint64 file, uint64 userName)
{
	if (!sXattrsEnabled)
		return -LX_EOPNOTSUPP;
	xattr_call call;
	int64 error = prepare_target(call, target, file);
	if (error == 0)
		error = prepare_name(call, userName);
	if (error != 0)
		return error;

	int fd = call.fd;
	bool opened = false;
	if (fd < 0) {
		// _user_remove_attr() takes a descriptor: open the node itself
		// (its attribute directory, which needs no read permission on the
		// file's data)
		fd = _user_open_attr_dir(-1, (const char*)(addr_t)call.path,
			call.traverse == 0);
		if (fd < 0)
			return renguin_encode(fd);
		opened = true;
	}
	status_t status = _user_remove_attr(fd, (const char*)(addr_t)call.userName);
	if (opened)
		_user_close(fd);
	if (status == B_ENTRY_NOT_FOUND)
		return -LX_ENODATA;
	return renguin_encode(status);
}


int64
renguin_sys_listxattr(xattr_target target, uint64 file, uint64 list,
	uint64 size)
{
	if (!sXattrsEnabled)
		return -LX_EOPNOTSUPP;
	xattr_call call;
	int64 error = prepare_target(call, target, file);
	if (error != 0)
		return error;

	int directory = _user_open_attr_dir(call.fd,
		(const char*)(addr_t)call.path, call.traverse == 0);
	if (directory < 0)
		return renguin_encode(directory);

	char* names = (char*)malloc(LX_XATTR_LIST_MAX);
	if (names == NULL) {
		_user_close(directory);
		return -LX_ENOMEM;
	}
	MemoryDeleter namesDeleter(names);
	size_t used = 0;
	int64 result = 0;

	size_t bufferSize = sizeof(struct dirent) + B_ATTR_NAME_LENGTH;
	uint64 userEntry = renguin_scratch_alloc(&call.scratch, bufferSize);
	if (userEntry == 0)
		result = -LX_EFAULT;
	while (result == 0) {
		ssize_t count = _user_read_dir(directory,
			(struct dirent*)(addr_t)userEntry, bufferSize, 1);
		if (count < 0) {
			result = renguin_encode(count);
			break;
		}
		if (count == 0)
			break;
		char entryBuffer[sizeof(struct dirent) + B_ATTR_NAME_LENGTH];
		if (user_memcpy(entryBuffer, (void*)(addr_t)userEntry, bufferSize)
				!= B_OK) {
			result = -LX_EFAULT;
			break;
		}
		struct dirent* entry = (struct dirent*)entryBuffer;
		entryBuffer[sizeof(entryBuffer) - 1] = '\0';
		size_t length = strlen(entry->d_name);
		if (used + kUserPrefixLength + length + 1 > LX_XATTR_LIST_MAX) {
			result = -LX_E2BIG_XATTR;
			break;
		}
		memcpy(names + used, kUserPrefix, kUserPrefixLength);
		memcpy(names + used + kUserPrefixLength, entry->d_name, length + 1);
		used += kUserPrefixLength + length + 1;
	}
	_user_close(directory);
	if (result != 0)
		return result;
	if (size == 0)
		return used;
	if (used > size)
		return -LX_ERANGE;
	if (used > 0 && renguin_copy_out(list, names, used) != B_OK)
		return -LX_EFAULT;
	return used;
}
