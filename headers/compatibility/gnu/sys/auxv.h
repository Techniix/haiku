/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux getauxval(). Haiku does not pass an auxiliary vector to programs;
 * libgnu derives the values software actually asks for from the process
 * and its program image. Unknown types return 0 with errno ENOENT, as in
 * glibc. Link with -lgnu.
 */
#ifndef _GNU_SYS_AUXV_H
#define _GNU_SYS_AUXV_H


#include <sys/cdefs.h>


#define AT_NULL				0
#define AT_PHDR				3	/* program headers of the program */
#define AT_PHENT			4	/* size of one program header */
#define AT_PHNUM			5	/* number of program headers */
#define AT_PAGESZ			6	/* page size */
#define AT_BASE				7	/* base address of the runtime loader */
#define AT_FLAGS			8
#define AT_ENTRY			9	/* entry point of the program */
#define AT_UID				11
#define AT_EUID				12
#define AT_GID				13
#define AT_EGID				14
#define AT_PLATFORM			15	/* string: the CPU architecture */
#define AT_HWCAP			16	/* CPU feature bits (x86: CPUID 1 EDX) */
#define AT_CLKTCK			17
#define AT_SECURE			23	/* set-user/group-ID program */
#define AT_RANDOM			25	/* address of 16 random bytes */
#define AT_HWCAP2			26
#define AT_EXECFN			31	/* string: path of the program */
#define AT_SYSINFO_EHDR		33	/* not supported (no vDSO) */
#define AT_MINSIGSTKSZ		51


__BEGIN_DECLS

unsigned long	getauxval(unsigned long type);

__END_DECLS


#endif	/* _GNU_SYS_AUXV_H */
