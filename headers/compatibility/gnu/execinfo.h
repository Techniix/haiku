/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * glibc's backtrace functions (also in FreeBSD's libexecinfo). Frames are
 * found with the unwind tables (no frame pointers needed); symbol names
 * come from the images' dynamic symbol tables, so static functions show
 * as the nearest exported symbol or just the image. Link with -lgnu.
 */
#ifndef _GNU_EXECINFO_H
#define _GNU_EXECINFO_H


#include <sys/cdefs.h>


__BEGIN_DECLS

int		backtrace(void** buffer, int size);
char**	backtrace_symbols(void* const* buffer, int size);
void	backtrace_symbols_fd(void* const* buffer, int size, int fd);

__END_DECLS


#endif	/* _GNU_EXECINFO_H */
