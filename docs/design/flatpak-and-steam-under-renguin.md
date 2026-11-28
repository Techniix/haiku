# Flatpak and Steam under Renguin: design and what is missing

Batch 13, piece 3 (maintainer, 2026-11-26). Roadmap `F-27`, "Getting Linux apps" and
"Games and Windows programs". Status: design. Nothing here is implemented yet; each step below
names the work it needs and how it will be tested.

## Where Renguin stands (ce/main, batch 13)

- One Linux root per system, fixed at `/boot/system/non-packaged/compat/linux`. Path lookup is
  per call (`path.cpp`, a FreeBSD-style "emul path"). Absolute link targets under `/boot/` are
  host paths.
- `chroot()` returns `EPERM`. `unshare`, `setns`, `clone` namespace flags, `mount`,
  `pivot_root` and `seccomp` are not there (`COVERAGE.md`).
- 64-bit programs only. Programs come from Alpine `apk` (musl) or static glibc; AppImages are
  extracted and run.
- Memory: `madvise(MADV_DONTNEED)` has Linux semantics from batch 13 (it was ignored).
  Mappings beyond a file's end are refused (the batch 11 attempt panicked the VM's commitment
  accounting and was reverted).

## Flatpak

Flatpak runs every application, and its own install triggers, through bubblewrap (`bwrap`).
`bwrap` builds the sandbox from a user namespace and a mount namespace:
- `--ro-bind <runtime>/files /usr`
- `--bind <app>/files /app`
- tmpfs `/tmp`
- `/proc`, `/dev`, `--symlink`, `--setenv`, `--seccomp <fd>`

It then executes the program inside. Flatpak itself (`flatpak`, `ostree`) is ordinary
userspace; it needs HTTPS and a writable repository under `~/.local/share/flatpak`.

### Step 1: unsandboxed Flatpak (no namespaces)

1. **Per-process root in Renguin** (kernel module, protected path).
   - Each Linux team gets a root prefix: today's constant `RENGUIN_LINUX_ROOT` becomes a
     field of the team's Renguin data, inherited on `fork()` and kept on `exec()`.
   - `chroot(path)` sets it to the host path of `path`, which must be a directory the
     caller can read.
   - `path.cpp` resolves against the field instead of the constant. `/proc/self/root`,
     `getcwd()` and the mount table (`procfs.cpp`) follow it.
   - Programs outside a chroot see no change.
   - Tests: a `libc-chroot` program (chroot into a directory of the root, open a file by its
     absolute path, symlinks with absolute targets, `..` stops at the new root, `fork` and
     `exec` keep it). Plus a quick default-profile boot run.
2. **`bwrap` shim** (`/usr/bin/bwrap` in the Linux root, a small static program).
   - It interprets the bubblewrap options Flatpak uses (`--ro-bind`, `--bind`,
     `--dev-bind`, `--symlink`, `--dir`, `--tmpfs`, `--setenv`, `--unsetenv`, `--chdir`,
     `--args <fd>`, `--file`, `--bind-data`, `--die-with-parent`, `--new-session`,
     `--as-pid-1`; namespace and `--seccomp` options are accepted and ignored).
   - It builds a private directory under `/tmp/renguin-bwrap-<pid>/` with a symlink or a
     copied file for each mount, then `chroot()`s into it and `execve()`s the command.
   - Binds become symlinks to the host paths. That works because absolute targets under
     `/boot/` are host paths.
   - Sandboxing is not provided. The shim says so on stderr once per launch, and the
     Renguin Manager shows it.
   - Tests: a shell script under the harness runs the shim with Flatpak's own argument
     list for a runtime/app pair laid out by hand.
3. **Flatpak and OSTree in the Linux root**: Alpine's `flatpak`, `ostree` and their
   libraries, added to the root's pins (about 15 MB). Alpine's real `bwrap` is not
   installed; the shim takes its path.
