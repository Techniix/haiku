/* Checks the Linux initial stack: argc/argv/envp/auxv layout.
 * Built freestanding: no libc, only raw syscalls. Exits 0 on success,
 * otherwise the number of the first failed check. */
typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
	long r;
	__asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
		: "rcx", "r11", "memory");
	return r;
}

static u64 len(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char* s) { sys3(1, 1, (long)s, len(s)); }
static void quit(int status) { sys3(231, status, 0, 0); for (;;) ; }

void cstart(u64* sp)
{
	u64 argc = sp[0];
	char** argv = (char**)(sp + 1);
	char** envp = argv + argc + 1;
	char** e = envp;
	while (*e) e++;
	u64* auxv = (u64*)(e + 1);

	u64 pagesz = 0, phdr = 0, phnum = 0, entry = 0, random = 0, execfn = 0;
	for (u64* a = auxv; a[0] != 0; a += 2) {
		switch (a[0]) {
			case 3: phdr = a[1]; break;      /* AT_PHDR */
			case 5: phnum = a[1]; break;     /* AT_PHNUM */
			case 6: pagesz = a[1]; break;    /* AT_PAGESZ */
			case 9: entry = a[1]; break;     /* AT_ENTRY */
			case 25: random = a[1]; break;   /* AT_RANDOM */
			case 31: execfn = a[1]; break;   /* AT_EXECFN */
		}
	}

	if (argc < 1) quit(10);
	if (argv[argc] != 0) quit(11);
	if (pagesz != 4096) quit(12);
	if (phdr == 0 || phnum == 0) quit(13);
	if (entry == 0) quit(14);
	if (random == 0) quit(15);
	if (execfn == 0) quit(16);
	if (((u64)sp & 15) != 0) quit(17);   /* rsp is 16-byte aligned at entry */

	for (u64 i = 1; i < argc; i++) { put(argv[i]); put("\n"); }
	quit(0);
}

__asm__(".globl _start\n_start:\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall cstart\n\thlt\n");
