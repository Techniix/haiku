/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Userland <-> kernel interface of a LinuxKPI-based driver.
 *
 * Shared by the kernel side (linuxkpi/haiku/lkpi_control.c) and by userland
 * (src/kits/linuxkpi, used by LinuxKPI Manager and lkpictl). Keep it plain C
 * with fixed-size fields: the same header is compiled by the kernel and by
 * 32- and 64-bit applications.
 *
 * Transport: a socket ioctl on any AF_INET socket. The network stack routes
 * it by the interface name in the first IF_NAMESIZE bytes (datalink.cpp,
 * default case), down to the driver's compat_control(), where the hook from
 * patches/haiku/0007 hands opcodes in [LKPI_CTL_FIRST, LKPI_CTL_LAST] to
 * LinuxKPI:
 *
 *	struct lkpi_control_request req = { .magic = LKPI_CONTROL_MAGIC, ... };
 *	strlcpy(req.name, "/dev/net/mt7921e/0", sizeof(req.name));
 *	ioctl(socket(AF_INET, SOCK_DGRAM, 0), LKPI_CTL_GET_INFO, &req, sizeof(req));
 *
 * Every operation is READ-ONLY. Replies are UTF-8 text, one "key=value" per
 * line, so new keys can be added without changing this ABI; readers must
 * ignore keys they do not know. Changing the request layout or the meaning
 * of an existing key requires bumping LKPI_CONTROL_VERSION.
 */
#ifndef _LINUXKPI_LKPI_CONTROL_H
#define _LINUXKPI_LKPI_CONTROL_H

#include <stdint.h>

#define LKPI_CONTROL_MAGIC	0x4c4b5049u	/* 'LKPI' */
#define LKPI_CONTROL_VERSION	1u

/* Opcodes. Chosen far above Haiku's SIOC* and B_* device opcodes. */
#define LKPI_CTL_BASE		0x4c4b0000u
#define LKPI_CTL_GET_INFO	(LKPI_CTL_BASE + 1)	/* identity, versions */
#define LKPI_CTL_GET_STATS	(LKPI_CTL_BASE + 2)	/* counters */
#define LKPI_CTL_GET_TUNABLES	(LKPI_CTL_BASE + 3)	/* settings + values */
#define LKPI_CTL_FIRST		LKPI_CTL_GET_INFO
#define LKPI_CTL_LAST		LKPI_CTL_GET_TUNABLES

/* Largest reply the kernel will produce; larger buffers are clamped. */
#define LKPI_CONTROL_MAX_REPLY	(64 * 1024)

#define LKPI_CONTROL_NAME_SIZE	32	/* == IF_NAMESIZE on Haiku */

struct lkpi_control_request {
	char		name[LKPI_CONTROL_NAME_SIZE];	/* interface; must be first */
	uint32_t	magic;		/* LKPI_CONTROL_MAGIC */
	uint32_t	version;	/* LKPI_CONTROL_VERSION */
	uint64_t	buffer;		/* user address of the reply buffer */
	uint32_t	buffer_size;	/* its size in bytes */
	uint32_t	reply_size;	/* out: bytes needed (without NUL) */
};

#endif /* _LINUXKPI_LKPI_CONTROL_H */
