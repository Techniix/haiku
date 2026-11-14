/* Raw Linux x86-64 system calls for libc-free test programs. */
#ifndef RENGUIN_TEST_SYS_H
#define RENGUIN_TEST_SYS_H

typedef unsigned long u64;
typedef long i64;

static inline i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
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
#define sys0(n)             sys6(n, 0, 0, 0, 0, 0, 0)
#define sys1(n, a)          sys6(n, (i64)(a), 0, 0, 0, 0, 0)
#define sys2(n, a, b)       sys6(n, (i64)(a), (i64)(b), 0, 0, 0, 0)
#define sys3(n, a, b, c)    sys6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
#define sys4(n, a, b, c, d) sys6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)

enum {
	SYS_read = 0, SYS_write = 1, SYS_close = 3, SYS_fstat = 5, SYS_lseek = 8,
	SYS_mmap = 9, SYS_munmap = 11, SYS_brk = 12, SYS_pread64 = 17,
	SYS_dup = 32, SYS_nanosleep = 35, SYS_getpid = 39, SYS_uname = 63,
	SYS_fcntl = 72, SYS_ftruncate = 77, SYS_getcwd = 79, SYS_umask = 95,
	SYS_getuid = 102, SYS_getppid = 110, SYS_gettid = 186,
	SYS_getdents64 = 217, SYS_clock_gettime = 228, SYS_exit_group = 231,
	SYS_openat = 257, SYS_mkdirat = 258, SYS_newfstatat = 262,
	SYS_unlinkat = 263, SYS_readlinkat = 267, SYS_faccessat = 269,
	SYS_dup3 = 292, SYS_pipe2 = 293, SYS_prlimit64 = 302, SYS_getrandom = 318,
	SYS_clone = 56, SYS_fork = 57, SYS_vfork = 58, SYS_execve = 59,
	SYS_wait4 = 61, SYS_kill = 62, SYS_exit = 60,
};
#define AT_FDCWD (-100)

static u64 slen(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static void put(const char* s) { sys3(SYS_write, 1, s, slen(s)); }
static void quit(int c) { sys1(SYS_exit_group, c); for (;;); }
static int seq(const char* a, const char* b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

/* Every program defines check_main(); this entry calls it and exits. */
int check_main(int argc, char** argv);
__asm__(".globl _start\n_start:\n\txor %ebp, %ebp\n\tmov %rsp, %rdi\n"
	"\tand $-16, %rsp\n\tcall _start_c\n\thlt\n");
void _start_c(u64* sp) { quit(check_main((int)sp[0], (char**)(sp + 1))); }

#endif
