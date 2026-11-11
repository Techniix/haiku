/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Minimal FreeBSD grouptask API (include/sys/gtaskqueue.h) on Haiku's
 * freebsd_network taskqueues.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/priority.h>
#include <sys/taskqueue.h>
#include <sys/gtaskqueue.h>

static int lkpi_qgroup_softirq_token;
struct taskqgroup *qgroup_softirq =
    (struct taskqgroup *)&lkpi_qgroup_softirq_token;

#define LKPI_MAX_GROUPTASKS	LKPI_MAX_CPUS
static struct grouptask *lkpi_grouptasks[LKPI_MAX_GROUPTASKS];

static volatile u_int lkpi_grouptask_runs;

static void
lkpi_grouptask_tramp(void *context, int pending)
{
	struct grouptask *gt = context;
	u_int n = atomic_fetchadd_int(&lkpi_grouptask_runs, 1) + 1;

	/* Diagnostics (stage c): tasklets run from here. */
	if (n <= 8 || (n & 1023) == 0)
		printf("linuxkpi: grouptask '%s' run #%u\n", gt->gt_name, n);
	gt->gt_fn(gt->gt_context);
}

void
lkpi_grouptask_init(struct grouptask *gt, int prio, gtask_fn_t *fn, void *ctx)
{
	TASK_INIT(&gt->gt_task, prio, lkpi_grouptask_tramp, gt);
	gt->gt_taskqueue = NULL;
	gt->gt_fn = fn;
	gt->gt_context = ctx;
}

void
taskqgroup_attach_cpu(struct taskqgroup *qgroup, struct grouptask *gt,
    void *uniq, int cpu, void *dev, void *irq, const char *name)
{
	int i;

	strlcpy(gt->gt_name, name != NULL ? name : "lkpi gtask",
	    sizeof(gt->gt_name));
	gt->gt_taskqueue = taskqueue_create_fast(gt->gt_name, M_WAITOK,
	    taskqueue_thread_enqueue, &gt->gt_taskqueue);
	taskqueue_start_threads(&gt->gt_taskqueue, 1, PI_SOFT, "%s",
	    gt->gt_name);
	for (i = 0; i < LKPI_MAX_GROUPTASKS; i++) {
		if (lkpi_grouptasks[i] == NULL) {
			lkpi_grouptasks[i] = gt;
			break;
		}
	}
}

void
taskqgroup_detach(struct taskqgroup *qgroup, struct grouptask *gt)
{
	int i;

	if (gt->gt_taskqueue == NULL)
		return;
	taskqueue_drain(gt->gt_taskqueue, &gt->gt_task);
	taskqueue_free(gt->gt_taskqueue);
	gt->gt_taskqueue = NULL;
	for (i = 0; i < LKPI_MAX_GROUPTASKS; i++) {
		if (lkpi_grouptasks[i] == gt)
			lkpi_grouptasks[i] = NULL;
	}
}

void
taskqgroup_drain_all(struct taskqgroup *qgroup)
{
	int i;

	for (i = 0; i < LKPI_MAX_GROUPTASKS; i++) {
		if (lkpi_grouptasks[i] != NULL &&
		    lkpi_grouptasks[i]->gt_taskqueue != NULL)
			taskqueue_drain_all(lkpi_grouptasks[i]->gt_taskqueue);
	}
}
