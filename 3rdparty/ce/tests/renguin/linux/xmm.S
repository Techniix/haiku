/* Vector registers must survive a syscall. Exits 0 if xmm0..xmm15 are intact, 1 if not. */
	.globl _start
	.text
_start:
	movdqa	pattern(%rip), %xmm0
	movdqa	%xmm0, %xmm1;  movdqa %xmm0, %xmm2;  movdqa %xmm0, %xmm3
	movdqa	%xmm0, %xmm4;  movdqa %xmm0, %xmm5;  movdqa %xmm0, %xmm6
	movdqa	%xmm0, %xmm7;  movdqa %xmm0, %xmm8;  movdqa %xmm0, %xmm9
	movdqa	%xmm0, %xmm10; movdqa %xmm0, %xmm11; movdqa %xmm0, %xmm12
	movdqa	%xmm0, %xmm13; movdqa %xmm0, %xmm14; movdqa %xmm0, %xmm15
	mov	$1, %eax		/* write(1, "x\n", 2) */
	mov	$1, %edi
	lea	msg(%rip), %rsi
	mov	$2, %edx
	syscall
	xor	%edi, %edi
	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	pcmpeqb	pattern(%rip), %xmm\r
	pmovmskb %xmm\r, %eax
	cmp	$0xffff, %eax
	jne	bad
	.endr
	jmp	done
bad:	mov	$1, %edi
done:	mov	$231, %eax
	syscall
	hlt
	.section .rodata
	.balign 16
pattern: .quad 0x0123456789abcdef, 0xfedcba9876543210
msg:	.ascii "x\n"
