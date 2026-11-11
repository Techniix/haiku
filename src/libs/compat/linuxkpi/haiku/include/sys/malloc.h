/* Haiku overlay: extends compat/sys/malloc.h with the FreeBSD malloc(9) flags
 * LinuxKPI's gfp.h asserts against (values as FreeBSD sys/malloc.h). Haiku's
 * _kernel_malloc ignores flags other than M_NOWAIT/M_WAITOK/M_ZERO.
 * Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_MALLOC_H_
#define _LKPI_HAIKU_SYS_MALLOC_H_
#include_next <sys/malloc.h>
#ifndef M_NORECLAIM
#define M_NORECLAIM	0x0080
#endif
#ifndef M_NOVM
#define M_NOVM		0x0200
#endif
#ifndef M_USE_RESERVE
#define M_USE_RESERVE	0x0400
#endif
#ifndef M_NODUMP
#define M_NODUMP	0x0800
#endif
#ifndef M_EXEC
#define M_EXEC		0x4000
#endif

/* FreeBSD kernel strdup(9)/realloc(9) take a malloc type (and flags). Haiku's
 * kernel has the libc forms; M_NOWAIT is not honoured by realloc here. */
char	*lkpi_kern_strdup(const char *s);
#define strdup(s, type)			lkpi_kern_strdup(s)
#define realloc(p, size, type, flags)	_kernel_realloc_lkpi((p), (size), (flags))
void	*_kernel_realloc_lkpi(void *p, size_t size, int flags);
#endif