4. **Flathub remote and a test application.**
   - `flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo`
     runs when the Renguin Manager switch "Flatpak" is first turned on (off by default:
     it downloads runtimes of several hundred MB).
   - The harness has no network. Its test application is a tiny OSTree repository built on
     the build host from a static "hello" program and a minimal runtime, as `--linux-root`
     builds the Linux root today. Building it needs `ostree` on the build host (not
     installed on the cloud host, hence not done in batch 13).
   - Tests: `flatpak install --noninteractive` from that repository, `flatpak run`, and
     the program's output.
5. **GUI path.**
   - Renguin Manager, "Linux programs" tab: a Flatpak switch, a Flathub search field
     (`flatpak search`), Install and Remove, and the installed list. Installed programs'
     `.desktop` files go to `~/.local/share/flatpak/exports/share/applications`. linux-apps
     reads that directory as well, so they get Deskbar entries.
   - Renga Depot: the "Flatpak" source of the catalogue (decisions log, 2026-11-21) uses
     the same commands.
   - Both switchable.

### Step 1 as built (batch 14): what chroot() contains

Items 1, 2 and the switch of item 5 are in `batch/14-flatpak-step1` (QEMU Tested). The root is a
view for the shim, not a sandbox (maintainer, 2026-11-27: step 1 unsandboxed).

- **Kept as a path, not a vnode.** The root is a canonical host path in the team's Renguin
  data. Nothing is pinned or released: `fork()` copies it with the team data, team exit
  frees it with the data. If the directory is removed, lookups under it fail.
- **Across exec.** `execve()` records the root on a list keyed by team, and the new
  program's load takes it. The list has no fixed size, so no root is dropped to make room.
  `execve()` fails with `ENOMEM` rather than lose a root. A failed `execve()` leaves the
  caller in its root. The probe marks the entry when the new program is a Linux one; the
  release before the load frees an unmarked entry, which is the native-exec case (the team
  leaves the personality, and team exit will not call Renguin again) and the dying case.
  `renguin_exec_keep_root()` also frees entries of teams that no longer exist. So entries
  live only during an exec. A native program, and any Linux program it starts, is outside
  the chroot. `/proc/self/status` shows the system-wide count as `RengaExecRoots`.
- **Contained:** absolute paths, `..` in them (it stops at the root), and absolute and
  relative link targets other than the two host prefixes below. `getcwd()` is relative to
  the root. The interpreter of a dynamic program is looked up in the root.
