# Partner ports

Load this when a task reads a partner's requests or writes a partner note. Partner ports are separate
projects that build on Renga and ask it for interfaces. They write requests; Renga answers in dated notes.

| Partner | Repository | Their requests | Renga's notes | Roadmap |
| --- | --- | --- | --- | --- |
| Quasar (NVIDIA driver) | `Techniix/nvidia-haiku`, branch `renga` | `RENGA-REQUESTS.md` in that repository | [docs/partners/QUASAR.md](../partners/QUASAR.md) | [F-35](../../RENGA_MASTER_ROADMAP.md#f-35-gpu-and-graphics), [F-47](../../RENGA_MASTER_ROADMAP.md#f-47-zero-copy-presentation-and-a-haiku-vulkan-surface), [F-58](../../RENGA_MASTER_ROADMAP.md#f-58-partner-requests-batch-1-obs-studio-and-quasar) |
| Chromium | `Techniix/Chromium-HaikuOS`, branch `renga` | `RENGA-REQUESTS.md` at the top of that repository | [docs/partners/CHROMIUM.md](../partners/CHROMIUM.md) | [F-43](../../RENGA_MASTER_ROADMAP.md#f-43-chromium-platform-readiness-native-fully-featured) |
| OBS Studio | `Techniix/OBS-Studio_HaikuOS` | `RENGA-REQUESTS.md` in that repository | [docs/partners/OBS.md](../partners/OBS.md) | [F-58](../../RENGA_MASTER_ROADMAP.md#f-58-partner-requests-batch-1-obs-studio-and-quasar) |

Port-visible interfaces, their exact semantics and limits are in
[3rdparty/ce/PORTING.md](../../3rdparty/ce/PORTING.md); notes link there instead of repeating it.

## Reading requests

- Fetch only the partner's `RENGA-REQUESTS.md` (for example with `gh api` or a shallow clone of that one
  file), not the whole repository. Requests are numbered per partner (Quasar `R-n`, OBS `O-n`, Chromium
  `R-n`); refer to them by number.
- Compare with the newest entries of the matching `docs/partners/*.md` to see what is already answered.
- A new request is a proposal for the maintainer: present it with a recommendation; once he decides, record
  the decision (dated) in the roadmap's decisions log and place the work in the work order.
- When an entry lands in a partner's file, tell the maintainer so he can point that partner's session at it.

## Writing a note

- Each note file starts with a **Status** line (READY when the "Latest" section is final for this round;
  IN PROGRESS while Renga is still working) and an **Updated** timestamp in UTC. Keep both current.
- Entries are dated headings, newest first, under "Latest": `### <YYYY-MM-DD>: <subject>`.
- Each entry is short (a line or two per point): what landed or was decided, the commit hash, the image
  that carries it, and what the partner should do next. Detail belongs in commit messages and PORTING.md.
- Results are labelled QEMU Tested or Hardware Tested, never mixed.
- Notes-only commits (`docs/partners/`) do not change the gate tree key, so they never force a re-run.
