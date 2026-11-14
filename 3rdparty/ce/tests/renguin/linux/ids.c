/* Process, time and misc syscalls (Stage 4). Exits 0 and prints "ok", or
 * exits with the number of the first failing check. */
#include "sys.h"

struct ts { i64 sec, nsec; };

int check_main(int argc, char** argv)
{
	(void)argc; (void)argv;
	i64 pid = sys0(SYS_getpid);
	if (pid <= 0) return 2;
	if (sys0(SYS_gettid) != pid) return 3;	/* main thread id == pid */
	if (sys0(SYS_getppid) <= 0) return 4;
	if (sys0(SYS_getuid) < 0) return 5;

	u64 lim[2];
	if (sys4(SYS_prlimit64, 0, 7, 0, lim) != 0) return 6;	/* NOFILE */
	if (lim[0] == 0 || lim[0] > lim[1]) return 7;
	if (sys4(SYS_prlimit64, 0, 3, 0, lim) != 0) return 8;	/* STACK */

	struct ts a, b, nap = { 0, 20 * 1000 * 1000 };
	if (sys2(SYS_clock_gettime, 1, &a) != 0) return 9;	/* MONOTONIC */
	if (sys2(SYS_nanosleep, &nap, 0) != 0) return 10;
	if (sys2(SYS_clock_gettime, 1, &b) != 0) return 11;
	i64 ns = (b.sec - a.sec) * 1000000000 + (b.nsec - a.nsec);
	if (ns < 19 * 1000 * 1000) return 12;
	if (sys2(SYS_clock_gettime, 0, &a) != 0 || a.sec < 1600000000)
		return 13;	/* REALTIME is a plausible date */
	if (sys2(SYS_clock_gettime, 99, &a) != -22) return 14;	/* EINVAL */

	unsigned char r[64];
	for (int i = 0; i < 64; i++) r[i] = 0;
	if (sys3(SYS_getrandom, r, sizeof(r), 0) != sizeof(r)) return 15;
	int nonzero = 0;
	for (int i = 0; i < 64; i++) nonzero += r[i] != 0;
	if (nonzero < 32) return 16;

	i64 old = sys1(SYS_umask, 077);
	if (old < 0 || sys1(SYS_umask, old) != 077) return 17;

	/* Bad pointers fail with EFAULT instead of touching kernel memory. */
	if (sys1(SYS_uname, (i64)0xffff800000001000UL) != -14) return 18;
	if (sys2(SYS_clock_gettime, 1, (i64)0xffff800000001000UL) != -14)
		return 19;

	put("ok\n");
	return 0;
}
