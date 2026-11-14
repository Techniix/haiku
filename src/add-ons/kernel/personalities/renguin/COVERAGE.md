# Renguin: Linux system call coverage

What the renguin personality (`src/add-ons/kernel/personalities/renguin`)
does with each x86-64 Linux system call, as of 2026-11-14. The numbers and
names are those of `abi/linux_syscall_names.h` (FreeBSD's `syscalls.master`,
350 named entries, up to `map_shadow_stack`); the table comes from the dispatch in
`syscall.cpp`. Roadmap F-27 keeps the plan; the harness's `renguin` suite
tests what is listed as implemented (3rdparty/ce/README.md).

- **Implemented** (206): handled, mostly on Haiku's own calls with Linux
  structures, flags and errno values translated; event and memory
  descriptors (epoll, eventfd, timerfd, signalfd, inotify, memfd with
  seals) are Renga's own implementations (`fds.cpp`); sockets are Haiku's
  network stack (`net.cpp`), with a read-only NETLINK_ROUTE.
- **Stubbed** (20): accepted without effect, or refused with a fixed error
  that Linux programs handle (in brackets).
- **Missing** (124): -ENOSYS, logged once per program in the syslog
  ("unimplemented Linux syscall").

## Implemented

`0 read`, `1 write`, `2 open`, `3 close`, `4 stat`, `5 fstat`, `6 lstat`
`7 poll`, `8 lseek`, `9 mmap`, `10 mprotect`, `11 munmap`, `12 brk`
`13 rt_sigaction`, `14 rt_sigprocmask`, `15 rt_sigreturn`, `16 ioctl`
`17 pread64`, `18 pwrite64`, `19 readv`, `20 writev`, `21 access`
`22 pipe`, `23 select`, `24 sched_yield`, `25 mremap`, `26 msync`
`28 madvise`, `32 dup`, `33 dup2`, `34 pause`, `35 nanosleep`
`36 getitimer`, `37 alarm`, `38 setitimer`, `39 getpid`, `40 sendfile`
`41 socket`, `42 connect`, `43 accept`, `44 sendto`, `45 recvfrom`
`46 sendmsg`, `47 recvmsg`, `48 shutdown`, `49 bind`, `50 listen`
`51 getsockname`, `52 getpeername`, `53 socketpair`, `54 setsockopt`
`55 getsockopt`, `56 clone`, `57 fork`, `58 vfork`, `59 execve`, `60 exit`
`61 wait4`, `62 kill`, `63 uname`, `72 fcntl`, `73 flock`, `74 fsync`
`75 fdatasync`, `76 truncate`, `77 ftruncate`, `79 getcwd`, `80 chdir`
`81 fchdir`, `82 rename`, `83 mkdir`, `84 rmdir`, `85 creat`, `86 link`
`87 unlink`, `88 symlink`, `89 readlink`, `90 chmod`, `91 fchmod`
`92 chown`, `93 fchown`, `94 lchown`, `95 umask`, `96 gettimeofday`
`97 getrlimit`, `98 getrusage`, `99 sysinfo`, `100 times`, `102 getuid`
`104 getgid`, `105 setuid`, `106 setgid`, `107 geteuid`, `108 getegid`
`109 setpgid`, `110 getppid`, `111 getpgrp`, `112 setsid`, `113 setreuid`
`114 setregid`, `115 getgroups`, `116 setgroups`, `117 setresuid`
`118 getresuid`, `119 setresgid`, `120 getresgid`, `121 getpgid`
`122 setfsuid`, `123 setfsgid`, `124 getsid`, `127 rt_sigpending`
`128 rt_sigtimedwait`, `129 rt_sigqueueinfo`, `130 rt_sigsuspend`
`131 sigaltstack`, `132 utime`, `133 mknod`, `135 personality`
`137 statfs`, `138 fstatfs`, `140 getpriority`, `141 setpriority`
`142 sched_setparam`, `143 sched_getparam`, `144 sched_setscheduler`
`145 sched_getscheduler`, `146 sched_get_priority_max`
`147 sched_get_priority_min`, `148 sched_rr_get_interval`, `157 prctl`
`158 arch_prctl`, `160 setrlimit`, `162 sync`, `186 gettid`
`187 readahead`, `200 tkill`, `201 time`, `202 futex`
`203 sched_setaffinity`, `204 sched_getaffinity`, `213 epoll_create`
`217 getdents64`, `218 set_tid_address`, `221 fadvise64`
`228 clock_gettime`, `229 clock_getres`, `230 clock_nanosleep`
`231 exit_group`, `232 epoll_wait`, `233 epoll_ctl`, `234 tgkill`
`235 utimes`, `253 inotify_init`, `254 inotify_add_watch`
`255 inotify_rm_watch`, `257 openat`, `258 mkdirat`, `259 mknodat`
`260 fchownat`, `261 futimesat`, `262 newfstatat`, `263 unlinkat`
`264 renameat`, `265 linkat`, `266 symlinkat`, `267 readlinkat`
`268 fchmodat`, `269 faccessat`, `270 pselect6`, `271 ppoll`
`273 set_robust_list`, `274 get_robust_list`, `277 sync_file_range`
`280 utimensat`, `281 epoll_pwait`, `282 signalfd`, `283 timerfd_create`
`284 eventfd`, `285 fallocate`, `286 timerfd_settime`
`287 timerfd_gettime`, `288 accept4`, `289 signalfd4`, `290 eventfd2`
`291 epoll_create1`, `292 dup3`, `293 pipe2`, `294 inotify_init1`
`295 preadv`, `296 pwritev`, `297 rt_tgsigqueueinfo`, `299 recvmmsg`
`302 prlimit64`, `306 syncfs`, `307 sendmmsg`, `309 getcpu`
`316 renameat2`, `318 getrandom`, `319 memfd_create`, `327 preadv2`
`328 pwritev2`, `332 statx`, `435 clone3`, `439 faccessat2`
`441 epoll_pwait2`, `452 fchmodat2`

