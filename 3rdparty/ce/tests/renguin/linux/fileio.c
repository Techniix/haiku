/* File-descriptor and path syscalls (Stage 4). Exits 0 and prints "ok", or
 * exits with the number of the first failing check. Works in an empty,
 * writable current directory. */
#include "sys.h"

struct linux_stat { u64 dev, ino, nlink; unsigned mode, uid, gid, pad;
	u64 rdev; i64 size, blksize, blocks, t[6], unused[3]; };

int check_main(int argc, char** argv)
{
	(void)argc; (void)argv;
	static const char name[] = "renguin-fileio.tmp";
	static const char data[] = "renguin file io\n";
	char buf[512];

	/* create, write, seek, read back */
	i64 fd = sys4(SYS_openat, AT_FDCWD, name, 0101 | 01000 | 02000000,
		0644);	/* O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC */
	if (fd < 0) return 2;
	if (sys3(SYS_write, fd, data, sizeof(data) - 1) != sizeof(data) - 1)
		return 3;
	if (sys1(SYS_close, fd) != 0) return 4;

	fd = sys4(SYS_openat, AT_FDCWD, name, 02, 0);	/* O_RDWR */
	if (fd < 0) return 5;
	struct linux_stat st;
	if (sys2(SYS_fstat, fd, &st) != 0) return 6;
	if (st.size != sizeof(data) - 1) return 7;
	if ((st.mode & 0170000) != 0100000) return 8;		/* S_IFREG */
	if (sys6(SYS_pread64, fd, (i64)buf, 7, 8, 0, 0) != 7) return 9;
	if (buf[0] != 'f' || buf[6] != 'o') return 10;
	if (sys3(SYS_lseek, fd, 0, 2) != sizeof(data) - 1) return 11;	/* END */
	if (sys2(SYS_ftruncate, fd, 4) != 0) return 12;
	if (sys4(SYS_newfstatat, AT_FDCWD, name, &st, 0) != 0) return 13;
	if (st.size != 4) return 14;

	/* fcntl, dup, dup3 */
	i64 fl = sys2(SYS_fcntl, fd, 3);			/* F_GETFL */
	if (fl < 0 || (fl & 3) != 2) return 15;
	if (sys2(SYS_fcntl, fd, 1) != 0) return 16;	/* F_GETFD: no CLOEXEC */
	i64 fd2 = sys1(SYS_dup, fd);
	if (fd2 < 0) return 17;
	if (sys3(SYS_dup3, fd, fd2, 02000000) != fd2) return 18;
	if (sys2(SYS_fcntl, fd2, 1) != 1) return 19;	/* FD_CLOEXEC set */
	if (sys3(SYS_dup3, fd, fd, 0) != -22) return 20;	/* EINVAL */
	sys1(SYS_close, fd2);
	sys1(SYS_close, fd);

	/* access, errors */
	if (sys3(SYS_faccessat, AT_FDCWD, name, 4) != 0) return 21;	/* R_OK */
	if (sys4(SYS_openat, AT_FDCWD, "renguin-no-such-file", 0, 0) != -2)
		return 22;		/* ENOENT */
	if (sys3(SYS_read, 999, buf, 1) != -9) return 23;	/* EBADF */

	/* getdents64 sees the file */
	i64 dir = sys4(SYS_openat, AT_FDCWD, ".", 0200000, 0);	/* O_DIRECTORY */
	if (dir < 0) return 24;
	int found = 0;
	for (;;) {
		i64 n = sys3(SYS_getdents64, dir, buf, sizeof(buf));
		if (n < 0) return 25;
		if (n == 0) break;
		for (i64 off = 0; off < n;) {
			unsigned short reclen = *(unsigned short*)(buf + off + 16);
			if (reclen == 0 || (reclen & 7) != 0) return 26;
			if (seq(buf + off + 19, name)) found = 1;
			off += reclen;
		}
	}
	sys1(SYS_close, dir);
	if (!found) return 27;

	/* pipe2 round trip */
	int p[2];
	if (sys2(SYS_pipe2, p, 02000000) != 0) return 28;
	if (sys3(SYS_write, p[1], "pipe", 4) != 4) return 29;
	if (sys3(SYS_read, p[0], buf, 4) != 4 || buf[3] != 'e') return 30;
	sys1(SYS_close, p[0]);
	sys1(SYS_close, p[1]);

	/* getcwd returns the length including the NUL */
	i64 len = sys2(SYS_getcwd, buf, sizeof(buf));
	if (len <= 1 || buf[0] != '/' || buf[len - 1] != 0) return 31;
	if (sys2(SYS_getcwd, buf, 1) != -34) return 32;		/* ERANGE */

	/* mkdir/rmdir, unlink */
	if (sys3(SYS_mkdirat, AT_FDCWD, "renguin-dir.tmp", 0755) != 0) return 33;
	if (sys3(SYS_unlinkat, AT_FDCWD, "renguin-dir.tmp", 0x200) != 0) return 34;
	if (sys3(SYS_unlinkat, AT_FDCWD, name, 0) != 0) return 35;
	if (sys4(SYS_newfstatat, AT_FDCWD, name, &st, 0) != -2) return 36;

	/* /proc/self/exe */
	len = sys4(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", buf, sizeof(buf));
	if (len <= 0 || buf[0] != '/') return 37;

	put("ok\n");
	return 0;
}
