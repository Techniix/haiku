/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * eventfd() moved to libroot and <sys/eventfd.h> to the POSIX header
 * directory (Renga, Quasar R-17); this copy stays for ports that put the
 * gnu directory first in their include path.
 */
#ifndef _GNU_SYS_EVENTFD_H
#define _GNU_SYS_EVENTFD_H


#include_next <sys/eventfd.h>


#endif	/* _GNU_SYS_EVENTFD_H */
