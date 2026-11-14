/* exit (60) from the only thread ends the process with status 7. */
	.globl _start
	.text
_start:
	mov	$60, %eax
	mov	$7, %edi
	syscall
	hlt
