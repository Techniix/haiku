/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's compat/sys/module.h.
 *
 * Haiku probes hardware in init_hardware(), before any LinuxKPI code may run
 * (module_init happens later, from init_driver). MODULE_DEVICE_TABLE(pci, t)
 * expands to MODULE_PNP_INFO(), which Haiku leaves empty; here it records the
 * table in a linker set so the PCI bridge (haiku/lkpi_pci.c) can match
 * devices from the ID tables alone.
 */
#ifndef _LKPI_HAIKU_SYS_MODULE_H_
#define _LKPI_HAIKU_SYS_MODULE_H_

#include_next <sys/module.h>
#include <sys/linker_set.h>

struct lkpi_pnp_info {
	const char	*descr;		/* FreeBSD PNP descriptor string */
	const char	*bus;		/* "pci", "usb", ... */
	const void	*table;
	size_t		entsize;
	int		count;		/* entries, excluding terminator */
};

#undef MODULE_PNP_INFO
#define MODULE_PNP_INFO(d, b, unique, t, n)				\
static const struct lkpi_pnp_info lkpi_pnp_##unique = {		\
	.descr = (d), .bus = #b, .table = (t),				\
	.entsize = sizeof((t)[0]), .count = (n),			\
};									\
DATA_SET(__lkpi_pnp, lkpi_pnp_##unique)

#endif
