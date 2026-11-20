/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	DriveSetup's "Write image" (roadmap a04ee0f6), through its writer
	(src/apps/drivesetup/ImageWriter.cpp):
	  image_writer.exact     a 3 MB image written to a target is there byte
	                         for byte, checked and reported as B_OK
	  image_writer.mismatch  a target that takes the data but does not keep
	                         it (/dev/zero) is reported as a failure with a
	                         sentence for the user; the old writer reported
	                         success here (it never read anything back)
	  image_writer.no_target a target that cannot be opened is an error
	Output format: see 3rdparty/ce/README.md.
*/


#include "ImageWriter.h"

#include <OS.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SUITE "image_writer"

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


static void
progress(void* cookie, bool verifying, off_t done, off_t total)
{
	int* calls = (int*)cookie;
	calls[verifying ? 1 : 0]++;
}


int
main()
{
	const char* image = "/tmp/image_writer_test.img";
	const char* target = "/tmp/image_writer_test.target";
	const size_t size = 3 * 1024 * 1024 + 4096;
	char* data = (char*)malloc(size);
	srand(42);
	for (size_t i = 0; i < size; i++)
		data[i] = (char)rand();
	int fd = open(image, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	write(fd, data, size);
	close(fd);
	fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	close(fd);

	BString error;
	int calls[2] = { 0, 0 };
	status_t status = write_disk_image(image, target, true, progress, calls,
		error);
	char* back = (char*)malloc(size);
	fd = open(target, O_RDONLY);
	ssize_t got = read(fd, back, size);
	close(fd);
	report("exact", status == B_OK && got == (ssize_t)size
			&& memcmp(data, back, size) == 0 && calls[0] > 0 && calls[1] > 0,
		"status %s, %zd bytes back, equal %d, progress %d writing / %d "
		"checking", strerror(status), got, got == (ssize_t)size
			&& memcmp(data, back, size) == 0, calls[0], calls[1]);

	error = "";
	status = write_disk_image(image, "/dev/zero", true, NULL, NULL, error);
	report("mismatch", status != B_OK && !error.IsEmpty(),
		"status %s: %s", strerror(status), error.String());

	error = "";
	status = write_disk_image(image, "/nonexistent/target", true, NULL, NULL,
		error);
	report("no_target", status != B_OK && !error.IsEmpty(),
		"status %s: %s", strerror(status), error.String());

	unlink(image);
	unlink(target);
	free(data);
	free(back);
	return sFailures == 0 ? 0 : 1;
}