## Stubbed

`149 mlock (0)`, `150 munlock (0)`, `151 mlockall (0)`
`152 munlockall (0)`, `161 chroot (EPERM)`, `188 setxattr (EOPNOTSUPP)`
`189 lsetxattr (EOPNOTSUPP)`, `190 fsetxattr (EOPNOTSUPP)`
`191 getxattr (EOPNOTSUPP)`, `192 lgetxattr (EOPNOTSUPP)`
`193 fgetxattr (EOPNOTSUPP)`, `194 listxattr (EOPNOTSUPP)`
`195 llistxattr (EOPNOTSUPP)`, `196 flistxattr (EOPNOTSUPP)`
`197 removexattr (EOPNOTSUPP)`, `198 lremovexattr (EOPNOTSUPP)`
`199 fremovexattr (EOPNOTSUPP)`, `325 mlock2 (0)`
`326 copy_file_range (ENOSYS)`, `334 rseq (ENOSYS)`

## Missing

`27 mincore`, `29 shmget`, `30 shmat`, `31 shmctl`, `64 semget`
`65 semop`, `66 semctl`, `67 shmdt`, `68 msgget`, `69 msgsnd`, `70 msgrcv`
`71 msgctl`, `78 getdents`, `101 ptrace`, `103 syslog`, `125 capget`
`126 capset`, `136 ustat`, `139 sysfs`, `153 vhangup`, `154 modify_ldt`
`155 pivot_root`, `156 sysctl`, `159 adjtimex`, `163 acct`
`164 settimeofday`, `165 mount`, `166 umount`, `167 swapon`, `168 swapoff`
`169 reboot`, `170 sethostname`, `171 setdomainname`, `172 iopl`
`173 ioperm`, `175 init_module`, `176 delete_module`, `179 quotactl`
`206 io_setup`, `207 io_destroy`, `208 io_getevents`, `209 io_submit`
`210 io_cancel`, `212 lookup_dcookie`, `216 remap_file_pages`
`219 restart_syscall`, `220 semtimedop`, `222 timer_create`
`223 timer_settime`, `224 timer_gettime`, `225 timer_getoverrun`
`226 timer_delete`, `227 clock_settime`, `237 mbind`, `238 set_mempolicy`
`239 get_mempolicy`, `240 mq_open`, `241 mq_unlink`, `242 mq_timedsend`
`243 mq_timedreceive`, `244 mq_notify`, `245 mq_getsetattr`
`246 kexec_load`, `247 waitid`, `248 add_key`, `249 request_key`
`250 keyctl`, `251 ioprio_set`, `252 ioprio_get`, `256 migrate_pages`
`272 unshare`, `275 splice`, `276 tee`, `278 vmsplice`, `279 move_pages`
`298 perf_event_open`, `300 fanotify_init`, `301 fanotify_mark`
`303 name_to_handle_at`, `304 open_by_handle_at`, `305 clock_adjtime`
`308 setns`, `310 process_vm_readv`, `311 process_vm_writev`, `312 kcmp`
`313 finit_module`, `314 sched_setattr`, `315 sched_getattr`
`317 seccomp`, `320 kexec_file_load`, `321 bpf`, `322 execveat`
`323 userfaultfd`, `324 membarrier`, `329 pkey_mprotect`, `330 pkey_alloc`
`331 pkey_free`, `333 io_pgetevents`, `424 pidfd_send_signal`
`425 io_uring_setup`, `426 io_uring_enter`, `427 io_uring_register`
`428 open_tree`, `429 move_mount`, `430 fsopen`, `431 fsconfig`
`432 fsmount`, `433 fspick`, `434 pidfd_open`, `436 close_range`
`437 openat2`, `438 pidfd_getfd`, `440 process_madvise`
`442 mount_setattr`, `443 quotactl_fd`, `444 landlock_create_ruleset`
`445 landlock_add_rule`, `446 landlock_restrict_self`, `447 memfd_secret`
`448 process_mrelease`, `449 futex_waitv`, `450 set_mempolicy_home_node`
`451 cachestat`, `453 map_shadow_stack`

