/* Haiku overlay: FreeBSD <sys/nv.h>. Only the opaque type is needed (SR-IOV
 * prototypes in linux/pci.h); Haiku has no nvlist and no SR-IOV.
 * Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_NV_H_
#define _LKPI_HAIKU_SYS_NV_H_
typedef struct nvlist nvlist_t;
#endif
