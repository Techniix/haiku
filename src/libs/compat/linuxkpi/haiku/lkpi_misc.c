/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Small FreeBSD kernel facilities LinuxKPI expects (lkpi_haiku_kpi.h,
 * lkpi_haiku_kern.h, sys/sysctl.h, sys/cpuset.h), on Haiku public kernel
 * API. Built in the "linuxkpi" profile: E* values are FreeBSD's here.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>
#include <sys/cpuset.h>
#include <machine/md_var.h>
#include <sys/socketvar.h>
#include <linux/compat.h>	/* fpu_safe_exec_cb_t */
#include <linux/sched.h>	/* put_task_struct */

#include <KernelExport.h>
#include <OS.h>

/* ---- hooks LinuxKPI installs into the FreeBSD kernel ------------------- */

int
linux_alloc_current_noop(struct thread *td, int flags)
{
	return (0);
}
int (*lkpi_alloc_current)(struct thread *, int) = linux_alloc_current_noop;

struct sx allproc_lock;

static void
lkpi_allproc_init(void *arg)
{
	sx_init(&allproc_lock, "lkpi allproc");
}
SYSINIT(lkpi_allproc, SI_SUB_LOCK, SI_ORDER_FIRST, lkpi_allproc_init, NULL);

/* ---- CPUs ------------------------------------------------------------------ */

cpuset_t all_cpus;
cpuset_t *cpuset_root = &all_cpus;
unsigned int cpu_feature, cpu_feature2, cpu_stdext_feature;
long physmem;

static void
lkpi_misc_init(void *arg)
{
	cpuid_info info;
	system_info sysinfo;
	int i;

	CPU_ZERO(&all_cpus);
	CPU_FOREACH(i)
		CPU_SET(i, &all_cpus);

	if (get_cpuid(&info, 1, 0) == B_OK) {
		cpu_feature = info.regs.edx;
		cpu_feature2 = info.regs.ecx;
	}
	if (get_cpuid(&info, 0, 0) == B_OK && info.regs.eax >= 7 &&
	    get_cpuid(&info, 7, 0) == B_OK)
		cpu_stdext_feature = info.regs.ebx;

	if (get_system_info(&sysinfo) == B_OK)
		physmem = sysinfo.max_pages;
}
SYSINIT(lkpi_misc, SI_SUB_CPU, SI_ORDER_FIRST, lkpi_misc_init, NULL);

int
intr_setaffinity(int irq, int mode, const void *mask)
{
	return (0);	/* Haiku: no per-IRQ affinity API for drivers; ignore */
}

/* ---- sysctl: registration is a no-op on Haiku (see sys/sysctl.h) -------- */

static struct sysctl_oid lkpi_dummy_oid;

struct sysctl_oid *
lkpi_sysctl_dummy_oid(struct sysctl_ctx_list *ctx,
    struct sysctl_oid_list *parent, const char *name, void *arg1, intmax_t arg2)
{
	return (&lkpi_dummy_oid);
}

int sysctl_ctx_init(struct sysctl_ctx_list *c) { return (0); }
int sysctl_ctx_free(struct sysctl_ctx_list *c) { return (0); }
int sysctl_remove_oid(struct sysctl_oid *o, int d, int r) { return (0); }
int sysctl_remove_name(struct sysctl_oid *p, const char *n, int d, int r) { return (0); }
int sysctl_handle_int(SYSCTL_HANDLER_ARGS) { return (0); }
int sysctl_handle_long(SYSCTL_HANDLER_ARGS) { return (0); }
int sysctl_handle_string(SYSCTL_HANDLER_ARGS) { return (0); }
int sysctl_handle_opaque(SYSCTL_HANDLER_ARGS) { return (0); }

/* ---- user memory --------------------------------------------------------- */

int
copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done)
{
	ssize_t n = user_strlcpy(kaddr, uaddr, len);

	if (n < 0)
		return (EFAULT);
	if ((size_t)n >= len)
		return (ENAMETOOLONG);
	if (done != NULL)
		*done = n + 1;
	return (0);
}

int
copyin_nofault(const void *uaddr, void *kaddr, size_t len)
{
	return (user_memcpy(kaddr, uaddr, len) == B_OK ? 0 : EFAULT);
}

int
copyout_nofault(const void *kaddr, void *uaddr, size_t len)
{
	return (user_memcpy(uaddr, kaddr, len) == B_OK ? 0 : EFAULT);
}

int vm_fault_disable_pagefaults(void) { return (0); }
void vm_fault_enable_pagefaults(int save) { }

/* ---- memory odds and ends ------------------------------------------------ */

void
explicit_bzero(void *b, size_t len)
{
	volatile unsigned char *p = b;

	while (len-- > 0)
		*p++ = 0;
}

