/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/* Renga: deferred per-bit requests; see include/lkpi_deferred.h. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/taskqueue.h>

#include <KernelExport.h>

#include "lkpi_deferred.h"


static void
lkpi_deferred_bits_task(void *arg, int pending __unused)
{
	struct lkpi_deferred_bits *d = arg;
	int bits, bit;

	bits = atomic_get_and_set((int32 *)&d->pending, 0);
	for (bit = 0; bits != 0 && bit < 32; bit++) {
		if ((bits & (1 << bit)) == 0)
			continue;
		bits &= ~(1 << bit);
		d->fn(d->context, bit);
	}
}


void
lkpi_deferred_bits_init(struct lkpi_deferred_bits *d, lkpi_deferred_fn fn,
    void *context)
{
	TASK_INIT(&d->task, 0, lkpi_deferred_bits_task, d);
	d->pending = 0;
	d->fn = fn;
	d->context = context;
}


int
lkpi_deferred_bits_request(struct lkpi_deferred_bits *d, int bit)
{
	if (bit < 0 || bit >= 32)
		return (0);
	if ((atomic_or((int32 *)&d->pending, 1 << bit) & (1 << bit)) != 0)
		return (0);
	taskqueue_enqueue(taskqueue_thread, &d->task);
	return (1);
}


void
lkpi_deferred_bits_drain(struct lkpi_deferred_bits *d)
{
	atomic_get_and_set((int32 *)&d->pending, 0);
	while (taskqueue_cancel(taskqueue_thread, &d->task, NULL) != 0)
		taskqueue_drain(taskqueue_thread, &d->task);
}
