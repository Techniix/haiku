/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <net/if_var.h>: freebsd_network's version plus
 * what newer FreeBSD (and LinuxKPI 802.11) expect from it.
 */
#ifndef _LKPI_HAIKU_NET_IF_VAR_H_
#define _LKPI_HAIKU_NET_IF_VAR_H_

#include_next <net/if_var.h>
#include <sys/epoch.h>		/* NET_EPOCH_*; FreeBSD gets it via if_var.h */

#ifdef __cplusplus
extern "C" {
#endif
/* Defined in freebsd_network (if.c) but not declared in its header. */
if_transmit_fn_t	if_gettransmitfn(if_t ifp);
#ifdef __cplusplus
}
#endif

#endif
