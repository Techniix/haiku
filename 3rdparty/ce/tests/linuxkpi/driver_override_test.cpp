/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Which copy of a driver the kernel loads when the system package ships
	one and a non-packaged directory holds another (driver_override.h,
	used by the kernel's legacy driver loader): the mapping from a
	non-packaged or user driver path to the system's copy, and the age
	rule (an older copy gives way to the system's, a newer or equally new
	one is kept, so developers' builds still override).

	The kernel side is used on every boot of every image (the system's
	drivers are found through it); a boot with a stale copy in place needs
	real hardware or a second boot and is not part of this test.
*/


#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <driver_override.h>


#define SUITE "driver_override"

static int sFailures = 0;
static const char* kSystem = "/boot/system/add-ons";


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[512];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
check_path(const char* name, const char* path, const char* expected)
{
	char buffer[256];
	int mapped = driver_system_copy_path(path, kSystem, buffer,
		sizeof(buffer));
	bool pass = expected == NULL ? !mapped
		: mapped && strcmp(buffer, expected) == 0;
	report(name, pass, "\"%s\" -> %s", path,
		mapped ? buffer : "(the system's own, or not a driver)");
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	check_path("map_nonpackaged_bin",
		"/boot/system/non-packaged/add-ons/kernel/drivers/bin/mt7921e",
		"/boot/system/add-ons/kernel/drivers/bin/mt7921e");
	// The maintainer's stale copy lay in dev/net itself.
	check_path("map_nonpackaged_dev",
		"/boot/system/non-packaged/add-ons/kernel/drivers/dev/net/mt7921e",
		"/boot/system/add-ons/kernel/drivers/dev/net/mt7921e");
	check_path("map_user",
		"/boot/home/config/non-packaged/add-ons/kernel/drivers/bin/hda",
		"/boot/system/add-ons/kernel/drivers/bin/hda");
	check_path("system_itself",
		"/boot/system/add-ons/kernel/drivers/bin/mt7921e", NULL);
	check_path("not_a_driver",
		"/boot/system/non-packaged/add-ons/kernel/busses/pci/x", NULL);
	check_path("directory_only",
		"/boot/system/non-packaged/add-ons/kernel/drivers/", NULL);
	{
		char small[16];
		report("too_long", !driver_system_copy_path(
				"/boot/system/non-packaged/add-ons/kernel/drivers/bin/hda",
				kSystem, small, sizeof(small)),
			"a 16-byte buffer is refused");
	}

	report("older_gives_way", driver_copy_is_older(1700000000, 0,
			1800000000, 0) && driver_copy_is_older(5, 1, 5, 2),
		"older seconds or nanoseconds: the system's copy is used");
	report("newer_kept", !driver_copy_is_older(1800000000, 0, 1700000000, 0),
		"a newer copy (a developer's build) overrides");
	report("equal_kept", !driver_copy_is_older(5, 7, 5, 7),
		"equally new: the copy overrides, as in Haiku");

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
