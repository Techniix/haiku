/*
 * Layout of Renguin's vDSO, after Linux's arch/x86/entry/vdso/vdso-layout.lds.S:
 * one read-only, executable PT_LOAD at address 0 holding everything (the
 * file is the memory image, so the module copies it as it is), and a
 * read-only PT_DYNAMIC, which tells glibc's loader not to write to it.
 */
SECTIONS
{
	. = SIZEOF_HEADERS;

	.hash		: { *(.hash) }			:text
	.gnu.hash	: { *(.gnu.hash) }
	.dynsym		: { *(.dynsym) }
	.dynstr		: { *(.dynstr) }
	.gnu.version	: { *(.gnu.version) }
	.gnu.version_d	: { *(.gnu.version_d) }
	.gnu.version_r	: { *(.gnu.version_r) }

	.dynamic	: { *(.dynamic) }		:text	:dynamic

	.rodata		: { *(.rodata*) *(.data*) *(.bss*) *(COMMON) }	:text

	. = ALIGN(16);
	.text		: { *(.text*) }			:text

	/DISCARD/ : {
		*(.note*)
		*(.eh_frame*)
		*(.comment)
		*(.got*)
		*(.plt*)
	}
}

PHDRS
{
	text		PT_LOAD		FLAGS(5) FILEHDR PHDRS;	/* PF_R | PF_X */
	dynamic		PT_DYNAMIC	FLAGS(4);				/* PF_R */
}
