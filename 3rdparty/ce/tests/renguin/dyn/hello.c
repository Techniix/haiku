/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Renguin test (F-27): the smallest dynamically linked Linux program. It is
 * linked against musl (Alpine's ld-musl) and against glibc (ld-linux), so
 * the ELF interpreter, its relocation of libc and the program, TLS set up
 * by the loader, stdio and the heap all have to work.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread int sTls = 7;

int
main(int argc, char** argv)
{
	char* copy = strdup(argc > 1 ? argv[1] : "none");
	const char* home = getenv("RENGUIN_TEST_ENV");
	printf("hello dynamic argc=%d arg=%s tls=%d env=%s\n", argc, copy, sTls,
		home != NULL ? "yes" : "no");
	free(copy);
	return 0;
}
