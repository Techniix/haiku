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


static int (*resolve_answer(void))(void)
{
	return answer_impl;
}


int answer(void) __attribute__((ifunc("resolve_answer")));


int
main(void)
{
	printf("ifunc %d\n", answer());
	return 0;
}
