/* Haiku overlay: extends compat/sys/_types.h with the FreeBSD base types the
 * imported sys/rtprio.h needs. Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS__TYPES_H_
#define _LKPI_HAIKU_SYS__TYPES_H_
#include_next <sys/_types.h>
#include <stdint.h>
typedef uint16_t	__uint16_t;
typedef uint8_t		__uint8_t;
typedef int32_t		__lwpid_t;
#endif
