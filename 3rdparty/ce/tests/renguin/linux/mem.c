/* Exercises Stage 4 syscalls with raw syscalls only (no libc).
 * Exits 0 on success, or a small nonzero code identifying the failed check. */
typedef unsigned long u64;
typedef long i64;

static i64 sys(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
	i64 r;
	register i64 r10 __asm__("r10") = d;
	register i64 r8  __asm__("r8")  = e;
	register i64 r9  __asm__("r9")  = f;
	__asm__ volatile("syscall" : "=a"(r)
		: "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory");
	return r;
}
#define SYS_write 1
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_uname 63
#define SYS_exit_group 231

static u64 slen(const char* s){u64 n=0;while(s[n])n++;return n;}
static void put(const char* s){sys(SYS_write,1,(i64)s,slen(s),0,0,0);}
static void quit(int c){sys(SYS_exit_group,c,0,0,0,0,0);for(;;);}

int _start2(void)
{
	/* mmap an anonymous RW page, write to it, read it back */
	i64 p = sys(SYS_mmap, 0, 4096, 0x1|0x2 /*R|W*/, 0x2|0x20 /*PRIV|ANON*/, -1, 0);
	if (p < 0 || p == -1) quit(2);
	volatile char* m = (volatile char*)p;
	m[0] = 42; m[4095] = 7;
	if (m[0] != 42 || m[4095] != 7) quit(3);
	if (sys(SYS_munmap, p, 4096, 0,0,0,0) != 0) quit(4);

	/* brk: query, then grow by a page and use it */
	i64 b0 = sys(SYS_brk, 0,0,0,0,0,0);
	if (b0 <= 0) quit(5);
	i64 b1 = sys(SYS_brk, b0 + 4096, 0,0,0,0,0);
	if (b1 < b0 + 4096) quit(6);
	volatile char* h = (volatile char*)b0;
	h[0] = 1; h[4095] = 2;
	if (h[0] != 1 || h[4095] != 2) quit(7);

	/* uname: sysname must be "Linux" */
	char buf[6*65];
	if (sys(SYS_uname, (i64)buf, 0,0,0,0,0) != 0) quit(8);
	const char* exp = "Linux";
	for (int i = 0; i < 5; i++) if (buf[i] != exp[i]) quit(9);

	put("ok\n");
	quit(0);
	return 0;
}

__asm__(".globl _start\n_start:\n\tand $-16, %rsp\n\tcall _start2\n\thlt\n");
