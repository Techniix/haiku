/* Haiku overlay: extends compat/sys/smp.h. Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_SMP_H_
#define _LKPI_HAIKU_SYS_SMP_H_
#include_next <sys/smp.h>
#define mp_ncores	mp_ncpus	/* Haiku exposes no core/thread split here */
#endif
