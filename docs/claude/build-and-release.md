# Building and releasing Renga

Load this when a task configures, builds, makes an image, delivers a build or builds packages. Sources:
[ReadMe.Compiling.md](../../ReadMe.Compiling.md) (upstream), [3rdparty/ce/README.md](../../3rdparty/ce/README.md)
("Renga images"), the roadmap's [F-18](../../RENGA_MASTER_ROADMAP.md#f-18-build-profiles-and-images) and
[F-45](../../RENGA_MASTER_ROADMAP.md#f-45-rengaports-separate-ports-and-package-repository-for-renga).

## Configure and build

Layout: the Haiku build tools checkout (`buildtools`) sits next to this tree, as in ReadMe.Compiling.md;
the output directory is `generated.x86_64` inside the tree. `tools/renga/renga-build` wraps these steps:

```sh
# jam, once, if it is not installed (ReadMe.Compiling.md, "Building Jam"):
cd ../buildtools/jam && make && ./jam0 -sBINDIR=$HOME/bin install
# configure, once (builds the cross-tools; 1-2 hours the first time):
mkdir -p generated.x86_64 && cd generated.x86_64
../configure --cross-tools-source ../../buildtools --build-cross-tools x86_64 -j$(nproc)
# build, from generated.x86_64; HAIKU_INCLUDE_3RDPARTY=1 picks up 3rdparty/ce:
jam -q -j$(nproc) -sHAIKU_INCLUDE_3RDPARTY=1 @renga-raw      # an image
jam -q -j$(nproc) -sHAIKU_INCLUDE_3RDPARTY=1 HaikuDepot      # one target
```

`configure` needs to run again only when it or the cross-tools change. Builds are incremental.

## Build profiles

Defined in [3rdparty/ce/build/RengaBuildProfiles](../../3rdparty/ce/build/RengaBuildProfiles):

| Profile | Image | Use |
| --- | --- | --- |
| `renga-raw` | `generated.x86_64/renga.image` | Full desktop disk image; the harness's Renga profile |
| `renga-anyboot` | `generated.x86_64/renga-anyboot.iso` | CD, USB and UEFI; handed to the maintainer |
| `renga-vmware` | `generated.x86_64/renga.vmdk` | VMware |
| `minimum-raw`, `minimum-anyboot` | upstream | Harness defaults (no desktop extras) |

A Renga image adds the volume label "Renga", the Techniix (Renga) package repository with priority over
HaikuPorts, and `HAIKU_RENGA_EXTRA_PACKAGES`.

## Images for the maintainer

1. Run the four-way sweep on the final tree (`tools/renga/renga-gate`); record its numbers.
2. `tools/renga/renga-image <name> --sweep "<gate summary line>"`. It:
   - runs `3rdparty/ce/build/build-handover-image renga-anyboot`, which **refuses a tree that is not
     clean** (the image must name exactly one commit) and determines the revision afresh;
   - in the renga-builds clone (default `../renga-builds`): updates `main` and cuts `build/<name>` from it
     (main carries `tools/join.sh` and the Publish workflow);
   - splits the ISO with `split -b 95M -d -a 3` into `parts/<iso>.partNNN` (GitHub's 100 MB file limit),
     writes `<iso>.sha256` and `build.txt`;
   - commits with explicit paths, pushes only with `--push`, and switches back to `main`.
3. `build.txt`'s **first line must be `iso: <iso>`**, with `<iso>` = `renga-<name>-anyboot.iso` (for
   example `iso: renga-2026.11.21-batch11-anyboot.iso`). The Publish workflow fails with "build.txt names no
   iso" without it. Names are `<YYYY.MM.DD>-<topic>`; the script adds the date if it is missing.
4. The maintainer publishes it: renga-builds → Actions → **"Publish build"** → the build name. The workflow
   joins the parts, checks the sha256, creates a pre-release `renga-<name>` and by default deletes the
   branch.
5. Report the delivery with its name, sha256, the Publish step and a hardware test list (hand-over).

Locally, an image can be built and tested without delivering it: `renga-build --image renga-anyboot`.

## Packages: RengaPorts and HaikuPorter on Renga

- `Techniix/RengaPorts` holds HaikuPorter recipes for software built for Renga (programs that use Renga's
  own APIs) and the Renga package repository. Its README and `docs/PORTING-TO-RENGA.md` are the reference.
- Packages are built **natively on Renga** (dual boot, or the build VM below) with HaikuPorter, for example
  `haikuporter -S mesa`, `haikuporter -S haikuwebkit`. They are not built on the Linux host.
- `tools/update-repo.sh <package>.hpkg ...` (in RengaPorts) adds them, rebuilds the repository index and
  publishes it as a single commit on the `packages` branch, which GitHub Pages serves.
- Renga builds of packages HaikuPorts also has keep HaikuPorts' version with a revision from 100 up;
  every RengaPorts package requires `renga >= 1`.
- Outstanding maintainer builds (hand-over batch 8): `mesa-26.3.0` revision 100 and `haikuwebkit-1.10.0`
  revision 101; then `media_html5_test` (renga-raw) with the new haikuwebkit.

## Native builds in a Renga VM (KVM)

The standard way to build a package that must be built on Renga (maintainer, 2026-11-27): HaikuPorter
inside a Renga VM under QEMU+KVM on the Linux host, driven headless over SSH with
[`tools/renga/renga-buildvm`](../../tools/renga/renga-buildvm) (`--help` lists every command).

Sizes: 8 vCPUs and 12 GB RAM by default (`CPUS`, `MEMORY`; leave room for the harness on the same host).
The system disk is a copy of a `renga-raw` image: its 1.4 GB BFS volume had about 650 MB free, and about
150 MB after HaikuPorter, git, the haikuwebkit build dependencies and ffmpeg's tools were installed. A
sparse 120 GB qcow2 data disk (`DATA_SIZE`) holds the BFS volume "Ports" at `/Ports`: the ports trees
(`~/ports` links to `/Ports/ports`), HaikuPorter's work directories, `TMPDIR` and a 16 GB swap file (6 GB
allocated after the haikuwebkit build). Networking is QEMU's user mode; only SSH is forwarded
(`127.0.0.1:2222`). Every drive is a file in the VM directory (`RENGA_BUILDVM_DIR`, default `../buildvm`
next to the tree; on the maintainer's host `/home/Techniix/renga/buildvm/vm`).

```sh
# once (any recent renga-raw build; a harness image is fine)
tools/renga/renga-build --image renga-raw
tools/renga/renga-buildvm prepare generated.x86_64/renga.image
tools/renga/renga-buildvm start      # boots, waits for SSH
tools/renga/renga-buildvm setup      # "Ports" volume, swap settings, /Ports/build.sh
tools/renga/renga-buildvm ssh 'echo n | pkgman install haikuporter git'   # read the plan first
tools/renga/renga-buildvm ssh 'pkgman install -y haikuporter git'
tools/renga/renga-buildvm ssh 'cd ~/ports && git clone -q --depth 1 https://github.com/haikuports/haikuports.git &&
  git clone -q https://github.com/Techniix/RengaPorts.git'
tools/renga/renga-buildvm ssh 'cat > ~/config/settings/haikuports.conf' <<'EOF'
TREE_PATH="/boot/home/ports/haikuports"
PACKAGER="Techniix <Techniix@users.noreply.github.com>"
VENDOR="Techniix"
ALLOW_UNTESTED="yes"
TARGET_ARCHITECTURE="x86_64"
EOF
tools/renga/renga-buildvm stop && tools/renga/renga-buildvm start   # swap moves to /Ports
```

`prepare` removes the harness job of a harness image (it would run the suites and power off), puts the
VM's SSH key (`ssh/id_ed25519` in the VM directory) and SSH host keys into the image (the openssh
package's post-install script does not finish on a first boot without a user session, so sshd would
never start), and adds a boot job that mounts "Ports" and writes `BUILDVM-READY`, the network setup and
the volumes to `serial.log`. The root user is `baron`, home `/boot/home`. An SSH session has no
`LIBRARY_PATH`: set the whole list when overriding it (see the test below). Check `pkgman`'s plan before
applying it: the image also lists upstream Haiku's repository, and a plan that replaces the `haiku`
package must be refused.

