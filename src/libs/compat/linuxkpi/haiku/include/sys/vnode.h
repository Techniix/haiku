/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/vnode.h>: only what linux/fs.h needs to
 * compile. LinuxKPI's character-device/VFS layer is not built on Haiku, so
 * vget()/vrele() are declared but have no implementation; using them is a
 * link error on purpose.
 */
#ifndef _LKPI_HAIKU_SYS_VNODE_H_
#define _LKPI_HAIKU_SYS_VNODE_H_

#include <sys/types.h>

struct cdev;

struct vnode {
	struct cdev	*v_rdev;
	void		*v_data;
};

#ifdef __cplusplus
extern "C" {
#endif
int	vget(struct vnode *vp, int flags);
void	vrele(struct vnode *vp);
#ifdef __cplusplus
}
#endif

#endif
