/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay for FreeBSD <machine/md_var.h>: the CPUID feature words that
 * LinuxKPI's asm/cpufeature.h tests. Filled from Haiku's cpu_ent at init
 * (lkpi_cpu.c). Bit values are the architectural CPUID bits, as in FreeBSD's
 * x86/include/specialreg.h.
 */
#ifndef _LKPI_HAIKU_MACHINE_MD_VAR_H_
#define _LKPI_HAIKU_MACHINE_MD_VAR_H_

#include_next <machine/md_var.h>

extern unsigned int cpu_feature;	/* CPUID.1:EDX */
extern unsigned int cpu_feature2;	/* CPUID.1:ECX */
extern unsigned int cpu_stdext_feature;	/* CPUID.(7,0):EBX */

#define CPUID_PAT	0x00010000	/* EDX bit 16 */
#define CPUID_CLFSH	0x00080000	/* EDX bit 19 */
#define CPUID2_SSE41	0x00080000	/* ECX bit 19 */
#define CPUID2_HV	0x80000000	/* ECX bit 31 */
#define CPUID_STDEXT_CLFLUSHOPT	0x00800000	/* EBX bit 23 */

#endif
