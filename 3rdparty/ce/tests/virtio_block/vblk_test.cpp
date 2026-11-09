/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for the virtio_block driver's flush and trim support.

	The harness attaches two scratch disks besides the boot disk (see
	ce-test.py, SCRATCH_DISKS), in this order:

	  first   DISCARD and WRITE ZEROES offered: trims use DISCARD
	  second  only WRITE ZEROES offered: trims use WRITE ZEROES with the
	          unmap flag

	The boot disk may be a virtio disk too (ce-test.py --disk-bus virtio), so
	the test never assumes device numbers: it only uses virtio disks that
	are exactly 64 MiB and blank in their first megabyte, which a boot disk
	never is. Writing the test pattern over a boot volume would wreck the
	running system.

	On each it writes a pattern, flushes, trims parts of it and reads back:
	trimmed sectors must read as zeroes, and everything outside them -
	including the partial sectors at the edges of an unaligned range - must
	still hold the pattern. It also checks the reported trimmed size and
	ranges that reach past the end of the device.

	The scratch disks are host files opened without -snapshot, so after the
	run the harness also checks on the host that the trimmed space was given
	back (the files are sparse again).

	Output format: see 3rdparty/ce/README.md.
*/


#include <Drivers.h>
#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "vblk"

static const off_t kMiB = 1024 * 1024;
static const uint8 kPattern = 0xab;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
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


/*!	Returns the offset of the first byte in [offset, offset + size) that is
	not \a value, or -1 if all of them are.
*/
static off_t
check_range(int fd, off_t offset, off_t size, uint8 value)
{
	static uint8 buffer[256 * 1024];
	while (size > 0) {
		size_t chunk = size > (off_t)sizeof(buffer) ? sizeof(buffer) : size;
		ssize_t bytes = pread(fd, buffer, chunk, offset);
		if (bytes != (ssize_t)chunk)
			return offset;
		for (size_t i = 0; i < chunk; i++) {
			if (buffer[i] != value)
				return offset + i;
		}
		offset += chunk;
		size -= chunk;
	}
	return -1;
}


static bool
fill_range(int fd, off_t offset, off_t size, uint8 value)
{
	static uint8 buffer[256 * 1024];
	memset(buffer, value, sizeof(buffer));
	while (size > 0) {
		size_t chunk = size > (off_t)sizeof(buffer) ? sizeof(buffer) : size;
		if (pwrite(fd, buffer, chunk, offset) != (ssize_t)chunk)
			return false;
		offset += chunk;
		size -= chunk;
	}
	return true;
}


static status_t
trim(int fd, off_t offset, off_t size, uint64& trimmed)
{
	fs_trim_data data;
	data.range_count = 1;
	data.trimmed_size = 0;
	data.ranges[0].offset = offset;
	data.ranges[0].size = size;
	status_t status = ioctl(fd, B_TRIM_DEVICE, &data, sizeof(data)) == 0
		? B_OK : errno;
	trimmed = data.trimmed_size;
	return status;
}


static off_t
device_size(int fd, uint32* _sectorSize = NULL)
{
	device_geometry geometry;
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) != 0)
		return -1;
	if (_sectorSize != NULL)
		*_sectorSize = geometry.bytes_per_sector;
	return (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
		* geometry.cylinder_count * geometry.head_count;
}


/*!	Finds the harness's scratch disks among the virtio disks, in device
	order (which is the order the harness attached them in).
*/
static int
find_scratch_disks(char paths[][64], int maxCount)
{
	int count = 0;
	for (int index = 0; index < 16 && count < maxCount; index++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/disk/virtual/virtio_block/%d/raw",
			index);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		bool scratch = device_size(fd) == 64 * kMiB
			&& check_range(fd, 0, kMiB, 0) < 0;
		close(fd);
		printf("CE-INFO %s: %s\n", path,
			scratch ? "scratch disk" : "not a scratch disk, left alone");
		if (scratch)
			strlcpy(paths[count++], path, 64);
	}
	return count;
}


