/* Prints "hello" and exits with status 42, using only raw syscalls. */
	.globl _start
	.text
_start:
	mov	$1, %eax		/* write */
	mov	$1, %edi		/* stdout */
	lea	msg(%rip), %rsi
	mov	$6, %edx
	syscall
	mov	$231, %eax		/* exit_group */
	mov	$42, %edi
	syscall
	hlt
	.section .rodata
msg:	.ascii "hello\n"
