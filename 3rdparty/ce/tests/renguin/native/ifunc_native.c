/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (Chromium R-8): a native Haiku program with an IFUNC. The
 * linker brands such programs ELFOSABI_GNU (EI_OSABI 3); Renguin must
 * leave them to Haiku's runtime_loader. Prints "ifunc 42".
 */
#include <stdio.h>

static int
answer_impl(void)
{
	return 42;
}


__attribute__((used)) static int (*resolve_answer(void))(void)
{
	return answer_impl;
}


/*	The IFUNC in assembly, not __attribute__((ifunc)): clang 21 for
	x86_64-unknown-haiku drops that attribute without a word, which left
	"answer" undefined (runtime_loader: "Could not resolve symbol"). The
	directive gives the same STT_GNU_IFUNC symbol with any clang, and
	ld.lld still turns the call into an R_X86_64_IRELATIVE relocation. */
__asm__(".globl answer\n"
	".type answer, @gnu_indirect_function\n"
	".set answer, resolve_answer\n");
int answer(void);


int
main(void)
{
	printf("ifunc %d\n", answer());
	return 0;
}
