/* An unknown syscall number must fail with -ENOSYS (38); exits with -rax. */
	.globl _start
	.text
_start:
	mov	$9999, %eax
	syscall
	neg	%rax
	mov	%eax, %edi
	mov	$231, %eax
	syscall
	hlt
