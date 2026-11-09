/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for the nvme_disk driver's support of several namespaces
	on one controller.

	The harness attaches one NVMe controller with two namespaces backed by
	scratch files (see ce-test.py, NVME_NAMESPACES):

	  nsid 1  48 MiB,  512-byte logical blocks
	  nsid 2  80 MiB, 4096-byte logical blocks

	Each starts with an MBR the harness wrote, holding one partition from 4
	to 8 MiB (in logical blocks, so 8192 sectors on one and 1024 on the
	other).

	The driver publishes the first active namespace of a controller as
	/dev/disk/nvme/<id>/raw (the path it always had) and every further one as
	/dev/disk/nvme/<id>/ns<nsid>/raw; the partitions appear next to them as
	.../0. The test looks for the namespaces by walking /dev/disk/nvme and
	only uses disks of exactly these sizes whose first megabyte is blank
	apart from the harness's MBR, so it can never write over a boot disk.

	For each namespace it checks the size and block size the driver reports,
	writes a distinct pattern and reads it back, and checks that writing to
	namespace 2 leaves namespace 1 alone (with a single namespace, or a
	driver that sends every request to the first namespace, the writes would
	land in the same place). It writes through each namespace's partition
	device (devfs opens partitions through the raw device's driver, with the
	partition's path, so the driver must map those to the right namespace
	too), writes an unaligned range on the 4096-byte namespace (the driver
	has to read-modify-write whole blocks), and trims a range on each
	namespace and flushes.

	The scratch files are opened without -snapshot, so after the run the
	harness checks on the host that each pattern landed in the right file at
	the right offset, that nothing else was written, and that the trimmed
	space was given back. Keep the layout below in step with ce-test.py.

	Output format: see 3rdparty/ce/README.md.
*/


#include <Drivers.h>
#include <OS.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define SUITE "nvme"

static const off_t kKiB = 1024;
static const off_t kMiB = 1024 * 1024;

struct Namespace {
	const char*	name;
	uint32		nsid;
	off_t		size;
	uint32		block_size;
	uint8		pattern;
	char		path[B_PATH_NAME_LENGTH];
	int			fd;
};

// Must match NVME_NAMESPACES in ce-test.py.
static Namespace sNamespaces[2] = {
	{ "ns1", 1, 48 * kMiB, 512, 0xa1, "", -1 },
	{ "ns2", 2, 80 * kMiB, 4096, 0xb2, "", -1 },
};

// The layout, in bytes of each namespace (the host checks the same):
//   [0, 512)                  MBR, written by the harness
//   [1 MiB, 2 MiB)            pattern, its first half trimmed later
//   [size - 4 KiB, size)      pattern (the last blocks of the namespace)
//   [4 MiB, +64 KiB)          0xe5, written through the partition device
//   [8 MiB - 4 KiB, 8 MiB)    0xe5, the partition's last 4 KiB, likewise
//   namespace 2 only:
//   [1 MiB + 768 KiB, +64 KiB)  0xc3, written after namespace 1 was checked
//   [2 MiB - 4086, +100)        0xd4, an unaligned write
static const off_t kPatternStart = 1 * kMiB;
static const off_t kPatternSize = 1 * kMiB;
static const off_t kTrimSize = 512 * kKiB;
static const off_t kTailSize = 4 * kKiB;
static const off_t kCrossStart = 1 * kMiB + 768 * kKiB;
static const off_t kCrossSize = 64 * kKiB;
static const uint8 kCrossPattern = 0xc3;
static const off_t kUnalignedStart = 2 * kMiB - 4086;
static const off_t kUnalignedSize = 100;
static const uint8 kUnalignedPattern = 0xd4;
static const off_t kMbrBlock = 4 * kKiB;
	// the first block of either namespace, which holds the MBR
static const off_t kPartitionStart = 4 * kMiB;
static const off_t kPartitionSize = 4 * kMiB;
static const uint8 kPartitionPattern = 0xe5;

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
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
	static uint8 buffer[1024 * 1024];
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


