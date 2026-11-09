/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	makecontext() for x86_64, and the layout checks for ucontext.S.

	The new context starts "function" on the stack given in uc_stack, with
	up to six arguments in registers and the rest on the stack, as the
	System V ABI passes them. Its return address is
	__makecontext_trampoline (ucontext.S), which resumes uc_link, or exits
	if uc_link is NULL; uc_link travels in %rbx, a register the function
	must preserve.

	Like glibc, arguments are read as 64-bit values, so passing pointers
	works (POSIX only promises int arguments).
*/


#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include "ucontext_internal.h"


#define CHECK_OFFSET(field, offset) \
	_Static_assert(offsetof(ucontext_t, field) == (offset), \
		"ucontext.S offset of " #field " is wrong")

CHECK_OFFSET(uc_link, UC_LINK);
CHECK_OFFSET(uc_sigmask, UC_SIGMASK);
CHECK_OFFSET(uc_mcontext, UC_MCONTEXT);
CHECK_OFFSET(uc_mcontext.rax, UC_RAX);
CHECK_OFFSET(uc_mcontext.rbx, UC_RBX);
CHECK_OFFSET(uc_mcontext.rcx, UC_RCX);
CHECK_OFFSET(uc_mcontext.rdx, UC_RDX);
CHECK_OFFSET(uc_mcontext.rdi, UC_RDI);
CHECK_OFFSET(uc_mcontext.rsi, UC_RSI);
CHECK_OFFSET(uc_mcontext.rbp, UC_RBP);
CHECK_OFFSET(uc_mcontext.r8, UC_R8);
CHECK_OFFSET(uc_mcontext.r9, UC_R9);
CHECK_OFFSET(uc_mcontext.r12, UC_R12);
CHECK_OFFSET(uc_mcontext.r13, UC_R13);
CHECK_OFFSET(uc_mcontext.r14, UC_R14);
CHECK_OFFSET(uc_mcontext.r15, UC_R15);
CHECK_OFFSET(uc_mcontext.rsp, UC_RSP);
CHECK_OFFSET(uc_mcontext.rip, UC_RIP);
CHECK_OFFSET(uc_mcontext.fpu, UC_FPU);
CHECK_OFFSET(uc_mcontext.fpu.fp_fxsave.control, UC_FPU_CONTROL);
CHECK_OFFSET(uc_mcontext.fpu.fp_fxsave.mxcsr, UC_FPU_MXCSR);
_Static_assert(SIG_SETMASK == UC_SIG_SETMASK, "SIG_SETMASK changed");


extern void __makecontext_trampoline(void);


void
makecontext(ucontext_t* context, void (*function)(void), int argumentCount,
	...)
{
	unsigned long arguments[6] = { 0 };
	int stackArguments = argumentCount > 6 ? argumentCount - 6 : 0;

	// Top of the new stack, 16-byte aligned, with room for the stack
	// arguments and the return address below it. At the function's first
	// instruction %rsp + 8 must be 16-byte aligned (the ABI's rule after a
	// call), with the return address at (%rsp).
	uintptr_t top = (uintptr_t)context->uc_stack.ss_sp
		+ context->uc_stack.ss_size;
	top &= ~(uintptr_t)15;
	unsigned long* stack = (unsigned long*)top - stackArguments;
	if ((((uintptr_t)stack) & 15) != 0)
		stack--;
	stack--;
	stack[0] = (unsigned long)&__makecontext_trampoline;

	va_list list;
	va_start(list, argumentCount);
	for (int i = 0; i < argumentCount; i++) {
		unsigned long value = va_arg(list, unsigned long);
		if (i < 6)
			arguments[i] = value;
		else
			stack[1 + i - 6] = value;
	}
	va_end(list);

	mcontext_t* registers = &context->uc_mcontext;
	registers->rdi = arguments[0];
	registers->rsi = arguments[1];
	registers->rdx = arguments[2];
	registers->rcx = arguments[3];
	registers->r8 = arguments[4];
	registers->r9 = arguments[5];
	registers->rax = 0;
	registers->rsp = (unsigned long)stack;
	registers->rip = (unsigned long)function;
	registers->rbx = (unsigned long)context->uc_link;
	registers->rbp = 0;		// ends frame-pointer stack walks here
}
