/* write(99, ...) must fail with -EBADF (9); exits with -rax. */
	.globl _start
	.text
_start:
	mov	$1, %eax
	mov	$99, %edi
	lea	_start(%rip), %rsi
	mov	$1, %edx
	syscall
	neg	%rax
	mov	%eax, %edi
	mov	$231, %eax
	syscall
	hlt
