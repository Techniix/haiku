; Renga test harness: a BIOS boot record that stands in for another
; operating system's (roadmap F-22, 3rdparty/ce/README.md "Other operating
; systems"). Variants, chosen with -D:
;
;   KIND_NTFS   a partition boot record that looks like Windows': "NTFS"
;               OEM name and the word BOOTMGR, as in Windows' own
;   KIND_MBR    a master boot record (code only, 440 bytes; the harness adds
;               the partition table)
;   (neither)   a floppy boot sector
;
; Chainloaded by the Renga boot loader, it writes "CE-OTHEROS started
; <MARKER> dl=<drive> lba=<start of the partition DS:SI points to>" to the
; first serial port, so the harness can check the registers a boot record
; gets, and then boots BIOS drive 0x80 (Renga's disk in the tests) again
; the way an MBR does: it moves itself out of the way, reads that disk's
; MBR to 0x7c00 and jumps there. The harness then sees a second Renga boot.
;
; Assemble: nasm -f bin -DKIND_NTFS -DMARKER='"bios-windows"' -o r.bin \
;     boot_record.asm

bits 16
org 0x7c00

%ifndef MARKER
%define MARKER "bios-test"
%endif

RELOCATED	equ 0x0800
%define R(x) ((x) - start + RELOCATED)

start:
%ifdef KIND_NTFS
	jmp		short code
	nop
	db		"NTFS    "			; OEM name, as NTFS writes it
	times 0x54 - ($ - start) db 0	; BIOS parameter block area
%endif
code:
	cli
	mov		[saved_si], si		; DS is still the caller's: 0
	mov		[saved_dl], dl
	xor		ax, ax
	mov		ds, ax
	mov		es, ax
	mov		ss, ax
	mov		sp, 0x7c00
	sti
	cld

	mov		si, started
	call	print
	mov		si, dl_text
	call	print
	movzx	eax, byte [saved_dl]
	mov		cx, 2
	call	print_hex
	mov		si, lba_text
	call	print
	mov		bx, [saved_si]
	xor		eax, eax
	test	bx, bx
	jz		.no_entry
	mov		eax, [bx + 8]
.no_entry:
	mov		cx, 8
	call	print_hex
	mov		si, newline
	call	print

	; Boot drive 0x80 again: move out of 0x7c00 first.
	mov		si, start
	mov		di, RELOCATED
	mov		cx, 256
	rep movsw
	jmp		0:R(relocated)

relocated:
	; Code here runs at RELOCATED: data through R(), calls are relative
	; and land in the moved copy by themselves.
	mov		si, R(packet)
	mov		ah, 0x42
	mov		dl, 0x80
	int		0x13
	jc		.failed
	mov		dl, 0x80
	xor		si, si
	jmp		0:0x7c00
.failed:
	mov		si, R(failed_text)
	call	print
.halt:
	hlt
	jmp		.halt

; Writes AL to COM1, waiting for the transmitter. Keeps all registers.
putc:
	push	dx
	push	ax
	mov		dx, 0x3fd
.wait:
	in		al, dx
	test	al, 0x20
	jz		.wait
	pop		ax
	mov		dx, 0x3f8
	out		dx, al
	pop		dx
	ret

; Prints the zero-terminated string at DS:SI.
print:
	lodsb
	test	al, al
	jz		.done
	call	putc
	jmp		print
.done:
	ret

; Prints the low CX hex digits of EAX, most significant first.
print_hex:
	push	ebx
.next:
	mov		ebx, eax
	push	cx
	dec		cx
	shl		cx, 2
	shr		ebx, cl
	pop		cx
	and		bl, 0x0f
	add		bl, '0'
	cmp		bl, '9'
	jbe		.digit
	add		bl, 'a' - '0' - 10
.digit:
	push	ax
	mov		al, bl
	call	putc
	pop		ax
	loop	.next
	pop		ebx
	ret

started:	db 13, 10, "CE-OTHEROS started ", MARKER, 0
dl_text:	db " dl=", 0
lba_text:	db " lba=", 0
newline:	db 13, 10, 0
failed_text: db "CE-OTHEROS cannot read BIOS drive 0x80", 13, 10, 0
%ifdef KIND_NTFS
			db "BOOTMGR", 0		; what Windows' boot record names
%endif
saved_dl:	db 0
saved_si:	dw 0

	align 4
packet:
	db		16, 0
	dw		1					; one sector
	dw		0x7c00, 0			; to 0000:7c00
	dq		0					; LBA 0

%ifdef KIND_MBR
	times 440 - ($ - start) db 0
%else
	times 510 - ($ - start) db 0
	dw		0xaa55
%endif
