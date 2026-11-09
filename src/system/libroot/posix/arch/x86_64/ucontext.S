/*
 * Copyright 2026, Haiku Community Edition contributors.
 * Distributed under the terms of the MIT License.
 */

/*	getcontext(), setcontext(), swapcontext() for x86_64 (System V ABI).

	A context holds what a function call must preserve: rbx, rbp, r12-r15,
	the stack pointer, the instruction pointer, the x87 control word and
	MXCSR (the ABI has no callee-saved vector registers), plus the signal
	mask. rdi, rsi, rdx, rcx, r8 and r9 are restored too, so that a context
	made by makecontext() starts with its arguments in place; for a context
	saved by getcontext()/swapcontext() they hold nothing of interest. A
	saved context resumes as a return from the saving call with value 0.

	The signal mask goes through sigprocmask(), so each switch is one
	syscall, as with glibc.
*/


#include <asm_defs.h>

#include "ucontext_internal.h"


/*	Saves the caller's context into the ucontext_t in %rdi.
	Does not touch the signal mask. Clobbers %rax only.
*/
.macro SAVE_CONTEXT
	movq	(%rsp), %rax
	movq	%rax, UC_RIP(%rdi)
	leaq	8(%rsp), %rax
	movq	%rax, UC_RSP(%rdi)

	movq	%rbx, UC_RBX(%rdi)
	movq	%rbp, UC_RBP(%rdi)
	movq	%r12, UC_R12(%rdi)
	movq	%r13, UC_R13(%rdi)
	movq	%r14, UC_R14(%rdi)
	movq	%r15, UC_R15(%rdi)

	fnstcw	UC_FPU_CONTROL(%rdi)
	stmxcsr	UC_FPU_MXCSR(%rdi)

	// When this context is resumed, the saving call returns 0.
	movq	$0, UC_RAX(%rdi)
.endm


/* int getcontext(ucontext_t* context) */
FUNCTION(getcontext):
	SAVE_CONTEXT

	// sigprocmask(SIG_BLOCK, NULL, &context->uc_sigmask) reads the mask.
	pushq	%rdi
	leaq	UC_SIGMASK(%rdi), %rdx
	xorl	%esi, %esi
	movl	$1, %edi				// SIG_BLOCK with no set: just read
	call	sigprocmask@PLT
	popq	%rdi
	ret								// sigprocmask's 0 or -1/errno
FUNCTION_END(getcontext)


/* int swapcontext(ucontext_t* oldContext, const ucontext_t* newContext) */
FUNCTION(swapcontext):
	SAVE_CONTEXT

	// sigprocmask(SIG_SETMASK, &new->uc_sigmask, &old->uc_sigmask): save
	// the current mask and install the new one in one call.
	pushq	%rsi
	leaq	UC_SIGMASK(%rdi), %rdx
	leaq	UC_SIGMASK(%rsi), %rsi
	movl	$UC_SIG_SETMASK, %edi
	call	sigprocmask@PLT
	popq	%rdi						// the new context
	testl	%eax, %eax
	jnz		1f
	jmp		.Lrestore_registers
1:	ret
FUNCTION_END(swapcontext)


/* int setcontext(const ucontext_t* context) -- does not return on success */
FUNCTION(setcontext):
	// sigprocmask(SIG_SETMASK, &context->uc_sigmask, NULL)
	pushq	%rdi
	leaq	UC_SIGMASK(%rdi), %rsi
	xorl	%edx, %edx
	movl	$UC_SIG_SETMASK, %edi
	call	sigprocmask@PLT
	popq	%rdi
	testl	%eax, %eax
	jz		.Lrestore_registers
	ret

.Lrestore_registers:
	// %rdi points to the context to load.
	fldcw	UC_FPU_CONTROL(%rdi)
	ldmxcsr	UC_FPU_MXCSR(%rdi)

	movq	UC_RBX(%rdi), %rbx
	movq	UC_RBP(%rdi), %rbp
	movq	UC_R12(%rdi), %r12
	movq	UC_R13(%rdi), %r13
	movq	UC_R14(%rdi), %r14
	movq	UC_R15(%rdi), %r15

	// Switch stacks, push the resume address, load the argument registers
	// (for makecontext()) and the return value, and "return" into it.
	movq	UC_RSP(%rdi), %rsp
	pushq	UC_RIP(%rdi)

	movq	UC_RSI(%rdi), %rsi
	movq	UC_RDX(%rdi), %rdx
	movq	UC_RCX(%rdi), %rcx
	movq	UC_R8(%rdi), %r8
	movq	UC_R9(%rdi), %r9
	movq	UC_RAX(%rdi), %rax
	movq	UC_RDI(%rdi), %rdi
	ret
FUNCTION_END(setcontext)


/*	Where a makecontext() function returns to. makecontext() put uc_link in
	%rbx (callee-saved, so the function kept it): resume that context, or
	exit the thread's team if there is none, as POSIX specifies.
*/
FUNCTION(__makecontext_trampoline):
	movq	%rbx, %rdi
	testq	%rdi, %rdi
	jz		1f
	call	setcontext@PLT
	// setcontext() only returns on failure; fall through and exit.
1:	xorl	%edi, %edi
	call	exit@PLT
	hlt
FUNCTION_END(__makecontext_trampoline)
