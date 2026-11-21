/*
 * Copyright 2026, Renguin contributors.
 * Distributed under the terms of the MIT License.
 *
 * The Linux vDSO of Renguin: clock_gettime(), gettimeofday(), time() and
 * getcpu() without entering the kernel, as Linux's arch/x86/entry/vdso.
 * glibc and musl find these functions through AT_SYSINFO_EHDR and call them
 * instead of the system calls (version LINUX_2.6, vdso.ld).
 *
 * Built freestanding as a position-independent shared object with no
 * relocations (every symbol it uses is hidden and reached RIP-relative),
 * then embedded in the renguin module (VDSO.h, data_to_source). At exec the
 * module copies it into an area of the new Linux process and fills in
 * sRenguinVdsoData (found by its magic) with the address of the team's
 * commpage.
 *
 * Time comes from the same place native programs get it from (libroot's
 * system_time() and real_time_clock_usecs()): the TSC times the conversion
 * factor in the commpage's real_time_data, plus the real-time offset there,
 * which the kernel updates when the clock is set. The TSC_AUX register
 * holds the CPU number (arch_cpu.cpp), which RDPID or RDTSCP read for
 * getcpu(). What the vDSO cannot answer (CPU-time clocks, a time zone
 * request, no RDPID/RDTSCP) goes to the system call, as Linux's does.
 */


// freestanding: no headers (the cross compiler builds it without a sysroot)
typedef __INT64_TYPE__		int64_t;
typedef __UINT64_TYPE__		uint64_t;
typedef __UINT32_TYPE__		uint32_t;


#define LX_CLOCK_REALTIME			0
#define LX_CLOCK_MONOTONIC			1
#define LX_CLOCK_MONOTONIC_RAW		4
#define LX_CLOCK_REALTIME_COARSE	5
#define LX_CLOCK_MONOTONIC_COARSE	6
#define LX_CLOCK_BOOTTIME			7

#define LX_NR_gettimeofday			96
#define LX_NR_time					201
#define LX_NR_clock_gettime			228
#define LX_NR_getcpu				309

#define COMMPAGE_ENTRY_REAL_TIME_DATA	2

#define VDSO_FEATURE_RDTSCP			0x1
#define VDSO_FEATURE_RDPID			0x2


struct linux_timespec {
	int64_t	tv_sec;
	int64_t	tv_nsec;
};

struct linux_timeval {
	int64_t	tv_sec;
	int64_t	tv_usec;
};

// Haiku's struct real_time_data (arch_real_time_data.h, x86_64)
struct real_time_data {
	int64_t		system_time_offset;
	uint32_t	system_time_conversion_factor;
};

// Filled in by the module at exec (vdso.cpp, struct renguin_vdso_data).
struct vdso_data {
	char		magic[16];
	uint64_t	commpage;		// the team's commpage address
	uint32_t	features;		// VDSO_FEATURE_*
	uint32_t	reserved;
};

__attribute__((visibility("hidden"), used, aligned(64)))
volatile struct vdso_data sRenguinVdsoData = {
	"RenguinVdsoData!", 0, 0, 0
};


static inline long
syscall2(long number, long a1, long a2)
{
	long result;
	__asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a1), "S"(a2)
		: "rcx", "r11", "memory");
	return result;
}


static inline long
syscall3(long number, long a1, long a2, long a3)
{
	long result;
	__asm__ volatile("syscall" : "=a"(result)
		: "a"(number), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
	return result;
}


static inline volatile struct real_time_data*
real_time_data(void)
{
	uint64_t commpage = sRenguinVdsoData.commpage;
	if (commpage == 0)
		return 0;
	uint64_t offset = ((volatile uint64_t*)commpage)
		[COMMPAGE_ENTRY_REAL_TIME_DATA];
	return (volatile struct real_time_data*)(commpage + offset);
}


static inline uint64_t
read_tsc(void)
{
	uint32_t low, high;
	if ((sRenguinVdsoData.features & VDSO_FEATURE_RDTSCP) != 0) {
		uint32_t aux;
		__asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(aux));
	} else
		__asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}


//! Haiku's system_time_nsecs() (libroot, arch/x86_64/system_time.cpp).
static inline int64_t
system_time_nsecs(volatile struct real_time_data* data)
{
	uint64_t factor = (uint64_t)data->system_time_conversion_factor * 1000;
	unsigned __int128 product = (unsigned __int128)read_tsc() * factor;
	return (int64_t)(product >> 32);
}


static inline int
read_clock(int clock, int64_t* _nsecs)
{
	volatile struct real_time_data* data = real_time_data();
	if (data == 0 || data->system_time_conversion_factor == 0)
		return -1;
	switch (clock) {
		case LX_CLOCK_REALTIME:
		case LX_CLOCK_REALTIME_COARSE:
			*_nsecs = system_time_nsecs(data)
				+ data->system_time_offset * 1000;
			return 0;
		case LX_CLOCK_MONOTONIC:
		case LX_CLOCK_MONOTONIC_RAW:
		case LX_CLOCK_MONOTONIC_COARSE:
		case LX_CLOCK_BOOTTIME:
			*_nsecs = system_time_nsecs(data);
			return 0;
	}
	return -1;
}


__attribute__((visibility("default")))
int
__vdso_clock_gettime(int clock, struct linux_timespec* ts)
{
	int64_t nsecs;
	if (read_clock(clock, &nsecs) != 0)
		return (int)syscall2(LX_NR_clock_gettime, clock, (long)ts);
	ts->tv_sec = nsecs / 1000000000;
	ts->tv_nsec = nsecs % 1000000000;
	return 0;
}


__attribute__((visibility("default")))
int
__vdso_gettimeofday(struct linux_timeval* tv, void* tz)
{
	int64_t nsecs;
	if (tz != 0 || read_clock(LX_CLOCK_REALTIME, &nsecs) != 0)
		return (int)syscall2(LX_NR_gettimeofday, (long)tv, (long)tz);
	if (tv != 0) {
		tv->tv_sec = nsecs / 1000000000;
		tv->tv_usec = (nsecs % 1000000000) / 1000;
	}
	return 0;
}


__attribute__((visibility("default")))
int64_t
__vdso_time(int64_t* t)
{
	int64_t nsecs;
	if (read_clock(LX_CLOCK_REALTIME, &nsecs) != 0)
		return syscall2(LX_NR_time, (long)t, 0);
	int64_t seconds = nsecs / 1000000000;
	if (t != 0)
		*t = seconds;
	return seconds;
}


__attribute__((visibility("default")))
int
__vdso_getcpu(unsigned* cpu, unsigned* node, void* cache)
{
	uint32_t features = sRenguinVdsoData.features;
	uint64_t aux;
	if ((features & VDSO_FEATURE_RDPID) != 0)
		__asm__ volatile("rdpid %0" : "=r"(aux));
	else if ((features & VDSO_FEATURE_RDTSCP) != 0) {
		uint32_t low, high, value;
		__asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(value));
		aux = value;
	} else
		return (int)syscall3(LX_NR_getcpu, (long)cpu, (long)node, (long)cache);
	if (cpu != 0)
		*cpu = (unsigned)(aux & 0xfff);
	if (node != 0)
		*node = 0;
	return 0;
}


// The names without the prefix, as Linux's vDSO also exports them.
int clock_gettime(int, struct linux_timespec*)
	__attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct linux_timeval*, void*)
	__attribute__((weak, alias("__vdso_gettimeofday")));
int64_t time(int64_t*) __attribute__((weak, alias("__vdso_time")));
int getcpu(unsigned*, unsigned*, void*)
	__attribute__((weak, alias("__vdso_getcpu")));
