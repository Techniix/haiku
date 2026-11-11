/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Hands the machine to another boot record, the way an MBR does (Renga,
	roadmap F-22; see other_systems.cpp).

	The caller has put the boot record at 0x7c00 and, for a partition, a
	copy of the disk's MBR at 0x0600. This switches to real mode with the
	BIOS interrupt vector table in place (switch_to_real_mode() from
	bios_asm.S, the same way call_bios_internal() does) and jumps to
	0000:7c00 with the registers boot records expect: DL the BIOS drive
	number, DS:SI the partition table entry (in the MBR copy), all segment
	registers 0 and the stack just below 0x7c00. It does not return.

	Like bios_asm.S, this file has to be linked within the boot loader's
	first 64 kB: the real mode part runs in segment 0x1000 (the Jamfile
	lists it right after bios_asm.S; other_systems.cpp checks the
	address).
*/


#define FUNCTION(x) .globl x ; x ## :

// Parameters for the real mode part, next to the values bios_asm.S keeps
// at the start of the boot loader (SAVED_ESP ... SAVED_EBP, 0x10000 to
// 0x10017; the BIOS disk address packet is at 0x10020). Fixed addresses,
// not labels: i386 relocations keep their addend in the 16 bit field,
// which cannot hold "label - 0x10000".
#define CHAINLOAD_DRIVE		0x10018
#define CHAINLOAD_ENTRY		0x1001a


.text
.code32


/*!	void chainload_boot_record(uint32 drive, uint32 partitionEntry) */
FUNCTION(chainload_boot_record)
	cli
	movl	4(%esp), %eax
	movb	%al, CHAINLOAD_DRIVE
	movl	8(%esp), %eax
	movw	%ax, CHAINLOAD_ENTRY

	// the real mode interrupt vector table
	lidt	idt_descriptor

	call	switch_to_real_mode
	.code16

	cli
	movb	(CHAINLOAD_DRIVE - 0x10000), %dl
	movw	(CHAINLOAD_ENTRY - 0x10000), %si
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss
	movw	$0x7c00, %sp
	sti

	ljmp	$0x0, $0x7c00

	.code32
