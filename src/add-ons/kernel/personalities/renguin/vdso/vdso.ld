/*
 * Version script of Renguin's vDSO: the names and the version glibc and
 * musl look up (LINUX_2.6, as Linux's arch/x86/entry/vdso/vdso.lds.S).
 */
LINUX_2.6 {
	global:
		clock_gettime;
		__vdso_clock_gettime;
		gettimeofday;
		__vdso_gettimeofday;
		time;
		__vdso_time;
		getcpu;
		__vdso_getcpu;
	local: *;
};