size_t
malloc_usable_size(const void *p)
{
	/* Haiku's kernel heap does not export allocation sizes. LinuxKPI only
	 * needs this for ksize() and zfree(); see docs/STATUS.md. */
	return (0);
}

void
vm_wait(void *obj)
{
	snooze(1000);
}

void
pmap_invalidate_cache(void)
{
	__asm__ __volatile__("wbinvd" ::: "memory");
}

void
pmap_force_invalidate_cache_range(vm_offset_t sva, vm_offset_t eva)
{
	for (sva &= ~(vm_offset_t)63; sva < eva; sva += 64)
		__asm__ __volatile__("clflush (%0)" :: "r"(sva) : "memory");
	__asm__ __volatile__("mfence" ::: "memory");
}

/* ---- kern_mib.c / user stores / rendezvous / sockets -------------------- */

char osrelease[OSRELEASELEN] = "Haiku";

int subyte(volatile void *b, int v) { uint8_t x = v; return (user_memcpy((void *)b, &x, 1) == B_OK ? 0 : -1); }
int suword16(volatile void *b, int v) { uint16_t x = v; return (user_memcpy((void *)b, &x, 2) == B_OK ? 0 : -1); }
int suword32(volatile void *b, int32_t v) { return (user_memcpy((void *)b, &v, 4) == B_OK ? 0 : -1); }
int suword64(volatile void *b, int64_t v) { return (user_memcpy((void *)b, &v, 8) == B_OK ? 0 : -1); }
int suword(volatile void *b, long v) { return (user_memcpy((void *)b, &v, sizeof(v)) == B_OK ? 0 : -1); }

struct lkpi_rendezvous {
	void	(*action)(void *);
	void	*arg;
};

static void
lkpi_rendezvous_cb(void *cookie, int cpu)
{
	struct lkpi_rendezvous *r = cookie;

	r->action(r->arg);
}

void
smp_rendezvous(void (*setup)(void *), void (*action)(void *),
    void (*teardown)(void *), void *arg)
{
	struct lkpi_rendezvous r = { action, arg };

	/* Haiku runs func on every CPU and waits; setup/teardown are only
	 * used with barrier semantics LinuxKPI does not request. */
	if (setup != NULL && setup != smp_no_rendezvous_barrier)
		setup(arg);
	call_all_cpus_sync(lkpi_rendezvous_cb, &r);
	if (teardown != NULL && teardown != smp_no_rendezvous_barrier)
		teardown(arg);
}

int
socreate(int dom, struct socket **aso, int type, int proto,
    struct ucred *cred, struct thread *td)
{
	return (EOPNOTSUPP);
}

int
soclose(struct socket *so)
{
	return (0);
}

/* ---- FreeBSD kernel strdup/realloc (include/sys/malloc.h) ---------------- */

#undef strdup
#undef realloc

char *
lkpi_kern_strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *d = malloc(len, M_DEVBUF, M_WAITOK);

	if (d != NULL)
		memcpy(d, s, len);
	return (d);
}

void *
_kernel_realloc_lkpi(void *p, size_t size, int flags)
{
	/* freebsd_network's _kernel_malloc() is the kernel heap's malloc()
	 * (malloc.cpp), so the heap's realloc() is the matching resize. */
	return (realloc(p, size));
}

/* ---- NUMA: Haiku exposes no domains --------------------------------------- */

struct domainset *
linux_get_vm_domain_set(int node)
{
	return (NULL);
}

/* ---- DPCPU (include/lkpi_haiku_kern.h) ------------------------------------ */

uintptr_t lkpi_dpcpu_off[LKPI_MAX_CPUS];
DPCPU_DEFINE_STATIC(int, lkpi_dpcpu_dummy);	/* keeps the section non-empty */
extern char __start_set_lkpi_dpcpu[], __stop_set_lkpi_dpcpu[];

void
lkpi_dpcpu_init(void)
{
	size_t size = __stop_set_lkpi_dpcpu - __start_set_lkpi_dpcpu;
	int i, ncpu = smp_get_num_cpus();
	char *copy;

	(void)DPCPU_GET(lkpi_dpcpu_dummy);
	for (i = 1; i < ncpu && i < LKPI_MAX_CPUS; i++) {
		copy = malloc(size, M_DEVBUF, M_WAITOK);
		memcpy(copy, __start_set_lkpi_dpcpu, size);
		lkpi_dpcpu_off[i] = (uintptr_t)copy -
		    (uintptr_t)__start_set_lkpi_dpcpu;
	}
}

/* ---- jiffies, qsort_r, FPU, MSI counters (lkpi_haiku_kern.h) --------------- */

#undef jiffies
unsigned long
lkpi_get_jiffies(void)
{
	/* hz is 1000 in freebsd_network: one jiffy per millisecond */
	return ((unsigned long)(system_time() / (1000000 / hz)));
}