- **Not contained, by design:**
  - absolute link targets under `/boot/system/` and `/boot/home/`, which are host paths
    (the shim's binds are such links);
  - absolute paths that do not exist in the root, which fall through to Haiku's tree, as in
    the Linux root;
  - relative paths, which Haiku resolves. `..` from a working directory at the root
    leaves it. A directory opened before `chroot()` stays usable with `fchdir()` and
    relative paths, as on Linux.
- **Permissions:** any program may call it (Linux asks for `CAP_SYS_CHROOT`; the shim
  runs as the user). The caller needs search permission on the directory (`EACCES`).
  `ENOENT`, `ENOTDIR` and `EFAULT` are as on Linux.
- **Tests:** `libc-chroot` in the `renguin` suite covers escapes by `..` and by symbolic
  links, `fchdir()`, `fork()` (two levels), `exec()` (two levels, and a failed exec), and
  200 native execs from inside the root that must leave no entry behind, no entry while a
  native program from the root runs, and a second thread's failed `execve()` during the main
  thread's exec (only the main thread may exec; the second thread is answered before it can
  touch the main thread's entry).
  `renguin.flatpak.*` covers the shim. `renguin_manager.flatpak_switch` covers the switch.

### Step 1, part 2 as built (batch 14): a real install without a network

- **Flatpak layer** of the harness root (`root/flatpak.pins`, `root/build-flatpak-root.sh`;
  every harness image, about 30 MB): Alpine's `flatpak` 1.16 and `ostree` with the libraries
  they load. Not included: bubblewrap (the shim replaces it), `xdg-dbus-proxy`, `fuse3`, the gpg programs and the
  trigger tools.
- **Test repository** `/opt/renguin-data/flatpak/repo`, built on the build host with the pinned
  flatpak itself, run through Alpine's musl loader (the build host needs no `ostree`):
  - `runtime/org.renga.TestPlatform/x86_64/1`: static busybox as `/usr/bin/sh`, and an
    `ldconfig` that writes the empty `ld.so.cache` `flatpak run` asks for;
  - `app/org.renga.Hello/x86_64/1`: a static program that reports its Flatpak ID, `/app`, `/usr`
    and `/.flatpak-info`.
- **Shim changes found on the way:**
  - Flatpak 1.16 binds `/usr`, `/app` and the app's data directories with `--ro-bind-fd` and
    `--bind-fd`. The shim names the directory through `/proc/self/fd`.
  - `/tmp` and `/var/tmp` sources map to Haiku's `/tmp`.
  - A bind, file or directory placed inside an earlier bind is skipped. The first shim made it
    through that bind's link, in the bound directory itself.
  - `--ro-bind / /` (install triggers) becomes a link for each top-level entry.
  - Destinations under `/boot` are skipped: the private root never has a `/boot`, so Renguin
    resolves them in Haiku's tree, where Flatpak's same-path binds of the home already are.
  - `/proc/self/fd` is read with `busybox readlink` as a program. ash's in-process readlink,
    in the forked subshell of `$(...)`, gets `ENOENT` under Renguin (open, not investigated).
- **Renguin changes found on the way** (kernel module):
  - `statx()` reports a mount ID (`STATX_MNT_ID`, and `STATX_MNT_ID_UNIQUE` when asked): the
    volume's `dev_t` + 1. libglnx fails with `ENODATA` without one. `/proc/self/mountinfo`
    uses the same IDs. Its Linux-root line takes 1: Haiku numbers volumes from 1, so no
    volume's ID is 1. Files in the root report their volume's ID, the mount they are on in
    Haiku.
  - A relative path from a descriptor of the team's `/` resolves like the absolute path
    (`openat()`, `mkdirat()`, `fstatat()`, `readlinkat()` and the other `*at()` calls).
    libglnx's `glnx_chaseat()` walks paths that way. The root's dev/ino is kept in the team
    data (set at load and at `chroot()`, copied on `fork()`), so the test costs one `fstat`.
  - No call creates a top-level `boot` in the Linux root (`renguin_creates_shadow()`):
    - `mkdir`, `symlink`, `mknod`, `link` and `rename` give `EEXIST`, and `open(O_CREAT)`
      gives `EISDIR`, as for Haiku's `/boot`.
    - The check is on the parent directory's identity (the Linux root's dev/ino). It covers
      every route: an absolute path, a descriptor of `/`, the working directory at `/`, and
      `..` walks.
    - Why only `boot`: Haiku's `/boot` is reached only because the root has none, and a
      root of its own would hide Haiku's volume from every Linux program.
    - `/tmp` and `/var/tmp` are kept shared by `linux` itself (`ensure_shared_tmp()`).
      Programs may make other top-level directories, as on Linux.
    - A `chroot()` root is a private tree and is not checked.
    - Not covered: binding an `AF_UNIX` socket named `/boot`.
  - Without the last two, Flatpak made a `/boot/home` of the root's own, hiding Haiku's.
- **Known limits of the shim**, acceptable only because it gives no isolation (step 1 is
  unsandboxed):
  - `--bind-fd` and `--ro-bind-fd` are resolved through `/proc/self/fd` to a name and bound
    by that name. That brings back the name race that binding by descriptor exists to close
    (CVE-2024-42472).
  - Masks are dropped: a `--tmpfs` (or `--dir`, `--file`) inside another bind, or under
    `/boot`, is skipped. What Flatpak meant to hide stays visible.
- **Tests:**
  - `renguin.flatpak.ostree_install` and `renguin.flatpak.ostree_app` (`flatpak --user` with
    its own `FLATPAK_USER_DIR`), and `renguin.flatpak.bwrap_nested`.
  - In `renguin.libc-fs`: the `statx` mount ID; mountinfo lines with those IDs; `mkdir /boot`
    absolute, from a descriptor of `/`, and from the working directory at `/` (also through
    `etc/..`).
  - In `renguin.libc-chroot`: the `create` part (`mkdir /etc` in an empty chroot).
