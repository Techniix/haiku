/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DRIVER_OVERRIDE_H
#define _DRIVER_OVERRIDE_H


/*!	Which copy of a legacy driver the kernel loads when the system ships it
	and a non-packaged (or user) add-on directory has one of the same name
	(Renga, roadmap F-26 follow-up).

	Haiku loads the copy in the directory searched first: the user's
	non-packaged, the user's, the system's non-packaged add-ons, then the
	system's (packaged) ones. That lets a developer try a new build of a
	driver without making a package, and Renga keeps it: a copy outside the
	system package still wins if it is at least as new as the system's.
	A copy that is older than the one the system package now carries is a
	leftover from before the system shipped the driver (or shipped a newer
	one), built against older kernel and compatibility code: the kernel uses
	the system's copy instead and says so in the syslog.

	Header-only and free of kernel types, so that the path mapping can be
	tested in userland (3rdparty/ce/tests/posix_compat/driver_override_test).
*/


#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif


/*!	The path of the system package's copy of the driver at \a path: the part
	of \a path from "kernel/drivers/" on, appended to \a systemAddOns (the
	system add-ons directory, ".../system/add-ons"). Returns 0 if \a path is
	not a driver path, already lies in \a systemAddOns, or the result does
	not fit into \a size bytes.
*/
static inline int
driver_system_copy_path(const char* path, const char* systemAddOns,
	char* buffer, size_t size)
{
	static const char kDrivers[] = "/kernel/drivers/";
	const char* tail;
	size_t baseLength;
	size_t tailLength;

	if (path == NULL || systemAddOns == NULL || buffer == NULL)
		return 0;
	baseLength = strlen(systemAddOns);
	while (baseLength > 0 && systemAddOns[baseLength - 1] == '/')
		baseLength--;
	if (baseLength == 0)
		return 0;
	if (strncmp(path, systemAddOns, baseLength) == 0
		&& strncmp(path + baseLength, kDrivers, sizeof(kDrivers) - 1) == 0)
		return 0;

	tail = strstr(path, kDrivers);
	if (tail == NULL || tail[sizeof(kDrivers) - 1] == '\0')
		return 0;
	tailLength = strlen(tail);
	if (baseLength + tailLength + 1 > size)
		return 0;
	memcpy(buffer, systemAddOns, baseLength);
	memcpy(buffer + baseLength, tail, tailLength + 1);
	return 1;
}


/*!	Whether a copy modified at \a copySeconds/\a copyNanoseconds is older
	than the system's, modified at \a systemSeconds/\a systemNanoseconds,
	so that the system's is to be used. Equal times keep the copy (a
	developer's build made from the same sources).
*/
static inline int
driver_copy_is_older(long long copySeconds, long copyNanoseconds,
	long long systemSeconds, long systemNanoseconds)
{
	if (copySeconds != systemSeconds)
		return copySeconds < systemSeconds;
	return copyNanoseconds < systemNanoseconds;
}


#ifdef __cplusplus
}
#endif


#endif	// _DRIVER_OVERRIDE_H
