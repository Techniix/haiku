# Porting software to Renga

This is the reference for porting applications to Renga, the `ce/main`
branch of
[Techniix/haiku](https://github.com/Techniix/haiku/tree/ce/main). It lists
what Renga provides beyond stock Haiku, how a port picks each feature up, and
what is planned, so ports can use the native APIs instead of carrying
workarounds written for stock Haiku.

It is kept current: every commit that adds or changes a port-visible feature
updates this file in the same commit.

**Base:** upstream Haiku `hrev60381`, plus the commits on `ce/main`.

## Quick reference

| Feature | Header | Link with | Stock Haiku | Status in Renga |
| --- | --- | --- | --- | --- |
| `eventfd()` | `<sys/eventfd.h>` | `-lgnu` | Missing | Available |
| `timerfd_create()` / `_settime()` / `_gettime()` | `<sys/timerfd.h>` | `-lgnu` | Missing | Available |
| `memfd_create()` | `<sys/mman.h>` | `-lgnu` | Missing | Available |
| POSIX message queues (`mq_*`) | `<mqueue.h>` | libroot (nothing extra) | Missing | Available |
| `virtio-sound` audio in VMs | (driver) | | Missing | Available (playback) |
| `epoll_create()`/`_create1()`/`_ctl()`/`_wait()`/`_pwait()`/`_pwait2()` | `<sys/epoll.h>` | `-lgnu` | Missing | Available |
| `inotify_init()`/`_init1()`/`_add_watch()`/`_rm_watch()` | `<sys/inotify.h>` | `-lgnu` | Missing | Available |
| `signalfd()` | `<sys/signalfd.h>` | `-lgnu` | Missing | Available |
| System V shared memory (`shmget`, ...) | `<sys/shm.h>` | libroot (nothing extra) | Missing | Available |
| `getrandom()` | `<sys/random.h>` | `-lgnu` | Missing (`getentropy()` only) | Available |
| `getauxval()` | `<sys/auxv.h>` | `-lgnu` | Missing | Available |
| `backtrace()`, `backtrace_symbols()`, `backtrace_symbols_fd()` | `<execinfo.h>` | `-lgnu` | Missing | Available |
| `copy_file_range()` | `<unistd.h>` | `-lgnu` | Missing | Available |
| `sendmmsg()`, `recvmmsg()` | `<sys/socket.h>` | libnetwork (nothing extra) | Missing | Available |
| `getcontext()`/`setcontext()`/`makecontext()`/`swapcontext()` | `<ucontext.h>` | libroot (nothing extra) | Missing | Available (x86_64) |
| TCP keepalive: `SO_KEEPALIVE` probes, `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` | `<netinet/tcp.h>` | libnetwork (nothing extra) | Missing (option accepted, no probes sent) | Available |
| `TCP_INFO` (`struct tcp_info`), `TCPS_*` states | `<netinet/tcp.h>`, `<netinet/tcp_fsm.h>` | libnetwork (nothing extra) | Missing | Available |
| Abortive close (`SO_LINGER` with zero timeout) sends a reset | (behaviour) | | Broken: no reset sent, peer never notified | Fixed |
| virtio-blk disks on x86_64 (boot from them, flush, trim) | (driver) | | Driver not shipped on x86_64 | Available |

All descriptors below are real file descriptors: they work with `select()`,
`poll()` and `wait_for_objects()`, are inherited by `fork()`, and honour
`O_CLOEXEC`/`FD_CLOEXEC`.

## How ports pick these up

**Headers and library.** The Linux-style APIs live in `libgnu`, with
headers in the `gnu` header directory (`headers/compatibility/gnu` in the
tree, `develop/headers/gnu` when installed). A port that already uses
`libgnu` (for example for `sched_getcpu()` or extended attributes) needs no
build changes. Otherwise add `-lgnu`, and add the `gnu` header directory to
the include path if the toolchain does not search it already.

POSIX message queues are standard POSIX, so they are in `libroot` and
`<mqueue.h>` like any other system header; nothing extra is needed.
`sysconf(_SC_MESSAGE_PASSING)` and `_POSIX_MESSAGE_PASSING` report support.

**Let the build system detect features.** Configure/CMake/Meson checks that
look for these functions on Linux will now find them on Renga. Prefer removing
`__HAIKU__` special cases that force a fallback, over adding Renga-specific
ones. Typical checks: `check_symbol_exists(eventfd "sys/eventfd.h" ...)`,
`AC_CHECK_FUNCS([eventfd timerfd_create memfd_create mq_open])`,
`cc.has_function('memfd_create', prefix: '#include <sys/mman.h>')`.

**Packaging caveat.** A binary that uses any of these will not load on stock
Haiku, because the symbols are missing there. Packages built against Renga
belong in the Renga package repository
([Techniix/haiku-packages](https://github.com/Techniix/haiku-packages)),
not in a repository meant for stock Haiku. If one source tree has to serve
both, keep the fallback path and choose at build time.

On Renga images the Techniix repository has a better priority than
HaikuPorts, so a package published there under the same name as a
HaikuPorts package replaces the HaikuPorts one on install and update, even
if HaikuPorts has a newer version. Bump the Techniix package when HaikuPorts
updates, or drop it from the repository once upstream has what it needed.

## Feature details

### epoll

`epoll_create()`, `epoll_create1()`, `epoll_ctl()`, `epoll_wait()`,
`epoll_pwait()` and `epoll_pwait2()`, with Linux's names, values and
`struct epoll_event` layout (packed on x86_64, as on Linux), so event loops
written for Linux (libuv and with it Node.js and Electron, libevent,
Chromium's `base/message_loop`, Wine, nginx-style servers) can use their
epoll backend instead of a `poll()` fallback.

- Works with everything that works with `poll()`: sockets (TCP, UDP,
  UNIX), pipes and FIFOs, `eventfd`, `timerfd`, message queues, terminals,
  and other epoll descriptors (nesting). Regular files and directories are
  refused with `EPERM`, as on Linux.
- Level-triggered (default), `EPOLLET` and `EPOLLONESHOT` (re-armed with
  `EPOLL_CTL_MOD`) behave as on Linux. Level-triggered descriptors that
  stay ready are reported round-robin when `maxevents` is smaller than the
  ready set. Every report reflects the descriptor's state at the time of
  the call, not an earlier notification.
- Events: `EPOLLIN`, `EPOLLOUT`, `EPOLLPRI`, `EPOLLRDNORM`, `EPOLLRDBAND`,
  `EPOLLWRNORM`, `EPOLLWRBAND`; `EPOLLERR` and `EPOLLHUP` always. `data` is
  returned exactly as given (`ptr`, `fd`, `u32`, `u64`).
- Registrations belong to the open file description, as on Linux: they
  survive `close()` while a `dup()`ed or inherited descriptor keeps it
  open, and go away with the last one. An epoll descriptor is inherited
  by `fork()` and shared: parent and child see the same registrations.
  `EPOLL_CLOEXEC` and `EPOLL_CLOFORK` (Haiku extension) are supported.
- The epoll descriptor itself is readable to `poll()`/`select()`/another
  epoll exactly when `epoll_wait()` would return something. Loops and
  nesting deeper than Linux allows fail with `ELOOP`.
- Timeouts: -1 waits forever, 0 does not wait, otherwise milliseconds
  (`epoll_pwait2()`: a `timespec`, rounded up to microseconds). A signal
  handler interrupts the wait with `EINTR`, even with `SA_RESTART`, as on
  Linux. `epoll_pwait()` swaps the signal mask atomically like `ppoll()`.
- Error codes follow epoll_ctl(2)/epoll_wait(2): `EBADF`, `EINVAL`
  (not an epoll fd, fd is the epoll fd, bad op, `maxevents` <= 0),
  `EEXIST`, `ENOENT`, `EPERM`, `ELOOP`, `EFAULT`. Unknown event bits are
  ignored, as on Linux.
- **Differences from Linux:**
  - `EPOLLEXCLUSIVE` is accepted on `EPOLL_CTL_ADD` (with Linux's
    restrictions: not with `EPOLLONESHOT`, not on `MOD`, not for epoll
    descriptors) and has no effect: every waiting instance is woken.
    Correct, only less efficient for many processes accepting on one
    socket.
  - `EPOLLWAKEUP` is accepted and ignored; `EPOLLMSG` is never reported.
  - `EPOLLRDHUP` is reported together with `EPOLLHUP` when the descriptor
    reports a disconnect. A peer's half-close (`shutdown(SHUT_WR)`) shows
    only as `EPOLLIN` with `read()` returning 0.
  - Closing the epoll descriptor while another thread waits on it wakes
    that thread with `EBADF` (Linux keeps it waiting).
  - One call returns at most 1024 events (the rest come with the next).
- Cost: every reported descriptor is re-checked when it is reported (as
  Linux re-polls it); 100 waits over 500 ready eventfds took about 0.5 to
  1 s under QEMU TCG, i.e. about 10-20 µs per reported event there.

Implementation: `src/system/kernel/events/epoll.cpp` (the design comment
there explains why it is built on the select hooks and select_sync like
Haiku's event_queue rather than on the event_queue itself). Pipes now
notify `select()`ors of every write, which edge-triggered epoll needs;
`ppoll()`/`pselect()` restore the caller's signal mask correctly after a
handler (an upstream bug, fixed with this work).

Since: the commit that added `src/system/kernel/events/epoll.cpp`.
Tests: `3rdparty/ce/tests/posix_compat/epoll_test.cpp` (84 checks).

### inotify

`inotify_init()`, `inotify_init1()` (`IN_NONBLOCK`, `IN_CLOEXEC`),
`inotify_add_watch()` and `inotify_rm_watch()`; events are `read()` as
`struct inotify_event` records with Linux's layout (names NUL-padded to a
multiple of 16 bytes, whole records only, `EINVAL` if the buffer cannot hold
the first one, `FIONREAD` for the queued byte count). The descriptor works
with `poll()`, `select()` and `epoll`, is shared across `fork()` and can be
used from several threads. This is what Chromium/Electron's
`FilePathWatcher`, Node's `fs.watch()` (libuv), GIO, Qt's
`QFileSystemWatcher` and file-watching build tools use on Linux.

- Events: `IN_ACCESS`, `IN_MODIFY` (every write, and truncation),
  `IN_ATTRIB` (mode, owner, times, extended attributes), `IN_CLOSE_WRITE`,
  `IN_CLOSE_NOWRITE`, `IN_OPEN`, `IN_CREATE`, `IN_DELETE`, `IN_MOVED_FROM`
  and `IN_MOVED_TO` (one cookie per rename, also across two watched
  directories), `IN_DELETE_SELF`, `IN_MOVE_SELF`, `IN_IGNORED`,
  `IN_Q_OVERFLOW`, `IN_ISDIR`.
- Flags: `IN_ONLYDIR`, `IN_DONT_FOLLOW`, `IN_MASK_ADD`, `IN_MASK_CREATE`,
  `IN_ONESHOT`. Adding a watch for an already watched node returns its
  watch descriptor and replaces (or with `IN_MASK_ADD` extends) its mask.
- Queue: identical consecutive events are merged and at most 16384 events
  are queued, the last place going to one `IN_Q_OVERFLOW` (wd -1), as with
  Linux's defaults. At most 8192 watches per instance (`ENOSPC`).
- Errors as on Linux: `EBADF`, `EINVAL` (not an inotify fd, no events in the
  mask, `IN_MASK_ADD` with `IN_MASK_CREATE`, unknown wd), `ENOENT`,
  `ENOTDIR`, `EEXIST`, `EACCES` (no read permission), `EFAULT`.
- **Differences from Linux:**
  - Names of a watched directory's entries come from a list the watch
    keeps: read when the watch is added (up to 100,000 entries) and kept
    current by create, delete and rename events. An entry renamed through
    a hard link outside the watched directory may keep its old name.
    `IN_ISDIR` on entries removed or moved out is known for entries created
    while watched and for the first 10,000 entries present when the watch
    was added.
  - `IN_DELETE_SELF` comes when the node's last name is removed, even if
    it is still open (Linux waits for the last close).
  - `IN_UNMOUNT` is never reported; `IN_EXCL_UNLINK` has no effect.
  - Memory-mapped writes produce no `IN_MODIFY` (Linux neither).

Implementation: `src/system/kernel/events/inotify.cpp` (design comment at
the top). Name events come from Haiku's node monitoring; reads, writes,
truncation, other stat changes, opens and closes from new hooks in the VFS,
because node monitoring has no such events and file systems rate-limit
their stat notifications. The hooks cost one atomic read while nothing is
watched.

Since: the commit that added `src/system/kernel/events/inotify.cpp`.
Tests: `3rdparty/ce/tests/posix_compat/inotify_test.cpp` (47 checks).

### signalfd

`signalfd(fd, mask, flags)`: block the signals with
`sigprocmask()`/`pthread_sigmask()`, then `read()` them from the descriptor
as 128-byte `struct signalfd_siginfo` records (Linux's layout) instead of
having handlers run. The descriptor works with `poll()`, `select()` and
`epoll`, so an event loop handles `SIGCHLD`, `SIGTERM` and friends with its
other events.

- `fd` -1 creates a descriptor; an existing signalfd gets its mask replaced
  (the call returns it). Flags: `SFD_NONBLOCK`, `SFD_CLOEXEC`, `SFD_CLOFORK`
  (Haiku extension). `SIGKILL` and `SIGSTOP` are silently left out of masks.
- `read()` dequeues pending signals of the mask for the calling thread
  (sent to it or to its process), as `sigwaitinfo()` would, as many as fit
  (`EINVAL` below 128 bytes, `EAGAIN` with `SFD_NONBLOCK`). Realtime
  signals keep their queue order and `sigqueue()` values.
- Readiness is per thread, as on Linux: a signal sent to another thread
  makes the descriptor readable for that thread only. In an epoll set it
  is evaluated for the thread probing the set. `kill()`, `raise()`,
  `pthread_kill()` and `sigqueue()` notify waiters before they return;
  signals sent by the kernel (`SIGCHLD`, timers) a moment later.
- After `fork()` the child reads its own signals through the inherited
  descriptor.
- Filled in: `ssi_signo`, `ssi_code`, `ssi_pid`, `ssi_uid`, `ssi_status`
  (`SIGCHLD` exit status), `ssi_int`/`ssi_ptr` (`sigqueue()` value),
  `ssi_addr`, `ssi_band`, `ssi_errno`.
- **Differences from Linux:**
  - Signal numbers are Haiku's (`SIGCHLD` is 5); use the names.
  - `ssi_utime`, `ssi_stime`, `ssi_fd`, `ssi_tid`, `ssi_overrun`,
    `ssi_trapno`, `ssi_addr_lsb`, `ssi_syscall`, `ssi_call_addr` and
    `ssi_arch` are 0.
  - Inherited Haiku behaviour: a `SIGCHLD` interrupts blocking calls
    (`poll()`, `epoll_wait()`, ...) with `EINTR` even while it is blocked.
    Event loops retry on `EINTR` anyway; the signal itself stays pending
    and is read from the signalfd.

Implementation: `src/system/kernel/events/signalfd.cpp`; reading uses the
kernel's `sigwaitinfo()` code, and `signal.cpp` notifies signalfds when a
signal becomes pending.

Since: the commit that added `src/system/kernel/events/signalfd.cpp`.
Tests: `3rdparty/ce/tests/posix_compat/signalfd_test.cpp` (28 checks).

### eventfd

A 64-bit counter as a file descriptor, for wakeups between threads and
processes. The Linux and FreeBSD semantics are matched: counter mode and
`EFD_SEMAPHORE`, `EFD_NONBLOCK`, `EFD_CLOEXEC` (plus `EFD_CLOFORK`, a Haiku
extension), 8-byte reads and writes, `EAGAIN` on overflow or empty counter,
and readability and writability reported to `poll()`.

Use it wherever a port emulates a wakeup with a pipe or socketpair on Haiku:
event-loop wakeups (glib's `GWakeup`, libuv async handles), thread pools,
and cross-process signalling.

Since: commit `dba8f621`. Tests: `3rdparty/ce/tests/posix_compat/eventfd_test.cpp`.

### timerfd

Timers delivered through a file descriptor, so an event loop can wait for
timers and I/O in one `poll()`.

- Clocks: `CLOCK_MONOTONIC` and `CLOCK_REALTIME`. Absolute `CLOCK_REALTIME`
  timers follow changes of the system time.
- Flags: `TFD_NONBLOCK`, `TFD_CLOEXEC`, `TFD_CLOFORK` (Haiku extension),
  `TFD_TIMER_ABSTIME`.
- `read()` returns the number of expirations since the last read. Periodic
  timers count every elapsed period, even when nobody read for a while.
- **Not supported:** `TFD_TIMER_CANCEL_ON_SET` fails with `EINVAL`, so
  callers fall back to their own clock-change handling.
- `write()` fails with `EINVAL`, as on Linux.

Since: commits `626dd266`, `b1d575c2`. Tests: `timerfd_test.cpp`.

### memfd_create

An anonymous, memory-backed file: size it with `ftruncate()`, `mmap()` it
`MAP_SHARED`, pass it to another process over a UNIX socket
(`SCM_RIGHTS`) or by `fork()`. It disappears when the last descriptor and
mapping are gone. This is the shared-memory pattern of Wayland (`wl_shm`
pools), Chromium/Electron IPC, Mesa and PipeWire.

- Flags: `MFD_CLOEXEC`. `MFD_ALLOW_SEALING`, `MFD_NOEXEC_SEAL` and
  `MFD_EXEC` are accepted and have no effect.
- **Not supported:** file sealing. `F_ADD_SEALS`/`F_GET_SEALS` are
  deliberately left undefined, so code guarded by `#ifdef F_ADD_SEALS`
  builds without sealing. `MFD_HUGETLB` fails with `EINVAL`.
- Names are for debugging only; up to 249 bytes, any characters.

Since: commit `e671e003`. Tests: `memfd_test.cpp`.

### POSIX message queues

`mq_open()`, `mq_send()`, `mq_timedsend()`, `mq_receive()`,
`mq_timedreceive()`, `mq_getattr()`, `mq_setattr()`, `mq_notify()`,
`mq_close()`, `mq_unlink()`, as specified by POSIX.1-2017.

- A `mqd_t` is a file descriptor (as on Linux and FreeBSD): `poll()` reports
  `POLLIN` while messages are queued and `POLLOUT` while there is room.
  Descriptors are closed on `exec()`, as POSIX requires.
- Names are `"/name"` (one leading slash, no other slashes). Owner, group
  and mode are checked on open; only the owner or root may `mq_unlink()`.
- Messages come out highest priority first, oldest first within a priority.
  `MQ_PRIO_MAX` is 32768.
- Defaults when `attr` is `NULL`: 10 messages of 8192 bytes (as Linux).
  Limits: 1024 messages, 1 MiB per message, 64 MiB per queue, 1024 queues.
- `mq_notify()` supports `SIGEV_SIGNAL` (with `si_code == SI_MESGQ`),
  `SIGEV_THREAD` and `SIGEV_NONE`. Blocking calls are restarted after
  `SA_RESTART` handlers and fail with `EINTR` otherwise, as on Linux.

Since: commit `06fb0d25`. Tests: `mqueue_test.cpp`.

### System V shared memory

`shmget()`, `shmat()`, `shmdt()`, `shmctl()` (`IPC_STAT`, `IPC_SET`,
`IPC_RMID`), as specified by POSIX.1-2017, in libroot.

- `shm_nattch` is exact: it counts every process with the segment
  attached, including children that inherited it by `fork()`, and drops
  when a process exits or `exec()`s without calling `shmdt()`. PostgreSQL's
  check for a running server relies on this.
- Attached segments are shared across `fork()` and dropped on `exec()`.
  `SHM_RDONLY` attaches are really read-only (writes fault).
- `IPC_RMID` removes the identifier at once; attached processes keep using
  the memory, which is freed after the last detach.
- Memory is allocated lazily: an untouched segment costs nothing.
- `SHMLBA` is 4096; `SHM_RND` rounds an attach address down to it.
- Limits: 4096 segments, each up to half of physical memory, all together
  up to the size of physical memory. Linux-only commands (`IPC_INFO`,
  `SHM_INFO`, `SHM_LOCK`) fail with `EINVAL`.

Since: commit `c6cbef2e`. Tests:
`shm_test.cpp`.

### libc additions

- `getrandom()`: the kernel generator `getentropy()` uses, without its
  256-byte limit (up to 32 MiB per call, as Linux). Haiku's generator is
  seeded before any program runs, so it never blocks and the `GRND_*`
  flags change nothing.
- `getauxval()`: Haiku has no auxiliary vector, so the values are derived
  from the process and program image: `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/
  `AT_ENTRY` (from the program's ELF header), `AT_BASE` (runtime loader),
  `AT_PAGESZ`, `AT_HWCAP` (x86: CPUID 1 EDX, as Linux), `AT_HWCAP2` (0),
  `AT_RANDOM` (16 bytes per process), `AT_EXECFN`, `AT_PLATFORM`,
  `AT_CLKTCK`, `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`, `AT_SECURE`,
  `AT_MINSIGSTKSZ`. Others (including `AT_SYSINFO_EHDR`: no vDSO) return 0
  with `ENOENT`, as glibc does.
- `backtrace()` and friends: frames come from the unwind tables, so code
  built without frame pointers works. A frame is only named after a
  function it lies inside (dynamic symbol and its size); frames in static
  functions print as `image(+0xoffset)`, resolvable with `addr2line`,
  instead of the misleading nearest-symbol names glibc can print.
  `backtrace_symbols_fd()` does not allocate (usable in crash handlers).
- `copy_file_range()`: user-space copy through a buffer; works across file
  systems; `EINVAL` for flags, non-regular files and overlapping ranges in
  one file, `EBADF` for an `O_APPEND` output.
- `sendmmsg()`/`recvmmsg()`: in libnetwork, as loops over
  `sendmsg()`/`recvmsg()` with Linux's error and `MSG_WAITFORONE`/timeout
  semantics. Correct, not yet faster than single calls.

Since: commit `9fef005a`. Tests:
`libc_ext_test.cpp`.

### User contexts (`<ucontext.h>`)

`getcontext()`, `setcontext()`, `makecontext()` and `swapcontext()`, the
basis of coroutine code (QEMU's coroutine backend, libtask, older
green-thread runtimes). In libroot; x86_64 for now (other architectures
need their own small assembly file).

- A context holds the callee-saved registers, stack and instruction
  pointers, the x87 control word and MXCSR (so rounding modes are per
  context), and the signal mask. Switching costs one `sigprocmask()`
  syscall, as with glibc.
- `makecontext()` passes any number of arguments the ABI way (six in
  registers, the rest on the stack) and reads them as 64-bit values, so
  pointer arguments work as with glibc. Stacks are 16-byte aligned.
- Returning from a `makecontext()` function resumes `uc_link`; with a
  NULL `uc_link` the process exits with status 0 (glibc's behaviour).
- `<sys/ucontext.h>` exists for code that includes it.

Since: commit `e3e8cb3b`. Tests:
`ucontext_test.cpp`.

### TCP keepalive

`SO_KEEPALIVE` now does what it does elsewhere: an idle connection is
probed, and one whose peer has gone away fails with `ETIMEDOUT` instead
of staying open forever. The per-socket tunables are there too.

- `TCP_KEEPIDLE` (seconds idle before the first probe, default 7200),
  `TCP_KEEPINTVL` (seconds between probes, default 75) and `TCP_KEEPCNT`
  (unanswered probes before giving up, default 9), set with
  `setsockopt(fd, IPPROTO_TCP, ...)`. Ranges as on Linux: 1..32767 s and
  1..127 probes, `EINVAL` otherwise.
- The numeric values are FreeBSD's (0x100, 0x200, 0x400), not Linux's
  (4, 5, 6; Linux's 4 is Haiku's `TCP_NOPUSH`). Code that uses the names,
  as libcurl, libuv, OpenSSH and most others do, needs nothing. Code
  guarded by `#ifdef __linux__` around these options should test for
  `#ifdef TCP_KEEPIDLE` instead; that is also the upstream-friendly fix.
  macOS's `TCP_KEEPALIVE` (idle time) is not provided; use
  `TCP_KEEPIDLE`.
- Accepted sockets inherit `SO_KEEPALIVE` and the three values from the
  listening socket.
- When the peer stops answering, blocked `recv()`/`read()` calls return
  `ETIMEDOUT`, `SO_ERROR` reports it, and later writes fail.
- Changing idle or interval on a connected socket takes effect at once.

Stock Haiku accepts `SO_KEEPALIVE` but never sends a probe and has no
`TCP_KEEP*` options, so ports often disabled keepalive or patched it
out; those patches can be dropped.

Since: commit `9c6d9157`. Tests:
`tcp_keepalive_test.cpp`.

### TCP_INFO

`getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &length)` fills a
`struct tcp_info` for any TCP socket.

- The layout is FreeBSD's. Its first part, up to `tcpi_rcv_space`, matches
  Linux byte for byte, so code reading `tcpi_rtt`, `tcpi_rttvar`,
  `tcpi_snd_mss`, `tcpi_rcv_mss`, `tcpi_snd_cwnd`, `tcpi_snd_ssthresh`,
  `tcpi_rcv_space` or `tcpi_last_data_recv` works unchanged.
- Units are FreeBSD's: microseconds for times, bytes for windows. The one
  difference that matters for Linux code: `tcpi_snd_cwnd` is in bytes, not
  segments (divide by `tcpi_snd_mss`). `tcpi_last_data_recv` is
  microseconds (Linux: milliseconds).
- `tcpi_state` is a BSD `TCPS_*` value from `<netinet/tcp_fsm.h>`
  (`TCPS_ESTABLISHED` is 4), not Linux's `TCP_ESTABLISHED` (1). Compare
  against the `TCPS_*` names, as FreeBSD ports already do.
- Filled in: state, options and window scales, RTO, MSS both ways, RTT and
  variation, congestion window, slow start threshold, receive space, peer
  window, `tcpi_snd_nxt`/`tcpi_rcv_nxt`. Retransmission counters and the
  other `__tcpi_*` fields read as zero.
- Like on Linux and FreeBSD, a shorter buffer gets the start of the
  structure. `TCP_INFO` is 0x20 (FreeBSD's number); code guarded by
  `#ifdef __linux__` should test `#ifdef TCP_INFO` instead.

Since: commit `dcb5350b`. Tests: `tcp_info_test.cpp`.

### Abortive close

`close()` with `SO_LINGER` set to `{1, 0}` now sends a reset, as everywhere
else: the peer's blocked `recv()` returns `ECONNRESET` at once and its
writes fail. Stock Haiku sent nothing, leaving the peer connected to a
socket that no longer existed. Workarounds in ports (timeouts added only
for Haiku, say) can go.

Since: commit `efd6464a`. Tests: `tcp_close_test.cpp`.

### virtio-blk disks

Renga ships the `virtio_block` driver on x86_64 (stock Haiku only does on
ARM and RISC-V), so VMs using virtio-blk disks (the default in
virt-manager/libvirt and Proxmox) see them and can boot from them. The
driver flushes the host cache on `fsync()`/`B_FLUSH_DRIVE_CACHE` and passes
trims through (DISCARD, or WRITE ZEROES with unmap), so `fstrim` gives
space back to sparse or thin images. Not a porting API, but useful when a
port's CI or test VM uses virtio-blk.

Since: commit `5b068a5e`. Tests: `vblk_test.cpp`.

### virtio-sound

Audio output in virtual machines (QEMU `-device virtio-sound-pci`) through
the standard Media Kit path. Playback only for now; capture is planned.

Since: commits `658d1756`, `fbbb290a`.

### Ctrl is the shortcut key

Renga uses the Windows/Linux convention: Ctrl+C copies, Ctrl+V pastes,
Alt+Tab switches windows. BeOS and Haiku use Alt. Nothing changed in the API:
Renga's default keymap puts `B_COMMAND_KEY` on the Ctrl keys and
`B_CONTROL_KEY` on the Alt keys, as Haiku's own "Windows/Linux mode" always
did. Users can switch back in Keymap preferences ("Shortcut key: Ctrl / Alt").

For ports:

- Use `B_COMMAND_KEY` for shortcuts (`AddShortcut()`, `BMenuItem`), as on
  Haiku. Do not map a toolkit's "Ctrl" to `B_CONTROL_KEY` for shortcuts:
  that is the Alt key in Renga's default and the Ctrl key in Haiku mode.
  Toolkits that translate Haiku's modifiers (SDL, Qt, GTK backends) should
  translate `B_COMMAND_KEY` to their "Ctrl"/"Primary" modifier.
- Do not decide by physical key codes (0x5c, 0x5d) which key is which; read
  the modifier bits, or `get_key_map()` if a port must show key names.
- Programs that need raw Ctrl+letter control characters (terminal emulators,
  editors running in one) get `B_COMMAND_KEY` for Ctrl in Renga's default.
  Terminal handles this (Ctrl+letter sends the control character,
  Ctrl+Shift+letter runs the shortcut); a port of another terminal should
  do the same when `left_command_key` is 0x5c, see
  `headers/private/shared/ShortcutKeyStyle.h`.

Since: the commit that added `headers/private/shared/ShortcutKeyStyle.h`.
Tests: `shortcut_keys_test.cpp`.

## Planned (do not work around these yet)

If a port needs one of these, note it rather than writing a Haiku-specific
workaround: they are on the roadmap and a workaround would have to be
removed again.

- File sealing for `memfd_create()`; `TFD_TIMER_CANCEL_ON_SET`.
- Linux driver support through LinuxKPI, and Linux binary compatibility
  through Renguin: both build on the APIs above.

## For assistants working on ports

When helping port software to Haiku and the target is Renga:

1. Read this file from `ce/main` first; it is the source of truth for what
   exists.
2. Prefer the native API listed here over Haiku-specific fallbacks in the
   upstream project, and remove `__HAIKU__` special cases that only exist
   because the API was missing.
3. For anything under "Planned", leave a short note in the port instead of
   a workaround.
4. Remember the packaging caveat: Renga builds go to the Renga package repository.
