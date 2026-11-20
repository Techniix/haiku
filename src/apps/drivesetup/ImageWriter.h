/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef IMAGE_WRITER_H
#define IMAGE_WRITER_H


#include <String.h>
#include <SupportDefs.h>


/*!	Copies a disk image to a target (a partition or disk device, or a file)
	so that the target really holds the image afterwards (Renga, roadmap
	a04ee0f6): every volume on the target is unmounted first (a mounted file
	system could write its cached blocks over the new image later), every
	read and write is checked, the data is synced and the drive's cache
	flushed, and the target is read back and compared with the image.

	\a progress (may be NULL) is called with the bytes done and the total,
	once per megabyte, first while writing and then while verifying.
	Returns B_OK, or an error with a sentence for the user in \a error.
*/
typedef void (*image_progress_hook)(void* cookie, bool verifying,
	off_t done, off_t total);

status_t	write_disk_image(const char* sourcePath, const char* targetPath,
				bool verify, image_progress_hook progress, void* cookie,
				BString& error);


#endif	// IMAGE_WRITER_H
