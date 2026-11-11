/* Haiku overlay: extends compat/sys/priority.h with FreeBSD priority classes
 * (values as FreeBSD sys/priority.h), used by imported sys/rtprio.h. Mapping
 * to Haiku thread priorities is in haiku/lkpi_thread.c (rtp_to_pri).
 * Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_PRIORITY_H_
#define _LKPI_HAIKU_SYS_PRIORITY_H_
#include_next <sys/priority.h>
#define PRI_ITHD	1
#define PRI_REALTIME	2
#define PRI_TIMESHARE	3
#define PRI_IDLE	4
#define PRI_FIFO_BIT	8
#define PRI_FIFO	(PRI_FIFO_BIT | PRI_REALTIME)
#define PRI_BASE(P)	((P) & ~PRI_FIFO_BIT)
#define PRI_IS_REALTIME(P)	(PRI_BASE(P) == PRI_REALTIME)
#define PRI_NEED_RR(P)	((P) != PRI_FIFO)
/* swi/ithread priorities (FreeBSD sys/interrupt.h, sys/priority.h) */
#define PRI_USER	(PRI_MIN_KERN + 64)
#ifndef PI_SWI
#define PI_SWI(x)	PI_SOFT
#endif
#define SWI_TTY		0
#define SWI_NET		1
#define SWI_CAMBIO	2
#define SWI_VM		3
#define SWI_CLOCK	4
#define SWI_TQ_FAST	5
#define SWI_TQ		6
#define SWI_TQ_GIANT	6
#endif