/*!	Writes \a size bytes of \a value at \a offset. The buffer is 1 MiB, so
	one write can be larger than the controller's maximum transfer size
	(QEMU's default MDTS is 512 KiB) and has to be split by the driver.
*/
static bool
fill_range(int fd, off_t offset, off_t size, uint8 value)
{
	static uint8 buffer[1024 * 1024];
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


static const char*
range_result(off_t bad)
{
	static char text[4][48];
	static int next = 0;
	char* result = text[next++ % 4];
	if (bad < 0)
		strlcpy(result, "ok", sizeof(text[0]));
	else
		snprintf(result, sizeof(text[0]), "WRONG at %lld", (long long)bad);
	return result;
}


static off_t
geometry_size(int fd, uint32* _blockSize)
{
	device_geometry geometry;
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) != 0)
		return -1;
	*_blockSize = geometry.bytes_per_sector;
	return (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
		* geometry.cylinder_count * geometry.head_count;
}


/*!	Walks \a directory (recursively) for "raw" devices and remembers those
	that are one of the scratch namespaces: exactly the expected size and
	blank in their first megabyte.
*/
static void
find_namespaces(const char* directory, int depth)
{
	DIR* dir = opendir(directory);
	if (dir == NULL || depth > 3) {
		if (dir != NULL)
			closedir(dir);
		return;
	}

	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		struct stat st;
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			find_namespaces(path, depth + 1);
			continue;
		}
		if (strcmp(entry->d_name, "raw") != 0)
			continue;

		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			printf("CE-INFO %s: %s\n", path, strerror(errno));
			continue;
		}
		uint32 blockSize = 0;
		off_t size = geometry_size(fd, &blockSize);
		uint8 mbr[512];
		bool blank = pread(fd, mbr, sizeof(mbr), 0) == (ssize_t)sizeof(mbr)
			&& mbr[510] == 0x55 && mbr[511] == 0xaa
			&& check_range(fd, kMbrBlock, kMiB - kMbrBlock, 0) < 0;
		close(fd);

		const char* role = "not a scratch namespace, left alone";
		for (int i = 0; i < 2; i++) {
			Namespace& ns = sNamespaces[i];
			if (size == ns.size && blank && ns.path[0] == '\0') {
				strlcpy(ns.path, path, sizeof(ns.path));
				role = ns.name;
				break;
			}
		}
		printf("CE-INFO %s: %lld bytes, %u-byte blocks, %s: %s\n", path,
			(long long)size, blockSize, blank ? "blank" : "not blank", role);
	}
	closedir(dir);
}


/*!	The path the driver should give namespace \a ns: the first namespace
	keeps disk/nvme/<id>/raw, the second one is disk/nvme/<id>/ns2/raw in
	the same controller directory.
*/
static void
test_paths()
{
	Namespace& first = sNamespaces[0];
	Namespace& second = sNamespaces[1];

	int id = -1;
	char rest[64] = "";
	bool firstOk = sscanf(first.path, "/dev/disk/nvme/%d/%63s", &id, rest)
			== 2 && strcmp(rest, "raw") == 0;
	report("path_ns1", firstOk, "%s (want /dev/disk/nvme/<id>/raw)",
		first.path[0] != '\0' ? first.path : "not found");

	char expected[B_PATH_NAME_LENGTH];
	snprintf(expected, sizeof(expected), "/dev/disk/nvme/%d/ns2/raw", id);
	report("path_ns2", firstOk && strcmp(second.path, expected) == 0,
		"%s (want %s)", second.path[0] != '\0' ? second.path : "not found",
		firstOk ? expected : "/dev/disk/nvme/<id>/ns2/raw");
}


