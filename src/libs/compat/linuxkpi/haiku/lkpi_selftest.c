/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Load-time self-test of the LinuxKPI primitives a driver's command path
 * relies on (mt76 sends an MCU command, then sleeps in wait_event_timeout()
 * until its interrupt tasklet / NAPI work queues the reply and wakes it):
 *
 *   1. wait_event_timeout() times out when nobody wakes it
 *   2. a kthread wakes a waiter (wake_up() across threads)
 *   3. a tasklet runs and completes a completion
 *   4. queue_work() on system_wq runs and completes a completion
 *   5. msleep()/jiffies advance at 1 per ms
 *   6. page_pool hands out pages (mt76 fills its RX rings from it), below
 *      4 GiB for GFP_DMA32, and virt_to_head_page() finds them again
 *   7. idr_remove() of an id that is not allocated returns NULL and leaves
 *      the idr intact, as in Linux (mt76's tx-status timeout path removes
 *      id 0, which is never allocated; FreeBSD's copy panicked)
 *   8. idr stress: 20000 rounds of allocations and removals in mt76's
 *      pattern (ids from 1, removals of live and of free ids), checked
 *      against a shadow table
 *
 * Results go to syslog as "linuxkpi: selftest ...". Runs once per driver
 * load, after all other LinuxKPI SYSINITs; takes about 100 ms.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <net/page_pool/helpers.h>
#include <linux/idr.h>

static DECLARE_WAIT_QUEUE_HEAD(st_wq);
static volatile int st_flag;
static struct completion st_tasklet_done, st_work_done;
static struct tasklet_struct st_tasklet;
static struct work_struct st_work;

static int
st_waker(void *arg)
{
	msleep(20);
	st_flag = 1;
	wake_up(&st_wq);
	return (0);
}

static void
st_tasklet_fn(unsigned long data)
{
	complete(&st_tasklet_done);
}

static void
st_work_fn(struct work_struct *work)
{
	complete(&st_work_done);
}

static void
lkpi_selftest(void *arg __unused)
{
	struct task_struct *t;
	unsigned long j0, j1;
	long r;

	/* 1 */
	j0 = jiffies;
	r = wait_event_timeout(st_wq, st_flag != 0, msecs_to_jiffies(30));
	j1 = jiffies;
	printf("linuxkpi: selftest 1 timeout: %s (ret %ld, %lu ms)\n",
	    r == 0 && j1 - j0 >= 25 ? "OK" : "FAIL", r, j1 - j0);

	/* 2 */
	st_flag = 0;
	t = kthread_run(st_waker, NULL, "lkpi_selftest");
	if (IS_ERR_OR_NULL(t)) {
		printf("linuxkpi: selftest 2 wake: FAIL (kthread_run)\n");
	} else {
		j0 = jiffies;
		r = wait_event_timeout(st_wq, st_flag != 0,
		    msecs_to_jiffies(1000));
		j1 = jiffies;
		printf("linuxkpi: selftest 2 wake: %s (ret %ld, %lu ms)\n",
		    r > 0 && st_flag ? "OK" : "FAIL", r, j1 - j0);
	}

	/* 3 */
	init_completion(&st_tasklet_done);
	tasklet_init(&st_tasklet, st_tasklet_fn, 0);
	tasklet_schedule(&st_tasklet);
	r = wait_for_completion_timeout(&st_tasklet_done,
	    msecs_to_jiffies(1000));
	printf("linuxkpi: selftest 3 tasklet: %s\n", r > 0 ? "OK" : "FAIL");
	tasklet_kill(&st_tasklet);

	/* 4 */
	init_completion(&st_work_done);
	INIT_WORK(&st_work, st_work_fn);
	queue_work(system_wq, &st_work);
	r = wait_for_completion_timeout(&st_work_done, msecs_to_jiffies(1000));
	printf("linuxkpi: selftest 4 workqueue: %s\n", r > 0 ? "OK" : "FAIL");

	/* 5 */
	j0 = jiffies;
	msleep(50);
	j1 = jiffies;
	printf("linuxkpi: selftest 5 jiffies: %s (%lu ms for msleep(50))\n",
	    j1 - j0 >= 45 && j1 - j0 < 200 ? "OK" : "FAIL", j1 - j0);

	/* 6 */
	{
		struct page_pool_params pp_params = { 0 };
		struct page_pool *pp;
		struct page *pg;
		uint32_t off = 0;

		pp_params.order = 0;
		pp_params.pool_size = 4;
		pp = page_pool_create(&pp_params);
		if (IS_ERR_OR_NULL(pp)) {
			printf("linuxkpi: selftest 6 page_pool: FAIL (create)\n");
		} else {
			pg = page_pool_alloc_frag(pp, &off, 2048,
			    GFP_KERNEL | GFP_DMA32);
			/* also: address -> struct page (virt_to_head_page()) */
			printf("linuxkpi: selftest 6 page_pool: %s (phys %#llx)\n",
			    pg != NULL && page_to_phys(pg) < (1ULL << 32) &&
			    virt_to_head_page((char *)page_address(pg) + 100) == pg ?
			    "OK" : "FAIL",
			    pg != NULL ? (unsigned long long)page_to_phys(pg) : 0ULL);
			if (pg != NULL)
				page_pool_put_full_page(pp, pg, false);
			page_pool_destroy(pp);
		}
	}

	/* 7 */
	{
		struct idr idr;
		int a, b, ok;
		static int x, y;

		idr_init(&idr);
		a = idr_alloc(&idr, &x, 1, 0x80, GFP_KERNEL);
		b = idr_alloc(&idr, &y, 1, 0x80, GFP_KERNEL);
		ok = a == 1 && b == 2
		    && idr_remove(&idr, 0) == NULL	/* never allocated */
		    && idr_remove(&idr, 77) == NULL	/* beyond, free */
		    && idr_remove(&idr, a) == &x
		    && idr_remove(&idr, a) == NULL	/* twice */
		    && idr_find(&idr, b) == &y
		    && idr_alloc(&idr, &x, 1, 0x80, GFP_KERNEL) == 1;
		idr_destroy(&idr);
		printf("linuxkpi: selftest 7 idr_remove_free: %s\n",
		    ok ? "OK" : "FAIL");
	}

	/* 8 */
	{
		static void *shadow[128];
		static int objects[128];
		struct idr idr;
		uint32_t seed = 12345;
		int round, id, bad = 0;

		memset(shadow, 0, sizeof(shadow));
		idr_init(&idr);
		for (round = 0; round < 20000 && bad == 0; round++) {
			seed = seed * 1103515245 + 12345;
			if ((seed >> 16) % 3 != 0) {
				/* allocate, as mt76_tx_status_skb_add() */
				int slot = (seed >> 8) % 128;
				id = idr_alloc(&idr, &objects[slot], 1, 128,
				    GFP_ATOMIC);
				if (id >= 1 && id < 128) {
					if (shadow[id] != NULL)
						bad = 1;
					shadow[id] = &objects[slot];
				} else if (id != -ENOSPC)
					bad = 2;
			} else {
				/* remove a live or a free id, as the timeout
				 * path may */
				id = (seed >> 9) % 128;
				if (idr_remove(&idr, id) != shadow[id])
					bad = 3;
				shadow[id] = NULL;
			}
		}
		for (id = 0; id < 128 && bad == 0; id++) {
			if (idr_find(&idr, id) != shadow[id])
				bad = 4;
		}
		idr_destroy(&idr);
		printf("linuxkpi: selftest 8 idr_stress: %s (%d rounds, error "
		    "%d)\n", bad == 0 ? "OK" : "FAIL", round, bad);
	}
}
SYSINIT(lkpi_selftest, SI_SUB_LAST, SI_ORDER_ANY, lkpi_selftest, NULL);
