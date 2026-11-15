/*
 * Copyright 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Checks, on the machine it runs on, the Renga interfaces the renga branch
 * of Chromium-HaikuOS relies on, the way Chromium calls them. Each check
 * prints "PASS name" or "FAIL name: reason"; the exit status is the number
 * of failures. Built and run by scripts/renga-tests.sh probe.
 *
 *   clang -D_GNU_SOURCE -D_DEFAULT_SOURCE -O0 -g -fuse-ld=lld \
 *       -o renga-probe tools/renga-probe.c -lgnu -lbsd -lnetwork
 */

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(name, cond, ...)                         \
  do {                                                 \
    if (cond) {                                        \
      printf("PASS %s\n", name);                       \
    } else {                                           \
      printf("FAIL %s: ", name);                       \
      printf(__VA_ARGS__);                             \
      printf(" (errno %d: %s)\n", errno, strerror(errno)); \
      failures++;                                      \
    }                                                  \
  } while (0)

/* MessagePumpEpoll: epoll_create1(0), an EFD_NONBLOCK eventfd as the wake-up
 * descriptor, EPOLLIN; a write from another thread wakes epoll_wait(). */
static int g_wake_fd;
static void* waker(void* arg) {
  (void)arg;
  usleep(50000);
  uint64_t one = 1;
  if (write(g_wake_fd, &one, sizeof(one)) != sizeof(one))
    perror("eventfd write");
  return NULL;
}

static void check_epoll_eventfd(void) {
  int ep = epoll_create1(0);
  g_wake_fd = eventfd(0, EFD_NONBLOCK);
  struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 42};
  int rv = (ep >= 0 && g_wake_fd >= 0)
               ? epoll_ctl(ep, EPOLL_CTL_ADD, g_wake_fd, &ev) : -1;
  pthread_t t;
  pthread_create(&t, NULL, waker, NULL);
  struct epoll_event out[16];
  int n = rv == 0 ? epoll_wait(ep, out, 16, 2000) : -1;
  pthread_join(t, NULL);
  uint64_t value = 0;
  ssize_t r = read(g_wake_fd, &value, sizeof(value));
  CHECK("epoll+eventfd wake-up", n == 1 && out[0].data.u64 == 42 &&
        r == sizeof(value) && value == 1,
        "epoll_wait()=%d read()=%zd value=%llu", n, r,
        (unsigned long long)value);
  r = read(g_wake_fd, &value, sizeof(value));
  CHECK("eventfd EFD_NONBLOCK empty read", r < 0 && errno == EAGAIN,
        "read()=%zd", r);
  close(g_wake_fd);
  close(ep);
}

/* FilePathWatcher (inotify): IN_CREATE | IN_MOVED_TO | IN_ONLYDIR on the
 * parent directory, as base/files/file_path_watcher_inotify.cc does. */
static void check_inotify(void) {
  char dir[] = "/tmp/renga-probe-XXXXXX";
  if (!mkdtemp(dir)) {
    CHECK("inotify", 0, "mkdtemp");
    return;
  }
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  int wd = fd >= 0 ? inotify_add_watch(fd, dir,
                                       IN_ATTRIB | IN_CREATE | IN_DELETE |
                                       IN_CLOSE_WRITE | IN_MOVE | IN_ONLYDIR)
                   : -1;
  char path[256];
  snprintf(path, sizeof(path), "%s/file", dir);
  int f = open(path, O_CREAT | O_WRONLY, 0600);
  if (f >= 0) {
    write(f, "x", 1);
    close(f);
  }
  struct pollfd p = {.fd = fd, .events = POLLIN};
  int pr = poll(&p, 1, 2000);
  char buf[4096] __attribute__((aligned(8)));
  ssize_t n = pr > 0 ? read(fd, buf, sizeof(buf)) : -1;
  int saw_create = 0, saw_close = 0;
  for (char* q = buf; n > 0 && q < buf + n;) {
    struct inotify_event* e = (struct inotify_event*)q;
    if (e->wd == wd && (e->mask & IN_CREATE) && !strcmp(e->name, "file"))
      saw_create = 1;
    if (e->wd == wd && (e->mask & IN_CLOSE_WRITE)) saw_close = 1;
    q += sizeof(*e) + e->len;
  }
  CHECK("inotify create/close-write", saw_create && saw_close,
        "wd=%d poll=%d read=%zd create=%d close=%d", wd, pr, n, saw_create,
        saw_close);
  unlink(path);
  rmdir(dir);
  close(fd);
}