static void
test_geometry(Namespace& ns)
{
	char name[64];
	uint32 blockSize = 0;
	off_t size = geometry_size(ns.fd, &blockSize);
	size_t deviceSize = 0;
	status_t status = ioctl(ns.fd, B_GET_DEVICE_SIZE, &deviceSize,
		sizeof(deviceSize)) == 0 ? B_OK : errno;

	snprintf(name, sizeof(name), "%s.geometry", ns.name);
	report(name, size == ns.size && blockSize == ns.block_size
			&& status == B_OK && (off_t)deviceSize == ns.size,
		"geometry %lld bytes in %u-byte blocks, B_GET_DEVICE_SIZE %s %llu "
		"(want %lld, %u)", (long long)size, blockSize, strerror(status),
		(unsigned long long)deviceSize, (long long)ns.size, ns.block_size);

	char model[256] = "";
	status = ioctl(ns.fd, B_GET_DEVICE_NAME, model, sizeof(model)) == 0
		? B_OK : errno;
	snprintf(name, sizeof(name), "%s.device_name", ns.name);
	report(name, status == B_OK && model[0] != '\0', "%s: \"%s\"",
		strerror(status), model);
}


static void
test_write(Namespace& ns)
{
	char name[64];
	bool written = fill_range(ns.fd, kPatternStart, kPatternSize, ns.pattern)
		&& fill_range(ns.fd, ns.size - kTailSize, kTailSize, ns.pattern);
	status_t flushed = ioctl(ns.fd, B_FLUSH_DRIVE_CACHE) == 0 ? B_OK : errno;
	off_t body = check_range(ns.fd, kPatternStart, kPatternSize, ns.pattern);
	off_t tail = check_range(ns.fd, ns.size - kTailSize, kTailSize,
		ns.pattern);
	off_t before = check_range(ns.fd, kMbrBlock, kPatternStart - kMbrBlock,
		0);
	off_t after = check_range(ns.fd, kPatternStart + kPatternSize, kMiB, 0);

	snprintf(name, sizeof(name), "%s.write_read", ns.name);
	report(name, written && flushed == B_OK && body < 0 && tail < 0
			&& before < 0 && after < 0,
		"write %s, flush %s; pattern 0x%02x %s, last 4 KiB %s, zeroes before "
		"%s, after %s", written ? "ok" : strerror(errno), strerror(flushed),
		ns.pattern, range_result(body), range_result(tail),
		range_result(before), range_result(after));
}


/*!	Namespace 1 must still read exactly as namespace 1 was written. */
static bool
ns1_intact(off_t& bad)
{
	Namespace& ns = sNamespaces[0];
	bad = check_range(ns.fd, kMbrBlock, kPatternStart - kMbrBlock, 0);
	if (bad < 0)
		bad = check_range(ns.fd, kPatternStart, kPatternSize, ns.pattern);
	if (bad < 0)
		bad = check_range(ns.fd, kPatternStart + kPatternSize, kMiB, 0);
	if (bad < 0)
		bad = check_range(ns.fd, ns.size - kTailSize, kTailSize, ns.pattern);
	return bad < 0;
}


static void
test_cross_talk()
{
	Namespace& ns1 = sNamespaces[0];
	Namespace& ns2 = sNamespaces[1];

	// Both were written over the same byte range; each must have its own.
	off_t bad1 = check_range(ns1.fd, kPatternStart, kPatternSize, ns1.pattern);
	off_t bad2 = check_range(ns2.fd, kPatternStart, kPatternSize, ns2.pattern);
	report("independent", bad1 < 0 && bad2 < 0,
		"same range, ns1 0x%02x %s, ns2 0x%02x %s", ns1.pattern,
		range_result(bad1), ns2.pattern, range_result(bad2));

	// A further write to namespace 2 only changes namespace 2.
	bool written = fill_range(ns2.fd, kCrossStart, kCrossSize, kCrossPattern);
	off_t bad = check_range(ns2.fd, kCrossStart, kCrossSize, kCrossPattern);
	off_t ns1Bad;
	bool intact = ns1_intact(ns1Bad);
	report("ns2_write_leaves_ns1", written && bad < 0 && intact,
		"0x%02x to ns2 at %lld: write %s, read back %s; ns1 %s",
		kCrossPattern, (long long)kCrossStart, written ? "ok" : strerror(errno),
		range_result(bad), intact ? "unchanged" : range_result(ns1Bad));
}


