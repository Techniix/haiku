/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef LKPI_DEFERRED_H
#define LKPI_DEFERRED_H

/*
 * Renga: requests that must not run in the caller's context, one bit each,
 * run later on taskqueue_thread without the caller's locks; a request for a
 * bit that is still pending is merged into it.
 *
 * Made for LinuxKPI 802.11's ieee80211_start_tx_ba_session(): Linux drivers
 * call it with their own locks held, because mac80211 only queues the ADDBA
 * (see linuxkpi_ieee80211_start_tx_ba_session() in linux_80211.c). The
 * harness checks it in lkpi_pcitest ("edu: deferred_*").
 *
 * Usable from either side (LinuxKPI or Haiku types): it has no Linux types.
 */

#include <sys/param.h>
#include <sys/taskqueue.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*lkpi_deferred_fn)(void *context, int bit);

struct lkpi_deferred_bits {
	struct task		task;
	volatile int		pending;	/* bit mask */
	lkpi_deferred_fn	fn;
	void			*context;
};

void	lkpi_deferred_bits_init(struct lkpi_deferred_bits *d,
	    lkpi_deferred_fn fn, void *context);
/* 1 if queued, 0 if that bit was already pending (merged) */
int	lkpi_deferred_bits_request(struct lkpi_deferred_bits *d, int bit);
/* drops pending bits and waits for a running pass; no new ones start */
void	lkpi_deferred_bits_drain(struct lkpi_deferred_bits *d);

#ifdef __cplusplus
}
#endif

#endif
