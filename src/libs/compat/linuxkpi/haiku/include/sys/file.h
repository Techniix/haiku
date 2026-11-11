/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/file.h>: the struct file fields that
 * LinuxKPI's linux/fs.h and linux/file.h touch. There are no FreeBSD file
 * descriptors on Haiku; LinuxKPI's linux_file layer is not built.
 */
#ifndef _LKPI_HAIKU_SYS_FILE_H_
#define _LKPI_HAIKU_SYS_FILE_H_

#include <sys/types.h>
#include <sys/selinfo.h>

struct file {
	volatile u_int	f_count;
	off_t		f_offset;
	void		*f_data;
};

#endif