/*!	Opens the partition the harness put on \a ns (published as "0" in the
	namespace's directory), checks where the kernel says it is, writes
	through it and checks on the raw device that the data landed in this
	namespace at the partition's offset and nowhere past its end.
*/
static void
test_partition(Namespace& ns)
{
	char name[64];
	snprintf(name, sizeof(name), "%s.partition", ns.name);

	char path[B_PATH_NAME_LENGTH];
	strlcpy(path, ns.path, sizeof(path));
	char* slash = strrchr(path, '/');
	if (slash == NULL) {
		report(name, false, "bad path %s", ns.path);
		return;
	}
	strlcpy(slash + 1, "0", sizeof(path) - (slash + 1 - path));

	// The disk device manager publishes partitions shortly after the disk.
	int fd = -1;
	for (int i = 0; i < 50 && fd < 0; i++) {
		fd = open(path, O_RDWR);
		if (fd < 0)
			snooze(100000);
	}
	if (fd < 0) {
		report(name, false, "%s: %s", path, strerror(errno));
		return;
	}

	partition_info info;
	memset(&info, 0, sizeof(info));
	status_t status = ioctl(fd, B_GET_PARTITION_INFO, &info, sizeof(info))
		== 0 ? B_OK : errno;
	// The MBR counts in logical blocks (1024 of 4096 bytes on namespace 2),
	// so the byte offset also shows the block size was used. The kernel
	// publishes partition devices before their block size is known, so
	// info.logical_block_size is 0 here whatever the disk; it is only shown.
	bool placed = status == B_OK && info.offset == kPartitionStart
		&& info.size == kPartitionSize;

	bool written = fill_range(fd, 0, 64 * kKiB, kPartitionPattern)
		&& fill_range(fd, kPartitionSize - 4 * kKiB, 4 * kKiB,
			kPartitionPattern);
	// Writing past the end of the partition must not reach the disk.
	char byte = 0x66;
	ssize_t pastEnd = pwrite(fd, &byte, 1, kPartitionSize);
	ioctl(fd, B_FLUSH_DRIVE_CACHE);
	close(fd);

	off_t start = check_range(ns.fd, kPartitionStart, 64 * kKiB,
		kPartitionPattern);
	off_t end = check_range(ns.fd, kPartitionStart + kPartitionSize - 4 * kKiB,
		4 * kKiB, kPartitionPattern);
	off_t beyond = check_range(ns.fd, kPartitionStart + kPartitionSize,
		64 * kKiB, 0);
	report(name, placed && written && pastEnd <= 0 && start < 0 && end < 0
			&& beyond < 0,
		"%s: info %s, offset %lld size %lld (want %lld, %lld; block %d); "
		"write %s, past end %zd; on raw device start %s, end %s, after %s",
		path, strerror(status), (long long)info.offset, (long long)info.size,
		(long long)kPartitionStart, (long long)kPartitionSize,
		(int)info.logical_block_size, written ? "ok" : "failed",
		pastEnd, range_result(start), range_result(end),
		range_result(beyond));
}


