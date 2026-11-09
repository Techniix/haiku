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
| `epoll` | `<sys/epoll.h>` | | Missing | Planned |
| `inotify` | `<sys/inotify.h>` | | Missing | Planned |
| `signalfd` | `<sys/signalfd.h>` | | Missing | Planned |
| System V shared memory (`shmget`, ...) | `<sys/shm.h>` | libroot (nothing extra) | Missing | Available |
| `getrandom()` | `<sys/random.h>` | `-lgnu` | Missing (`getentropy()` only) | Available |
| `getauxval()` | `<sys/auxv.h>` | `-lgnu` | Missing | Available |
| `backtrace()`, `backtrace_symbols()`, `backtrace_symbols_fd()` | `<execinfo.h>` | `-lgnu` | Missing | Available |
| `copy_file_range()` | `<unistd.h>` | `-lgnu` | Missing | Available |
| `sendmmsg()`, `recvmmsg()` | `<sys/socket.h>` | libnetwork (nothing extra) | Missing | Available |
| `getcontext()`/`setcontext()`/`makecontext()`/`swapcontext()` | `<ucontext.h>` | libroot (nothing extra) | Missing | Available (x86_64) |
| `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/`TCP_KEEPCNT` | `<netinet/tcp.h>` | | Missing (no TCP keepalive at all) | Planned |

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

## Feature details

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

### virtio-sound

Audio output in virtual machines (QEMU `-device virtio-sound-pci`) through
the standard Media Kit path. Playback only for now; capture is planned.

Since: commits `658d1756`, `fbbb290a`.

## Planned (do not work around these yet)

If a port needs one of these, note it rather than writing a Haiku-specific
workaround: they are on the roadmap and a workaround would have to be
removed again.

- `epoll` (on top of the kernel's event queue), then `inotify`.
- `signalfd`.
- TCP keepalive (`SO_KEEPALIVE` probes and `TCP_KEEPIDLE`/`TCP_KEEPINTVL`/
  `TCP_KEEPCNT`): Haiku's TCP does not send keepalive probes at all yet.
- File sealing for `memfd_create()`; `TFD_TIMER_CANCEL_ON_SET`.
- Linux driver support through LinuxKPI, and Linux binary compatibility
  through Renguin: both build on the APIs above.

## For assistants working on ports

When helping port software to Haiku and the target is Haiku Community
Edition:

1. Read this file from `ce/main` first; it is the source of truth for what
   exists.
2. Prefer the native API listed here over Haiku-specific fallbacks in the
   upstream project, and remove `__HAIKU__` special cases that only exist
   because the API was missing.
3. For anything under "Planned", leave a short note in the port instead of
   a workaround.
4. Remember the packaging caveat: Renga builds go to the Renga package repository.
