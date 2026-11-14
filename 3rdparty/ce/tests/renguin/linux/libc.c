/* A real C library program: static glibc (or musl) printf, malloc, stdio
 * file access, and exit status. Linked statically, it exercises the libc
 * startup path (brk, arch_prctl, set_tid_address, rseq, prlimit64,
 * readlinkat, getrandom, mprotect, newfstatat). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	char* heap = malloc(100000);
	if (heap == NULL)
		return 2;
	strcpy(heap, "heap");

	FILE* f = fopen("renguin-libc.tmp", "w");
	if (f == NULL)
		return 3;
	fprintf(f, "%s %d\n", heap, 42);
	fclose(f);
	f = fopen("renguin-libc.tmp", "r");
	char line[32] = "";
	if (f == NULL || fgets(line, sizeof(line), f) == NULL)
		return 4;
	fclose(f);
	unlink("renguin-libc.tmp");
	if (strcmp(line, "heap 42\n") != 0)
		return 5;

	printf("hello from libc, argc=%d %s\n", argc, argc > 1 ? argv[1] : "");
	free(heap);
	return 3;
}