/* PlatformSharedMemoryRegion::CreateAnonymousRegion() on Linux. */
static void check_memfd(void) {
  int fd = memfd_create("shared-memory-region",
                        MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
  if (fd < 0 && errno == EINVAL)
    fd = memfd_create("shared-memory-region", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  int ok = fd >= 0 && ftruncate(fd, 65536) == 0 &&
           fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) ==
               0;
  CHECK("memfd_create + F_ADD_SEALS", ok, "fd=%d", fd);
  if (!ok) return;
  int seals = fcntl(fd, F_GET_SEALS);
  CHECK("F_GET_SEALS", seals >= 0 && (seals & F_SEAL_SHRINK) &&
        (seals & F_SEAL_GROW) && (seals & F_SEAL_SEAL), "seals=%#x", seals);
  CHECK("sealed memfd refuses growth", ftruncate(fd, 131072) != 0 &&
        errno == EPERM, "ftruncate succeeded or wrong errno");
  char* w = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK("memfd MAP_SHARED", w != MAP_FAILED, "mmap");
  if (w != MAP_FAILED) {
    strcpy(w, "hello");
    munmap(w, 65536);
  }
  /* What Linux does for writable regions (RENGA-REQUESTS.md R-1). */
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  int ro = open(path, O_RDONLY | O_CLOEXEC);
  int flags = ro >= 0 ? fcntl(ro, F_GETFL) : -1;
  char* r = ro >= 0 ? mmap(NULL, 65536, PROT_READ, MAP_SHARED, ro, 0)
                    : MAP_FAILED;
  printf("INFO R-1 read-only reopen of a memfd (%s): %s\n", path,
         ro >= 0 && (flags & O_ACCMODE) == O_RDONLY && r != MAP_FAILED &&
                 !strcmp(r, "hello")
             ? "available"
             : "missing (writable regions use the file fallback)");
  if (r != MAP_FAILED) munmap(r, 65536);
  if (ro >= 0) close(ro);
  close(fd);
}

/* Stack traces: a frame in a static function must resolve with
 * tools/symbolize-trace.py; see RENGA-REQUESTS.md R-3. */
__attribute__((noinline)) static void static_frame(int out) {
  void* frames[16];
  int n = backtrace(frames, 16);
  backtrace_symbols_fd(frames, n, out);
}
__attribute__((noinline)) void exported_frame(int out) {
  static_frame(out);
  __asm__ volatile("");
}
static void check_backtrace(void) {
  int p[2];
  pipe(p);
  exported_frame(p[1]);
  close(p[1]);
  char buf[4096] = {0};
  ssize_t n = read(p[0], buf, sizeof(buf) - 1);
  close(p[0]);
  int named = strstr(buf, "(exported_frame+0x") != NULL;
  int offset = strstr(buf, "(+0x") != NULL;
  CHECK("backtrace_symbols_fd frames", n > 0 && named && offset,
        "exported name %d, static offset %d, output:\n%s", named, offset,
        buf);
  if (n > 0) {
    printf("---- backtrace_symbols_fd output (feed to symbolize-trace.py)\n%s"
           "----\n", buf);
  }
  printf("INFO static_frame at %p, exported_frame at %p\n",
         (void*)static_frame, (void*)exported_frame);
}

/* QUIC over UDP: IP_RECVTOS, IP_PKTINFO, IP_MTU_DISCOVER, recvmmsg() and a
 * sendmsg() with an IP_TOS control message (ECN), over loopback. */
static void check_udp(void) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  int c = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  socklen_t len = sizeof(a);
  int one = 1;
  int ok = bind(s, (struct sockaddr*)&a, sizeof(a)) == 0 &&
           getsockname(s, (struct sockaddr*)&a, &len) == 0;
  CHECK("setsockopt IP_RECVTOS",
        setsockopt(s, IPPROTO_IP, IP_RECVTOS, &one, sizeof(one)) == 0, "");
  CHECK("setsockopt IP_PKTINFO",
        setsockopt(s, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) == 0, "");
#ifdef IP_PMTUDISC_DO
  int pmtu = IP_PMTUDISC_DO;
  CHECK("setsockopt IP_MTU_DISCOVER",
        setsockopt(c, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof(pmtu)) == 0,
        "");
#endif
  char payload[] = "quic";
  struct iovec iov = {.iov_base = payload, .iov_len = sizeof(payload)};
  char cbuf[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr m = {.msg_name = &a, .msg_namelen = sizeof(a),
                     .msg_iov = &iov, .msg_iovlen = 1,
                     .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
  struct cmsghdr* cm = CMSG_FIRSTHDR(&m);
  cm->cmsg_level = IPPROTO_IP;
  cm->cmsg_type = IP_TOS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  int tos = 0x02; /* ECT(0) */
  memcpy(CMSG_DATA(cm), &tos, sizeof(tos));
  ssize_t sent = ok ? sendmsg(c, &m, 0) : -1;
  CHECK("sendmsg with IP_TOS control message", sent == sizeof(payload),
        "sendmsg()=%zd", sent);

  char rbuf[64];
  char rc[256];
  struct iovec riov = {.iov_base = rbuf, .iov_len = sizeof(rbuf)};
  struct mmsghdr mm = {.msg_hdr = {.msg_iov = &riov, .msg_iovlen = 1,
                                   .msg_control = rc,
                                   .msg_controllen = sizeof(rc)}};
  struct pollfd p = {.fd = s, .events = POLLIN};
  int got = poll(&p, 1, 2000) > 0 ? recvmmsg(s, &mm, 1, 0, NULL) : -1;
  int tos_seen = -1, pktinfo_seen = 0;
  for (struct cmsghdr* h = got == 1 ? CMSG_FIRSTHDR(&mm.msg_hdr) : NULL; h;
       h = CMSG_NXTHDR(&mm.msg_hdr, h)) {
    if (h->cmsg_level == IPPROTO_IP &&
        (h->cmsg_type == IP_RECVTOS || h->cmsg_type == IP_TOS))
      tos_seen = *(unsigned char*)CMSG_DATA(h);
    if (h->cmsg_level == IPPROTO_IP && h->cmsg_type == IP_PKTINFO)
      pktinfo_seen = 1;
  }
  CHECK("recvmmsg", got == 1 && mm.msg_len == sizeof(payload),
        "recvmmsg()=%d", got);
  CHECK("ECN codepoint received (IP_RECVTOS)", (tos_seen & 3) == 2,
        "tos=%d", tos_seen);
  CHECK("IP_PKTINFO received", pktinfo_seen, "");
  close(s);
  close(c);
}

#define INFO(name, cond) \
  printf("INFO %s: %s\n", name, (cond) ? "available" : "missing")

/* Renga's /proc supports open() only (no stat(), access() or listing), and
 * Chromium only ever open()s and read()s these files, so probe them the same
 * way: true if the file opens and has content. */
static int proc_readable(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  char c;
  ssize_t n = read(fd, &c, 1);
  close(fd);
  return n == 1;
}

/* Things Chromium probes and the port does not work around. */
static void check_misc(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CHECK("sched_getaffinity", sched_getaffinity(0, sizeof(set), &set) == 0 &&
        CPU_COUNT(&set) > 0, "");
  int sv[2];
  uid_t uid;
  gid_t gid;
  int ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 &&
           getpeereid(sv[0], &uid, &gid) == 0 && uid == geteuid();
  CHECK("getpeereid", ok, "");
  int t = socket(AF_INET, SOCK_DGRAM, 0);
  INFO("R-4 SO_TIMESTAMP",
#ifdef SO_TIMESTAMP
       setsockopt(t, SOL_SOCKET, SO_TIMESTAMP, &ok, sizeof(ok)) == 0
#else
       0
#endif
  );
  close(t);
  INFO("R-2 /proc/self/maps", proc_readable("/proc/self/maps"));
  INFO("R-6 /proc/sys/fs/inotify/max_user_watches",
       proc_readable("/proc/sys/fs/inotify/max_user_watches"));
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  check_epoll_eventfd();
  check_inotify();
  check_memfd();
  check_backtrace();
  check_udp();
  check_misc();
  printf("%d check(s) failed\n", failures);
  return failures;
}