/*!	A write that starts and ends inside 4096-byte blocks: the driver bounces
	it and must keep the rest of both blocks.
*/
static void
test_unaligned(Namespace& ns)
{
	uint8 data[kUnalignedSize];
	memset(data, kUnalignedPattern, sizeof(data));
	ssize_t bytes = pwrite(ns.fd, data, sizeof(data), kUnalignedStart);

	off_t block = kUnalignedStart / ns.block_size * ns.block_size;
	off_t bad = check_range(ns.fd, kUnalignedStart, kUnalignedSize,
		kUnalignedPattern);
	off_t before = check_range(ns.fd, block, kUnalignedStart - block,
		ns.pattern);
	off_t afterStart = kUnalignedStart + kUnalignedSize;
	off_t after = check_range(ns.fd, afterStart,
		kPatternStart + kPatternSize - afterStart, ns.pattern);

	char name[64];
	snprintf(name, sizeof(name), "%s.unaligned_write", ns.name);
	report(name, bytes == kUnalignedSize && bad < 0 && before < 0
			&& after < 0,
		"%zd bytes at %lld (%u-byte blocks): data %s, rest of block before "
		"%s, after %s", bytes, (long long)kUnalignedStart, ns.block_size,
		range_result(bad), range_result(before), range_result(after));
}


static void
test_trim(Namespace& ns)
{
	fs_trim_data data;
	data.range_count = 1;
	data.trimmed_size = 0;
	data.ranges[0].offset = kPatternStart;
	data.ranges[0].size = kTrimSize;
	status_t status = ioctl(ns.fd, B_TRIM_DEVICE, &data, sizeof(data)) == 0
		? B_OK : errno;
	status_t flushed = ioctl(ns.fd, B_FLUSH_DRIVE_CACHE) == 0 ? B_OK : errno;

	off_t zeroes = check_range(ns.fd, kPatternStart, kTrimSize, 0);
	// The rest of the pattern is kept (namespace 2 has its other writes in
	// there; those are checked on the host).
	off_t kept = check_range(ns.fd, kPatternStart + kTrimSize, 64 * kKiB,
		ns.pattern);

	char name[64];
	snprintf(name, sizeof(name), "%s.trim", ns.name);
	report(name, status == B_OK && flushed == B_OK
			&& data.trimmed_size == (uint64)kTrimSize && zeroes < 0
			&& kept < 0,
		"%s, trimmed %llu of %lld, flush %s; zeroes %s, pattern after %s",
		strerror(status), (unsigned long long)data.trimmed_size,
		(long long)kTrimSize, strerror(flushed), range_result(zeroes),
		range_result(kept));
}


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	find_namespaces("/dev/disk/nvme", 0);
	int found = 0;
	for (int i = 0; i < 2; i++) {
		if (sNamespaces[i].path[0] != '\0')
			found++;
	}
	report("namespaces", found == 2, "found %d of 2 (ns1 %s, ns2 %s)", found,
		sNamespaces[0].path[0] != '\0' ? sNamespaces[0].path : "missing",
		sNamespaces[1].path[0] != '\0' ? sNamespaces[1].path : "missing");
	test_paths();

	for (int i = 0; i < 2; i++) {
		Namespace& ns = sNamespaces[i];
		char name[64];
		snprintf(name, sizeof(name), "%s.open", ns.name);
		if (ns.path[0] == '\0') {
			report(name, false, "namespace %u not found", ns.nsid);
			continue;
		}
		ns.fd = open(ns.path, O_RDWR);
		report(name, ns.fd >= 0, "%s: %s", ns.path,
			ns.fd >= 0 ? "ok" : strerror(errno));
	}

	for (int i = 0; i < 2; i++) {
		if (sNamespaces[i].fd < 0)
			continue;
		test_geometry(sNamespaces[i]);
		test_write(sNamespaces[i]);
		test_partition(sNamespaces[i]);
	}

	if (sNamespaces[0].fd >= 0 && sNamespaces[1].fd >= 0) {
		test_cross_talk();
		test_unaligned(sNamespaces[1]);
	} else {
		report("independent", false, "needs both namespaces");
		report("ns2_write_leaves_ns1", false, "needs both namespaces");
	}

	for (int i = 0; i < 2; i++) {
		if (sNamespaces[i].fd < 0)
			continue;
		test_trim(sNamespaces[i]);
		close(sNamespaces[i].fd);
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
