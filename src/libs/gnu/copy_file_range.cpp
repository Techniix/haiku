/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * copy_file_range() (Linux): copies a range between two regular files,
 * from and to either explicit offsets (which are updated) or the file
 * positions. Done here with pread()/pwrite() through a buffer; that works
 * across file systems, as Linux allows since 5.19. Returns the number of
 * bytes copied, which is short at the end of the input file.
 *
 * Errors as on Linux: EINVAL for flags, non-regular files and overlapping
 * ranges in the same file; EBADF for unreadable input, unwritable output or
 * an O_APPEND output.
 */


#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <errno_private.h>


static const size_t kBufferSize = 256 * 1024;


ssize_t
copy_file_range(int inFD, off_t* inOffset, int outFD, off_t* outOffset,
	size_t length, unsigned int flags)
{
	if (flags != 0) {
		__set_errno(EINVAL);
		return -1;
	}

	int inMode = fcntl(inFD, F_GETFL);
	int outMode = fcntl(outFD, F_GETFL);
	if (inMode < 0 || outMode < 0) {
		__set_errno(EBADF);
		return -1;
	}
	if ((inMode & O_ACCMODE) == O_WRONLY
		|| (outMode & O_ACCMODE) == O_RDONLY
		|| (outMode & O_APPEND) != 0) {
		__set_errno(EBADF);
		return -1;
	}

	struct stat inStat, outStat;
	if (fstat(inFD, &inStat) != 0 || fstat(outFD, &outStat) != 0)
		return -1;
	if (!S_ISREG(inStat.st_mode) || !S_ISREG(outStat.st_mode)) {
		__set_errno(EINVAL);
		return -1;
	}

	off_t readPosition = inOffset != NULL ? *inOffset : lseek(inFD, 0, SEEK_CUR);
	off_t writePosition = outOffset != NULL
		? *outOffset : lseek(outFD, 0, SEEK_CUR);
	if (readPosition < 0 || writePosition < 0) {
		__set_errno(EINVAL);
		return -1;
	}

	if (inStat.st_dev == outStat.st_dev && inStat.st_ino == outStat.st_ino
		&& readPosition < writePosition + (off_t)length
		&& writePosition < readPosition + (off_t)length) {
		__set_errno(EINVAL);
		return -1;
	}

	if (length == 0)
		return 0;

	size_t bufferSize = length < kBufferSize ? length : kBufferSize;
	char* buffer = (char*)malloc(bufferSize);
	if (buffer == NULL) {
		__set_errno(ENOMEM);
		return -1;
	}

	size_t copied = 0;
	int error = 0;
	while (copied < length) {
		size_t chunk = length - copied;
		if (chunk > bufferSize)
			chunk = bufferSize;

		ssize_t bytesRead = pread(inFD, buffer, chunk, readPosition);
		if (bytesRead < 0) {
			error = errno;
			break;
		}
		if (bytesRead == 0)
			break;	// end of input

		ssize_t written = 0;
		while (written < bytesRead) {
			ssize_t result = pwrite(outFD, buffer + written,
				bytesRead - written, writePosition + written);
			if (result < 0) {
				error = errno;
				break;
			}
			written += result;
		}
		readPosition += written;
		writePosition += written;
		copied += written;
		if (error != 0 || written < bytesRead)
			break;
	}
	free(buffer);

	// Update the offsets or file positions by what was copied.
	if (inOffset != NULL)
		*inOffset = readPosition;
	else
		lseek(inFD, readPosition, SEEK_SET);
	if (outOffset != NULL)
		*outOffset = writePosition;
	else
		lseek(outFD, writePosition, SEEK_SET);

	if (copied == 0 && error != 0) {
		__set_errno(error);
		return -1;
	}
	return copied;
}
