; Renga test harness: a minimal UEFI application (PE32+, x86-64) that
; stands in for another operating system's loader (Windows Boot Manager,
; shim, a removable medium's BOOTX64.EFI) in the boot menu tests (roadmap
; F-22, 3rdparty/ce/README.md "Other operating systems").
;
; Started by the Renga boot loader with LoadImage()/StartImage(), it
;   - writes "CE-OTHEROS started <MARKER>" to the first serial port
;     directly (port 0x3f8), so the harness sees it whatever the firmware
;     does with its console,
;   - says whether its Loaded Image protocol names the device it was loaded
;     from ("device handle: yes"): a real loader (Windows Boot Manager reads
;     its BCD store from there) needs it, and it is only set if the boot
;     loader passed LoadImage() a full device path,
;   - prints the marker on the firmware console (ConOut),
;   - and returns EFI_SUCCESS, so the boot menu has to cope with a loader
;     that comes back.
;
; Assemble: nasm -f bin -DMARKER='"windows-efi"' -o app.efi efi_test_app.asm
; The file is the whole PE image: headers in the first 4 KiB, one section
; (.text, code and data) in the next; file offsets equal RVAs. The code
; only uses RIP-relative addressing, so it needs no relocations.

bits 64
default rel

%ifndef MARKER
%define MARKER "efi-test"
%endif

IMAGE_BASE	equ 0x10000000
SECTION_ALIGNMENT	equ 0x1000
FILE_ALIGNMENT	equ 0x200
TEXT_RVA	equ 0x1000
TEXT_SIZE	equ 0x1000

; EFI_SYSTEM_TABLE and EFI_BOOT_SERVICES offsets (x86-64)
ST_CONOUT	equ 64
ST_BOOTSERVICES	equ 96
BS_HANDLEPROTOCOL	equ 152
CONOUT_OUTPUTSTRING	equ 8
LOADED_IMAGE_DEVICEHANDLE	equ 24

header:
	db	"MZ"
	times 0x3c - ($ - header) db 0
	dd	pe - header			; e_lfanew

pe:
	db	"PE", 0, 0
	dw	0x8664				; x86-64
	dw	1					; one section
	dd	0, 0, 0
	dw	optional_end - optional
	dw	0x0206				; executable, no line numbers, no debug info

optional:
	dw	0x020b				; PE32+
	db	0, 0
	dd	TEXT_SIZE			; size of code
	dd	0, 0
	dd	TEXT_RVA + (entry - text)	; entry point
	dd	TEXT_RVA			; base of code
	dq	IMAGE_BASE
	dd	SECTION_ALIGNMENT
	dd	FILE_ALIGNMENT
	dw	0, 0, 0, 0, 0, 0
	dd	0
	dd	TEXT_RVA + TEXT_SIZE	; size of image
	dd	TEXT_RVA			; size of headers
	dd	0					; checksum
	dw	10					; EFI application
	dw	0
	dq	0x10000, 0x10000, 0x10000, 0
	dd	0
	dd	16					; data directories, all empty
	times 16 dq 0
optional_end:

	db	".text", 0, 0, 0
	dd	TEXT_SIZE			; virtual size
	dd	TEXT_RVA			; virtual address
	dd	TEXT_SIZE			; raw size
	dd	TEXT_RVA			; raw offset
	dd	0, 0
	dw	0, 0
	dd	0x60000020			; code, execute, read

	times TEXT_RVA - ($ - header) db 0

text:
entry:
	; rcx = image handle, rdx = system table (Microsoft x64 calling
	; convention). Stack: 8 mod 16 at entry; three pushes and 48 bytes
	; (32 shadow space, 16 for the protocol pointer) align it.
	push	rbx
	push	rsi
	push	rdi
	sub		rsp, 48
	mov		rbx, rdx
	mov		rsi, rcx

	lea		rdi, [started]
	call	serial_print

	mov		rax, [rbx + ST_BOOTSERVICES]
	mov		rcx, rsi
	lea		rdx, [loaded_image_guid]
	lea		r8, [rsp + 32]
	call	[rax + BS_HANDLEPROTOCOL]
	lea		rdi, [device_no]
	test	rax, rax
	jnz		.print_device
	mov		rax, [rsp + 32]
	cmp		qword [rax + LOADED_IMAGE_DEVICEHANDLE], 0
	je		.print_device
	lea		rdi, [device_yes]
.print_device:
	call	serial_print

	mov		rcx, [rbx + ST_CONOUT]
	lea		rdx, [console_text]
	call	[rcx + CONOUT_OUTPUTSTRING]

	lea		rdi, [returning]
	call	serial_print

	xor		eax, eax			; EFI_SUCCESS
	add		rsp, 48
	pop		rdi
	pop		rsi
	pop		rbx
	ret

; Writes the zero-terminated string at rdi to COM1, waiting for the
; transmitter before each byte.
serial_print:
	mov		dx, 0x3fd
.wait:
	in		al, dx
	test	al, 0x20
	jz		.wait
	mov		al, [rdi]
	test	al, al
	jz		.done
	mov		dx, 0x3f8
	out		dx, al
	inc		rdi
	jmp		serial_print
.done:
	ret

started:
	db	13, 10, "CE-OTHEROS started ", MARKER, 13, 10, 0
device_yes:
	db	"CE-OTHEROS ", MARKER, " device handle: yes", 13, 10, 0
device_no:
	db	"CE-OTHEROS ", MARKER, " device handle: no", 13, 10, 0
returning:
	db	"CE-OTHEROS ", MARKER, " returning", 13, 10, 0

	align 2
console_text:
	dw	__utf16__("Renga test: other system "), __utf16__(MARKER), 13, 10, 0

	align 4
loaded_image_guid:
	dd	0x5b1b31a1
	dw	0x9562, 0x11d2
	db	0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b

	times TEXT_RVA + TEXT_SIZE - ($ - header) db 0
