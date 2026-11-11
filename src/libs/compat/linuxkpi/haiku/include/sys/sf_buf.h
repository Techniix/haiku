/* Haiku overlay: FreeBSD <sys/sf_buf.h>. Every page from the page shim is
 * already mapped (vm_page.lkpi_kva), so an sf_buf is just the page.
 * Copyright 2026, LinuxKPI_HaikuOS contributors. MIT. */
#ifndef _LKPI_HAIKU_SYS_SF_BUF_H_
#define _LKPI_HAIKU_SYS_SF_BUF_H_
#include <vm/vm_page.h>
#define SFB_CATCH	1
#define SFB_CPUPRIVATE	2
#define SFB_DEFAULT	0
#define SFB_NOWAIT	4
struct sf_buf;
static __inline struct sf_buf *
sf_buf_alloc(struct vm_page *m, int flags)
{
	(void)flags;
	return ((struct sf_buf *)m);
}
static __inline void *
sf_buf_kva(struct sf_buf *sf)
{
	return (((struct vm_page *)sf)->lkpi_kva);
}
static __inline struct vm_page *
sf_buf_page(struct sf_buf *sf)
{
	return ((struct vm_page *)sf);
}
static __inline void
sf_buf_free(struct sf_buf *sf)
{
	(void)sf;
}
#endif
