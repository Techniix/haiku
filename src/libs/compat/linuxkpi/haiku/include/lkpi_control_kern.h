/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Kernel-internal side of the status interface (lkpi_control.c): lets other
 * LinuxKPI parts (linuxkpi_wlan) append read-only keys to LKPI_CTL_GET_STATS.
 * Keys use the "key=value" rules of headers/private/linuxkpi/lkpi_control.h.
 * The provider runs in the caller's thread (an ioctl), may sleep, must only
 * read state, and must not use %j/%t (Haiku printf).
 */
#ifndef _LKPI_CONTROL_KERN_H_
#define _LKPI_CONTROL_KERN_H_

#ifdef __cplusplus
extern "C" {
#endif

struct lkpi_reply;
typedef void (*lkpi_control_stats_fn)(struct lkpi_reply *r);

void	lkpi_control_set_stats_provider(lkpi_control_stats_fn fn);

/*
 * Renga: data-path counters for "lkpictl wifi-stats" (LKPI_CTL_GET_STATS
 * keys perf.*): interrupts, NAPI polls and the 802.11 receive path, with
 * the time spent in each (microseconds). Cheap: two atomic adds per event.
 */
uint64_t	lkpi_perf_now(void);
void	lkpi_perf_irq(uint64_t start);
void	lkpi_perf_napi(uint64_t start, int packets);
void	lkpi_perf_rx80211(uint64_t start);
void	lkpi_perf_rxq(uint64_t start, int frames);
void	lkpi_control_add(struct lkpi_reply *r, const char *key,
	    const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#endif
