/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/socketvar.h>. LinuxKPI's linux/net.h has
 * inline kernel-socket helpers (sock_create_kern() &c.) for drivers that
 * open sockets in the kernel; wifi drivers do not. Haiku's networking is
 * not FreeBSD's, so only declarations exist here; calling socreate() fails
 * with EOPNOTSUPP (lkpi_misc.c).
 */
#ifndef _LKPI_HAIKU_SYS_SOCKETVAR_H_
#define _LKPI_HAIKU_SYS_SOCKETVAR_H_

#include <sys/types.h>

struct socket;
struct sockaddr;
struct ucred;
struct thread;

struct lkpi_protosw {
	int	(*pr_peeraddr)(struct socket *, struct sockaddr *);
	int	(*pr_sockaddr)(struct socket *, struct sockaddr *);
};

struct socket {
	short			so_state;
	struct lkpi_protosw	*so_proto;
};

#define SS_ISCONNECTED	0x0002

#ifdef __cplusplus
extern "C" {
#endif
int	socreate(int dom, struct socket **aso, int type, int proto,
	    struct ucred *cred, struct thread *td);
int	soclose(struct socket *so);
#ifdef __cplusplus
}
#endif

#endif