static void
test_disk(const char* path, int index)
{
	char prefix[16];
	snprintf(prefix, sizeof(prefix), "disk%d", index);
	char name[64];

	int fd = open(path, O_RDWR);
	snprintf(name, sizeof(name), "%s.open", prefix);
	report(name, fd >= 0, "%s: %s", path, fd >= 0 ? "ok" : strerror(errno));
	if (fd < 0)
		return;

	uint32 sectorSize = 0;
	off_t deviceSize = device_size(fd, &sectorSize);
	snprintf(name, sizeof(name), "%s.size", prefix);
	report(name, deviceSize == 64 * kMiB, "%lld bytes, %u-byte sectors",
		(long long)deviceSize, sectorSize);

	// Pattern over the first 8 MiB, made durable.
	bool written = fill_range(fd, 0, 8 * kMiB, kPattern);
	status_t flushed = ioctl(fd, B_FLUSH_DRIVE_CACHE) == 0 ? B_OK : errno;
	snprintf(name, sizeof(name), "%s.write_flush", prefix);
	report(name, written && flushed == B_OK, "write %s, flush %s",
		written ? "ok" : strerror(errno), strerror(flushed));

	// An aligned range: exactly 2 MiB gone.
	uint64 trimmed;
	status_t status = trim(fd, 1 * kMiB, 2 * kMiB, trimmed);
	off_t bad = check_range(fd, 1 * kMiB, 2 * kMiB, 0);
	off_t before = check_range(fd, 0, 1 * kMiB, kPattern);
	off_t after = check_range(fd, 3 * kMiB, 1 * kMiB, kPattern);
	snprintf(name, sizeof(name), "%s.trim_aligned", prefix);
	report(name, status == B_OK && trimmed == (uint64)(2 * kMiB) && bad < 0
			&& before < 0 && after < 0,
		"%s, trimmed %llu of %lld; zeroes %s, pattern before %s, after %s",
		strerror(status), (unsigned long long)trimmed, (long long)(2 * kMiB),
		bad < 0 ? "ok" : "BAD", before < 0 ? "kept" : "LOST",
		after < 0 ? "kept" : "LOST");

	// An unaligned range: only the whole sectors inside it may go, and the
	// partial ones at both ends must keep their data.
	off_t start = 4 * kMiB + 100;
	off_t size = 1 * kMiB;
	status = trim(fd, start, size, trimmed);
	// The first zeroed sector: trims are whole sectors, so look on sector
	// boundaries only (the device's alignment may push it further in).
	off_t firstZero = -1;
	for (off_t offset = (start + 511) / 512 * 512; offset < start + size;
			offset += 512) {
		uint8 byte;
		pread(fd, &byte, 1, offset);
		if (byte == 0) {
			firstZero = offset;
			break;
		}
	}
	bool edgesKept = check_range(fd, 4 * kMiB, 512, kPattern) < 0
		&& check_range(fd, start + size - (start + size) % 512, 512, kPattern)
			< 0;
	snprintf(name, sizeof(name), "%s.trim_unaligned", prefix);
	report(name, status == B_OK && trimmed <= (uint64)size
			&& trimmed % 512 == 0 && trimmed > 0 && edgesKept,
		"%s, trimmed %llu of %lld (multiple of 512, inside the range), "
		"partial sectors %s", strerror(status), (unsigned long long)trimmed,
		(long long)size, edgesKept ? "kept" : "LOST");

	// The trimmed sectors read as zeroes, the rest of the range as the
	// pattern, and nothing outside it changed.
	off_t zeroStart = firstZero;
	bool consistent = zeroStart >= 0
		&& check_range(fd, zeroStart, trimmed, 0) < 0
		&& check_range(fd, 4 * kMiB, zeroStart - 4 * kMiB, kPattern) < 0
		&& check_range(fd, zeroStart + trimmed,
			8 * kMiB - zeroStart - trimmed, kPattern) < 0;
	snprintf(name, sizeof(name), "%s.trim_unaligned_data", prefix);
	report(name, consistent, "zeroes from %lld for %llu bytes, pattern "
		"elsewhere: %s", (long long)zeroStart, (unsigned long long)trimmed,
		consistent ? "yes" : "no");

	// Past the end: clipped to the device.
	status = trim(fd, 63 * kMiB, 4 * kMiB, trimmed);
	snprintf(name, sizeof(name), "%s.trim_past_end", prefix);
	report(name, status == B_OK && trimmed == (uint64)kMiB,
		"%s, trimmed %llu (want %lld)", strerror(status),
		(unsigned long long)trimmed, (long long)kMiB);

	// Data written after a trim is stored normally.
	fill_range(fd, 1 * kMiB, 4096, 0x5a);
	snprintf(name, sizeof(name), "%s.write_after_trim", prefix);
	report(name, check_range(fd, 1 * kMiB, 4096, 0x5a) < 0
			&& check_range(fd, 1 * kMiB + 4096, 4096, 0) < 0,
		"rewritten block reads back, its neighbour stays zero");

	ioctl(fd, B_FLUSH_DRIVE_CACHE);
	close(fd);
}


int
main()
{
	char paths[2][64];
	int count = find_scratch_disks(paths, 2);
	report("scratch_disks", count == 2, "found %d of 2", count);
	for (int i = 0; i < count; i++)
		test_disk(paths[i], i);

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