/* Used by linux_current.c (DEV_APIC branch) to size its reserve. */
u_int first_msi_irq = 0;
u_int num_msi_irqs = 0;

void
lkpi_fpu_safe_exec(fpu_safe_exec_cb_t func, void *ctx)
{
	/* Code reaching here is compiled with Haiku's kernel flags, so it is
	 * as FPU-safe as any other Haiku kernel code. Only DRM drivers use
	 * kernel_fpu_begin() for real FPU work; revisit for DRM. */
	func(ctx);
}

/* Merge sort (stable, like FreeBSD's) with a scratch buffer; falls back to
 * insertion sort if the buffer cannot be allocated. */
static void
lkpi_msort(char *base, char *tmp, size_t n, size_t size,
    int (*cmp)(const void *, const void *, void *), void *thunk)
{
	size_t h, i, j, k;

	if (n < 2)
		return;
	h = n / 2;
	lkpi_msort(base, tmp, h, size, cmp, thunk);
	lkpi_msort(base + h * size, tmp, n - h, size, cmp, thunk);
	for (i = 0, j = h, k = 0; i < h || j < n; k++) {
		if (j >= n || (i < h && cmp(base + i * size, base + j * size,
		    thunk) <= 0))
			memcpy(tmp + k * size, base + i++ * size, size);
		else
			memcpy(tmp + k * size, base + j++ * size, size);
	}
	memcpy(base, tmp, n * size);
}

void
lkpi_qsort_r(void *base, size_t nmemb, size_t size,
    int (*cmp)(const void *, const void *, void *), void *thunk)
{
	char *tmp, *b = base, swap[64];
	size_t i, j;

	tmp = malloc(nmemb * size, M_DEVBUF, M_NOWAIT);
	if (tmp != NULL) {
		lkpi_msort(b, tmp, nmemb, size, cmp, thunk);
		free(tmp, M_DEVBUF);
		return;
	}
	if (size > sizeof(swap))
		panic("lkpi_qsort_r: element too large");
	for (i = 1; i < nmemb; i++) {
		for (j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size,
		    thunk) > 0; j--) {
			memcpy(swap, b + j * size, size);
			memcpy(b + j * size, b + (j - 1) * size, size);
			memcpy(b + (j - 1) * size, swap, size);
		}
	}
}

/* sysctl is not registered on Haiku, so handlers never run; keep it valid. */
struct sbuf *
sbuf_new_for_sysctl(struct sbuf *s, char *buf, int length,
    struct sysctl_req *req)
{
	return (sbuf_new(s, buf, length, SBUF_FIXEDLEN));
}

/* hexdump(9): 16 bytes per line, offset, hex and characters to syslog. */
void
hexdump(const void *ptr, int length, const char *hdr, int flags)
{
	const unsigned char *cp = ptr;
	char line[128];
	int cols, i, j, n;

	cols = flags & HD_COLUMN_MASK;
	if (cols == 0)
		cols = 16;
	if (cols > 32)
		cols = 32;
	for (i = 0; i < length; i += cols) {
		n = 0;
		if ((flags & HD_OMIT_COUNT) == 0)
			n += snprintf(line + n, sizeof(line) - n, "%04x  ", i);
		if ((flags & HD_OMIT_HEX) == 0) {
			for (j = 0; j < cols; j++) {
				if (i + j < length)
					n += snprintf(line + n, sizeof(line) - n,
					    "%02x ", cp[i + j]);
				else
					n += snprintf(line + n, sizeof(line) - n,
					    "   ");
			}
		}
		if ((flags & HD_OMIT_CHARS) == 0) {
			n += snprintf(line + n, sizeof(line) - n, " |");
			for (j = 0; j < cols && i + j < length; j++)
				line[n++] = (cp[i + j] >= ' ' && cp[i + j] <= '~') ?
				    cp[i + j] : '.';
			line[n++] = '|';
			line[n] = '\0';
		}
		printf("%s%s\n", hdr != NULL ? hdr : "", line);
	}
}


/*
 * FreeBSD's linuxkpi_thread_dtor() (linux_current.c), which Haiku never
 * calls: drop the exiting thread's reference to its Linux task_struct.
 * Called by the thread reaper in lkpi_thread.cpp from the exiting thread.
 * A kthread stopped with kthread_stop() has already handed its task over
 * (linux_kthread_fn() clears td_lkpi_task), as on FreeBSD.
 */
void
lkpi_thread_release_task(struct thread *td)
{
	struct task_struct *ts;

	ts = td->td_lkpi_task;
	if (ts == NULL)
		return;
	td->td_lkpi_task = NULL;
	put_task_struct(ts);
}