- **Still open:** the session bus. `flatpak run` puts `xdg-dbus-proxy` between the program and
  the bus, so the test runs with `--no-session-bus --no-documents-portal`. The proxy and its
  sync descriptor under the shim come next, then the Flathub remote (step 4) and the GUI
  (step 5).

### Step 2: sandboxed

Needs user and mount namespaces in Renguin (a per-team mount table over Haiku's VFS), or a
Haiku-native sandbox the shim maps onto. Not before step 1 works on the maintainer's machine.

Notes for step 2 (from the step 1 review):
- Flatpak front ends (Flatseal, GNOME Software, `flatpak info --show-permissions`) still list
  an application's permissions as if bwrap enforced them. Under step 1 nothing is
  enforced. The user documentation must say so, and the Renguin Manager's switch text
  already does.
- `chroot()` without `CAP_SYS_CHROOT`, together with set-uid programs honoured on exec, is
  the classic chroot privilege trick: a user builds a root with their own `/etc/passwd` or
  libraries, and a set-uid program in it runs with them. Renguin's IDs are a view today, but
  before step 2 (or any real set-uid support) `execve()` inside a chroot must ignore set-uid
  and set-gid bits, or `chroot()` must require privilege.

## Steam

The Steam client is 32-bit (`steam`, `steamwebhelper` is 64-bit Chromium). Its runtime
("Steam Linux Runtime", pressure-vessel) starts games in containers through `bwrap`.

What is missing, in order:

1. **32-bit Linux programs (i386).**
   - Haiku's x86_64 kernel has an ia32 compatibility mode (`_COMPAT_MODE`,
     `entry_compat.S`, `signals_compat.cpp`). It is built only when the image has an x86
     secondary architecture (`build/jam/ArchitectureRules`).
   - Renguin needs, on top of that:
     - an ELF32 loader path
     - the i386 system call entry (`int 0x80` and `sysenter`/`syscall` from 32-bit code
       segments), with i386 syscall numbers and structure layouts (`stat64`, `iovec`,
       `msghdr`, signal frames, `ucontext`)
     - `set_thread_area`/`modify_ldt` for 32-bit TLS
     - 32-bit `mmap` limits (below 4 GB)
   - Design and kernel work: XL. It needs the architect and a kernel brief.
2. **Multilib Linux root.** Alpine has no 32-bit packages on x86_64. Steam needs a
   glibc-based root with i386 and x86_64 libraries (Debian or Ubuntu `multiarch`, or Arch
   `lib32-*`). Renguin runs static glibc programs today; dynamic glibc needs the glibc
   `ld-linux` loader paths, `/etc/ld.so.cache` and the glibc-specific system calls
   (`rseq`, `clone3`) checked. The roadmap's Debian/Arch roots item (`F-27`) covers it.
3. **Libraries Steam needs.**
   - 32-bit and 64-bit: libc6, libstdc++6, libgcc, zlib, libX11 and XWayland (Steam is an
     X11 program; Renguin has no XWayland yet), libGL/Mesa (llvmpipe until the GPU
     stage), libvulkan, libdbus, libnss3, libcurl, libudev (stub), libpulse (until the
     PipeWire/Pulse bridge), libgtk2 (the client's tray), libva.
   - `steamwebhelper` is Chromium (CEF): it needs the same work as Linux Chromium
     (zygote, GPU process, shared memory growth).
4. **Containers.** pressure-vessel and the runtime's `bwrap`: Flatpak step 1 (`chroot`
   and the shim) first. With `STEAM_RUNTIME=0` and `PRESSURE_VESSEL` off, games can start
   without them.
5. **Kernel interfaces games use.**
   - `/dev/input/event*` (done, evdev bridge)
   - `/dev/uinput` (done)
   - `/dev/dri` render nodes (with the GPU stage, Quasar)
   - `/sys/class/input` and udev enumeration (open)
   - `futex_waitv` (Proton's fsync; open)
   - `ptrace` (not needed by Steam itself)

First useful milestone: a 64-bit native Linux game from Steam's files, started outside the
client with the runtime's libraries, under Renguin with a glibc root. It needs items 2 and 3
(64-bit only), XWayland, and Mesa. The client itself needs item 1.
