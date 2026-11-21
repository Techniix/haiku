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
- Packages are built **natively on Renga** (dual boot or a Renga VM) with HaikuPorter, for example
  `haikuporter -S mesa`, `haikuporter -S haikuwebkit`. They are not built on the Linux host.
- `tools/update-repo.sh <package>.hpkg ...` (in RengaPorts) adds them, rebuilds the repository index and
  publishes it as a single commit on the `packages` branch, which GitHub Pages serves.
- Renga builds of packages HaikuPorts also has keep HaikuPorts' version with a revision from 100 up;
  every RengaPorts package requires `renga >= 1`.
- Outstanding maintainer builds (hand-over batch 8): `mesa-26.3.0` revision 100 and `haikuwebkit-1.10.0`
  revision 101; then `media_html5_test` (renga-raw) with the new haikuwebkit.

## Revision and hrev tags

The "Renga hrev tag" workflow (`.github/workflows/ce-hrev-tag.yml`) pushes the nearest upstream `hrev` tag
on every push to `ce/main` (roadmap [F-21](../../RENGA_MASTER_ROADMAP.md#f-21-hrev-tag-workflow-and-branch-model)).
Images carry a revision such as `hrev60381+427` that names the Renga commit.
