/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <sys/gtaskqueue.h>: the grouptask subset
 * LinuxKPI's tasklets use (linux_tasklet.c), on freebsd_network taskqueues.
 * Each attached grouptask gets its own taskqueue thread at PI_SOFT; FreeBSD's
 * per-CPU binding is not reproduced (Haiku has no public CPU-binding API).
 * Implementation: haiku/lkpi_gtaskqueue.c.
 */
#ifndef _LKPI_HAIKU_SYS_GTASKQUEUE_H_
#define _LKPI_HAIKU_SYS_GTASKQUEUE_H_

#include <sys/taskqueue.h>

typedef void gtask_fn_t(void *context);

struct grouptask {
	struct task		gt_task;	/* freebsd_network task */
	struct taskqueue	*gt_taskqueue;
	gtask_fn_t		*gt_fn;
	void			*gt_context;
	char			gt_name[32];
};

struct taskqgroup;
extern struct taskqgroup *qgroup_softirq;

#ifdef __cplusplus
extern "C" {
#endif
void	lkpi_grouptask_init(struct grouptask *gt, int prio, gtask_fn_t *fn,
	    void *ctx);
void	taskqgroup_attach_cpu(struct taskqgroup *qgroup,
	    struct grouptask *grptask, void *uniq, int cpu, void *dev,
	    void *irq, const char *name);
void	taskqgroup_detach(struct taskqgroup *qgroup, struct grouptask *gtask);
void	taskqgroup_drain_all(struct taskqgroup *qgroup);
#ifdef __cplusplus
}
#endif

#define GROUPTASK_INIT(gt, prio, fn, ctx)	lkpi_grouptask_init((gt), (prio), (fn), (ctx))
#define GROUPTASK_ENQUEUE(gt)	taskqueue_enqueue((gt)->gt_taskqueue, &(gt)->gt_task)

#endif
