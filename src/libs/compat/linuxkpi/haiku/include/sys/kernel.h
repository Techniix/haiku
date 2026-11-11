/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's compat/sys/kernel.h for LinuxKPI.
 *
 * Haiku's freebsd_network SYSINIT() drops the subsystem argument, honours
 * only six SI_ORDER values (sysinit.c), and runs inside init_driver.
 * LinuxKPI depends on FreeBSD's (subsystem, order) sequencing — e.g.
 * SI_SUB_LOCK < SI_SUB_TASKQ < SI_SUB_DRIVERS < module_init's
 * SI_SUB_OFED_MODINIT. In LinuxKPI translation units SYSINIT/SYSUNINIT
 * therefore go into LinuxKPI's own linker sets, which haiku/lkpi_sysinit.c
 * sorts and runs from a single entry in Haiku's own sysinit set.
 */
#ifndef _LKPI_HAIKU_SYS_KERNEL_H_
#define _LKPI_HAIKU_SYS_KERNEL_H_

#include_next <sys/kernel.h>
#include <sys/linker_set.h>

/* FreeBSD sys/kernel.h subsystem ids (values verbatim). */
#define SI_SUB_DUMMY	0x0000000
#define SI_SUB_FIRST	0x0600000
#define SI_SUB_TUNABLES	0x0700000
#define SI_SUB_COPYRIGHT	0x0800001
#define SI_SUB_NUMA	0x0810000
#define SI_SUB_VM	0x1000000
#define SI_SUB_COUNTER	0x1100000
#define SI_SUB_KMEM	0x1800000
#define SI_SUB_KENV	0x1900000
#define SI_SUB_HYPERVISOR	0x1A40000
#define SI_SUB_WITNESS	0x1A80000
#define SI_SUB_MTX_POOL_DYNAMIC	0x1AC0000
#define SI_SUB_LOCK	0x1B00000
#define SI_SUB_EVENTHANDLER	0x1C00000
#define SI_SUB_VNET_PRELINK	0x1E00000
#define SI_SUB_KLD	0x2000000
#define SI_SUB_KHELP	0x2080000
#define SI_SUB_CPU	0x2100000
#define SI_SUB_RACCT	0x2110000
#define SI_SUB_KDTRACE	0x2140000
#define SI_SUB_RANDOM	0x2160000
#define SI_SUB_MAC	0x2180000
#define SI_SUB_MAC_POLICY	0x21C0000
#define SI_SUB_MAC_LATE	0x21D0000
#define SI_SUB_VNET	0x21E0000
#define SI_SUB_INTRINSIC	0x2200000
#define SI_SUB_VM_CONF	0x2300000
#define SI_SUB_DDB_SERVICES	0x2380000
#define SI_SUB_RUN_QUEUE	0x2400000
#define SI_SUB_KTRACE	0x2480000
#define SI_SUB_OPENSOLARIS	0x2490000
#define SI_SUB_AUDIT	0x24C0000
#define SI_SUB_CREATE_INIT	0x2500000
#define SI_SUB_SCHED_IDLE	0x2600000
#define SI_SUB_MBUF	0x2700000
#define SI_SUB_INTR	0x2800000
#define SI_SUB_TASKQ	0x2880000
#define SI_SUB_EPOCH	0x2888000
#ifdef EARLY_AP_STARTUP
#define SI_SUB_SMP	0x2900000
#endif
#define SI_SUB_SOFTINTR	0x2A00000
#define SI_SUB_DEVFS	0x2F00000
#define SI_SUB_INIT_IF	0x3000000
#define SI_SUB_NETGRAPH	0x3010000
#define SI_SUB_DTRACE	0x3020000
#define SI_SUB_DTRACE_PROVIDER	0x3048000
#define SI_SUB_DTRACE_ANON	0x308C000
#define SI_SUB_DRIVERS	0x3100000
#define SI_SUB_CONFIGURE	0x3800000
#define SI_SUB_VFS	0x4000000
#define SI_SUB_CLOCKS	0x4800000
#define SI_SUB_SYSV_SHM	0x6400000
#define SI_SUB_SYSV_SEM	0x6800000
#define SI_SUB_SYSV_MSG	0x6C00000
#define SI_SUB_P1003_1B	0x6E00000
#define SI_SUB_PSEUDO	0x7000000
#define SI_SUB_EXEC	0x7400000
#define SI_SUB_PROTO_BEGIN	0x8000000
#define SI_SUB_PROTO_PFIL	0x8100000
#define SI_SUB_PROTO_MC	0x8300000
#define SI_SUB_PROTO_IF	0x8400000
#define SI_SUB_PROTO_DOMAININIT	0x8600000
#define SI_SUB_PROTO_DOMAIN	0x8800000
#define SI_SUB_PROTO_FIREWALL	0x8806000
#define SI_SUB_PROTO_IFATTACHDOMAIN	0x8808000
#define SI_SUB_PROTO_END	0x8ffffff
#define SI_SUB_KPROF	0x9000000
#define SI_SUB_KICK_SCHEDULER	0xa000000
#define SI_SUB_INT_CONFIG_HOOKS	0xa800000
#define SI_SUB_ROOT_CONF	0xb000000
#define SI_SUB_INTRINSIC_POST	0xd000000
#define SI_SUB_SYSCALLS	0xd800000
#define SI_SUB_VNET_DONE	0xdc00000
#define SI_SUB_KTHREAD_INIT	0xe000000
#define SI_SUB_KTHREAD_PAGE	0xe400000
#define SI_SUB_KTHREAD_VM	0xe800000
#define SI_SUB_KTHREAD_BUF	0xea00000
#define SI_SUB_KTHREAD_UPDATE	0xec00000
#define SI_SUB_KTHREAD_IDLE	0xee00000
#ifndef EARLY_AP_STARTUP
#define SI_SUB_SMP	0xf000000
#endif
#define SI_SUB_RACCTD	0xf100000
#define SI_SUB_LAST	0xfffffff

#define SI_ORDER_FIFTH		0x0000004
#define SI_ORDER_SIXTH		0x0000005
#define SI_ORDER_SEVENTH	0x0000006
#define SI_ORDER_EIGHTH		0x0000007
#define SI_ORDER_LAST		0xfffffff

struct lkpi_sysinit {
	const char		*name;
	unsigned int		subsystem;
	unsigned int		order;
	system_init_func_t	func;
	const void		*arg;
};

#undef SYSINIT
#undef SYSUNINIT

#define SYSINIT(uniquifier, _subsystem, _order, _func, ident)		\
static struct lkpi_sysinit lkpi_sysinit_##uniquifier = {		\
	.name		= #uniquifier,					\
	.subsystem	= (_subsystem),					\
	.order		= (_order),					\
	.func		= (system_init_func_t)(_func),			\
	.arg		= (ident),					\
};									\
DATA_SET(__lkpi_sysinit, lkpi_sysinit_##uniquifier)

#define SYSUNINIT(uniquifier, _subsystem, _order, _func, ident)		\
static struct lkpi_sysinit lkpi_sysuninit_##uniquifier = {		\
	.name		= #uniquifier,					\
	.subsystem	= (_subsystem),					\
	.order		= (_order),					\
	.func		= (system_init_func_t)(_func),			\
	.arg		= (ident),					\
};									\
DATA_SET(__lkpi_sysuninit, lkpi_sysuninit_##uniquifier)

#endif
