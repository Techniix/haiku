/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 *
 * Offsets into ucontext_t (headers/posix/signal.h, arch/x86_64/signal.h)
 * for ucontext.S. makecontext.c checks every one of them against the real
 * structure with static assertions, so a layout change breaks the build
 * instead of corrupting registers.
 */
#ifndef _LIBROOT_X86_64_UCONTEXT_INTERNAL_H
#define _LIBROOT_X86_64_UCONTEXT_INTERNAL_H


#define UC_LINK			0
#define UC_SIGMASK		8
#define UC_MCONTEXT		40

#define UC_RAX			(UC_MCONTEXT + 0)
#define UC_RBX			(UC_MCONTEXT + 8)
#define UC_RCX			(UC_MCONTEXT + 16)
#define UC_RDX			(UC_MCONTEXT + 24)
#define UC_RDI			(UC_MCONTEXT + 32)
#define UC_RSI			(UC_MCONTEXT + 40)
#define UC_RBP			(UC_MCONTEXT + 48)
#define UC_R8			(UC_MCONTEXT + 56)
#define UC_R9			(UC_MCONTEXT + 64)
#define UC_R12			(UC_MCONTEXT + 88)
#define UC_R13			(UC_MCONTEXT + 96)
#define UC_R14			(UC_MCONTEXT + 104)
#define UC_R15			(UC_MCONTEXT + 112)
#define UC_RSP			(UC_MCONTEXT + 120)
#define UC_RIP			(UC_MCONTEXT + 128)
#define UC_FPU			(UC_MCONTEXT + 144)
#define UC_FPU_CONTROL	(UC_FPU + 0)	/* x87 control word */
#define UC_FPU_MXCSR	(UC_FPU + 24)	/* SSE control/status */

/* sigprocmask() "how" values (signal.h) */
#define UC_SIG_SETMASK	3


#endif	/* _LIBROOT_X86_64_UCONTEXT_INTERNAL_H */