Next, by use in the application ladder (F-27): `waitid`, `close_range`,
`execveat`, `restart_syscall`, `membarrier`, `mincore`; the POSIX timers
(`timer_*`) and message queues (`mq_*`) and System V shared memory, which
Renga has natively (F-12) and only need mapping; `copy_file_range` and
`splice`/`tee` (coreutils, cat and cp fall back today); `pidfd_open`,
`pidfd_send_signal`, `pidfd_getfd` (with F-52); `getdents` (old ABI);
`ptrace` (gdb, strace; a kernel addition); `seccomp` and `unshare`/`setns`
(sandboxes, Flatpak; later); `io_uring` only if F-52 adds it.

## Beyond system calls

- **ioctl**: terminal requests (TCGETS/TCSETS*, TIOCGWINSZ/SWINSZ,
  TIOC[GS]PGRP, TIOCGSID, TIOCSCTTY), FIONREAD, FIONBIO, FIOCLEX/FIONCLEX;
  socket requests SIOCGIFNAME, SIOCGIFCONF, SIOCGIFFLAGS, SIOCGIFADDR,
  SIOCGIFDSTADDR, SIOCGIFBRDADDR, SIOCGIFNETMASK, SIOCGIFMETRIC,
  SIOCGIFMTU, SIOCGIFHWADDR, SIOCGIFINDEX, SIOCGIFTXQLEN, SIOCATMARK.
  Anything else: ENOTTY. Not yet: evdev and hidraw pass-through for
  gamepads (F-42 note), DRM, V4L2.
- **fcntl**: F_DUPFD(_CLOEXEC), F_GETFD/SETFD, F_GETFL/SETFL, F_GETLK/
  SETLK/SETLKW and the F_OFD_* variants (as process locks), F_ADD_SEALS/
  F_GET_SEALS, F_GETPIPE_SZ/SETPIPE_SZ (64 KiB reported), F_SETOWN/
  F_GETOWN/F_SETSIG/F_GETSIG (accepted, no SIGIO).
- **Sockets**: AF_UNIX (path names; abstract names are refused until
  F-52 adds them to Haiku's stack), AF_INET, AF_INET6 (no IPv4-mapped
  addresses: a server listening on [::] does not see IPv4 clients, as on
  Linux with IPV6_V6ONLY), AF_NETLINK with NETLINK_ROUTE (RTM_GETLINK,
  RTM_GETADDR, RTM_GETROUTE dumps and single requests; empty neighbour and
  rule dumps; changes refused with EPERM; no multicast notifications);
  AF_PACKET is refused. SCM_RIGHTS works in both directions and with
  native programs; SCM_CREDENTIALS and SO_PASSCRED are accepted and
  ignored. SO_RCVTIMEO/SO_SNDTIMEO expiry is EAGAIN, as on Linux. Socket
  options: SOL_SOCKET, IPPROTO_IP, IPPROTO_IPV6 and IPPROTO_TCP by table,
  performance hints accepted, others ENOPROTOOPT; TCP_INFO is not mapped
  yet. Interfaces are named "lo" and "eth0", "eth1", ... (Haiku's link
  layer does not tell Wi-Fi from Ethernet); indexes are Haiku's.
- **Files**: absolute paths are looked up in the Linux root
  (`/boot/system/non-packaged/compat/linux`) first, then in Haiku's tree,
  following links through both (path.cpp); `/proc` is generated
  (procfs.cpp); `/dev` is Haiku's. On BFS, which stores no access time,
  st_atime is the modification time (as on a Linux volume mounted with
  noatime) instead of "now"; access times set with utimensat() are kept
  only on volumes that store them. Extended attributes: EOPNOTSUPP until
  the F-52 mapping of BFS attributes.