A build (the maintainer's form, relay M-1), started detached in the guest so that a dropped SSH
connection does not stop it; wait on the host for the status file:

```sh
tools/renga/renga-buildvm ssh 'cd ~/ports/RengaPorts && git fetch -q origin && git checkout -q <branch> &&
  cp -r <category>/<port>/. ../haikuports/<category>/<port>/ &&
  rm -rf ../haikuports/<category>/<port>/work-<version>'
tools/renga/renga-buildvm ssh 'nohup /Ports/build.sh <port> -j8 --get-dependencies <port>-<version> >/dev/null 2>&1 &'
until tools/renga/renga-buildvm ssh 'test -e /Ports/<port>.status'; do sleep 300; done
tools/renga/renga-buildvm ssh 'cat /Ports/<port>.status; tail -n 30 /Ports/<port>.log'
tools/renga/renga-buildvm get /boot/home/ports/haikuports/packages/<package>.hpkg <host dir>
```

`/Ports/build.sh <name> <haikuporter args>` sets `TMPDIR=/Ports/tmp`, runs HaikuPorter in
`~/ports/haikuports`, logs to `/Ports/<name>.log` (start and end times in seconds) and writes the exit
status to `/Ports/<name>.status`. If HaikuPorter dies after "cleaning chroot folder" (see below), the
finished packages are in `<category>/<port>/work-<version>/hpkgs/`; check them with `package list` and
copy them to `packages/`.

Testing in the VM: extract the package (the directory must exist), quit the program, and start it with
the extracted libraries first; `listimage <team>` shows which libraries it loaded:

```sh
mkdir -p ~/<name>test && package extract -C ~/<name>test <hpkg>
hey WebPositive quit
LIBRARY_PATH=/boot/home/<name>test/lib:%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib \
  /boot/system/apps/WebPositive http://127.0.0.1:8000/index.html &
```
Serve pages with `python3 -m http.server` in the guest.
`renga-buildvm screendump shot.png` takes a screenshot (QEMU monitor); the guest's sound is recorded to
`audio.wav` in the VM directory (virtio-sound, written only while a stream plays). The screen saver
blanks the display after a long build: run `/boot/system/non-packaged/bin/ce_screensaver_off` (harness
images) or move the pointer (`renga-buildvm monitor "mouse_move 20 20"`).

Timings (QEMU, Ryzen 5 7600X host, 8 vCPUs, 12 GB): boot to SSH about 10 s, shutdown about 3 s;
`pkgman install haikuporter git` 25 s; HaikuPorter's first run re-indexes the haikuports tree in about
2 minutes; haikuwebkit 1.10.0 (7207 ninja steps, `-j8`) 4 h 55 min from start to packages.

Known problems (2026-11-27):
- The system volume cannot be grown: `resizefs /boot` reports that the disk system does not support
  resizing, and BFS's `BFS_IOCTL_RESIZE` back door panics the kernel (`ASSERT FAILED Journal.h:31`, the
  journal lock is not held). With about 150 MB free, larger builds (Mesa) need a larger image.
- HaikuPorter (python3.14 3.14.10) crashed with a segment violation in `_Py_type_getattro` right after
  "cleaning chroot folder" of the haikuwebkit build (exit status 149; the harness image's debug_server
  setting kills crashing programs), before moving the packages; both packages were complete.

## Revision and hrev tags

The "Renga hrev tag" workflow (`.github/workflows/ce-hrev-tag.yml`) pushes the nearest upstream `hrev` tag
on every push to `ce/main` (roadmap [F-21](../../RENGA_MASTER_ROADMAP.md#f-21-hrev-tag-workflow-and-branch-model)).
Images carry a revision such as `hrev60381+427` that names the Renga commit.
