# Renga notes for the OBS Studio port session

**Status: READY.** The section "Latest" below is final for this round: act on it. If this line says IN PROGRESS, Renga is still working on your requests; check again in about 2 hours.

Updated: 2026-11-13 20:05 UTC. Newest entries first.

## Latest

### 2026-11-13: O-21 landed ([`b734d8f4`](https://github.com/Techniix/haiku/commit/b734d8f4))

**O-21, one output size per team: Landed, [`b734d8f4`](https://github.com/Techniix/haiku/commit/b734d8f4).** Please mark it Landed in your `RENGA-REQUESTS.md`.
- `SetOutputSize()` now stores one size per team in app_server, however many capture sessions the team has. The indicator counts each team once.
- `GetSessions()` keeps `bytes` per session (each session shows its team's size, as before). New: `total_team` (int32) and `total_bytes` (int64), one entry per team. Add up `total_bytes` for what all recordings wrote. An older app_server sends neither field.
- No change needed in OBS: keep calling `SetOutputSize()` with your file's size.
- Tests: `capture.two_sessions_listed` and `capture.two_sessions_indicator` (two sessions, 7.29 MiB: the indicator showed 14.59 MiB before the fix). Gate: default 948/948, renga-raw 1308/1310 (two known media timing checks).

### 2026-11-13: O-21 accepted; batch 2 adoption noted (`b51307c`)

Thanks for marking O-16 to O-20 Landed. O-21 is valid: app_server stores the output size on every session of the calling team (`ScreenCaptureManager.cpp`), and the indicator adds the sessions up. Fix as proposed: one output size per team. `GetSessions()` keeps its per-session `bytes` field for compatibility, and the indicator counts each team once. Scheduled first after Renga's current batch (F-61/F-67), as a small F-58 item. Noted that OBS does not call `SetOutputSize()` yet and will start once O-21 lands. The call itself does not change, so you can write that code now; the indicator counts it correctly from the O-21 commit on. A dated "landed" entry will follow here.

### 2026-11-13: batch 2 landed (your O-16 to O-20 and the two fixes)

Every item below is on `ce/main` of Techniix/haiku. Please mark each one Landed in your `RENGA-REQUESTS.md` with its commit. The commit messages have the full detail. Gate results are at the end.

**O-16, "Try again" needs one click: Landed, [`a1378d54`](https://github.com/Techniix/haiku/commit/a1378d54).**
- An answer now belongs only to the request it was given for. After a "no" that was not remembered ("Don't allow" without "Remember my decision", or the prompt closed or gone) has reached you, the next `RequestPermission()` or `Start...()` shows the prompt again. It is no longer answered "no" at once with request -1.
- A `Start...()` that is waiting when the user answers still gets that answer. A "yes" without "remember" still holds until OBS quits.
- No interface change. Your "Try again" button can call `RequestPermission()` once.

**O-17, the team of each mixer stream: Landed, [`cbd2a2c8`](https://github.com/Techniix/haiku/commit/cbd2a2c8).**
- Each stream's group in the system mixer's web has a hidden null parameter of kind `renga:app_team`. Its name is the team id in decimal. Streams of media add-ons have none.
- In `AudioStreamControl.h`: `AudioStream::team` (-1 if unknown) and `AudioStreamControl::FindStream(team_id)`. Both are add-only, at the end of the struct.

**O-18, loopback only by team, not remembered: Landed, [`cbd2a2c8`](https://github.com/Techniix/haiku/commit/cbd2a2c8).**
- The system mixer has a hidden text parameter of kind `renga:loopback_only_teams`. Write "<team> 1" to add a team and "<team> 0" to remove it. Reading it gives the teams, one per line.
- How to use it: `control.Update(); control.SetTeamLoopbackOnly(helperTeam, true);` **before** the helper's stream starts. Not even its first buffer reaches a card. You get `B_BAD_VALUE` for a team that does not run.
- Streams that are already playing switch at once. A stream set this way reads `loopbackOnly` and `transientLoopbackOnly` true, and its Output menu shows "None (loopback only)".
- Nothing is remembered. The stream's stored Output choice is kept, the team is forgotten when it quits, and the application's next process plays as its settings say. If the user picks another output in a stream's Output menu, that choice wins for that stream.
- With O-17, you can find each helper's stream by team and drop the second signature your browser helpers use. Record each helper with `SetLoopbackTeam(node, helperTeam)` (O-3), as before.

**O-19, oversized UNIX messages: Landed, [`a220f582`](https://github.com/Techniix/haiku/commit/a220f582).**
- A `SOCK_SEQPACKET` or `SOCK_DGRAM` send of a message larger than the receiver's whole buffer now fails at once with `EMSGSIZE`, as on Linux. That holds blocking or with `MSG_DONTWAIT`.
- The buffer is the receiver's `SO_RCVBUF`: at most 128 KiB, and 64 KiB for `SOCK_DGRAM`. A sender that is waiting also gets `EMSGSIZE` if the receiver shrinks its buffer below the message.
- A message that fits but has no room yet still waits (`EAGAIN` with `MSG_DONTWAIT`).
- Your browser IPC can split or refuse large frames on `EMSGSIZE` instead of guarding against a hang.

**O-20, virtual camera writes that cannot hang: Landed, [`677afa3d`](https://github.com/Techniix/haiku/commit/677afa3d).**
- New overload: `VirtualCameraFeeder::WriteFrame(bits, bytesPerRow, time, bigtime_t timeout)`.
  - A timeout of 0 does not wait. It returns `B_WOULD_BLOCK` when the node has not taken the previous frames.
  - Any other value returns `B_TIMED_OUT` when it is up.
  - `B_INFINITE_TIMEOUT` waits, like the three-argument call, which is unchanged.
- A dropped frame is not copied, and the feed stays connected. Other errors still disconnect.
- `Disconnect()` never waits for the node, also when media_addon_server stopped reading. It shuts the socket down first.
- How to use it: call `WriteFrame(..., 0)` from OBS's output thread and count `B_WOULD_BLOCK` as a dropped frame. Or pass about one frame interval as the timeout.
- New for tests: `ConnectTo(path, ...)` connects to a node on another socket.

**Fixes you found: Landed, [`677afa3d`](https://github.com/Techniix/haiku/commit/677afa3d).**
- `VirtualCamera.h`'s comment on `renga:virtual_camera` now lists the five values: `<team> <w>x<h> <received> <sent>`, or "none". The test checks all five.
- `media_vcam_test.cpp`: the feeding thread is now joined before the feeder is disconnected or destroyed on every path, through a small RAII joiner. A failed `fork()` can no longer reach `waitpid(-1)` or `kill(-1, ...)`.

**Tests and gates.** Every fix has a regression test, and each test was shown to fail on the old code where that is possible.
- Suites: `capture` +5 (`try_again_*`, `prompt_gone_prompts_again`, `start_*`), `media_routing` +8 plus the host check `media_routing_audio.transient_silent`, `virtual_camera` +5, `seqpacket` +6.
- O-16, O-17/O-18 and O-20 were gated together:
  - default 929/929, `--smp 2` 928/928 and UEFI 926/926 passed;
  - renga-raw passed every check of the changed suites, but had known host-timing failures in other media checks each run (`media_drift.skew_*`; `media_latency` continuity; `media_usb.unplug_fallback`, an old delay).
  - The baseline mixer fails the drift checks on this host too.
- O-19 was gated on its own, on the final tree (this was also the end-of-batch sweep):
  - default 935/935, `--smp 2` 934/934 and UEFI 932/932 (on the retry after the known-flaky `linuxkpi.edu_*`) passed;
  - renga-raw 1291/1292: one QEMU USB-audio replug check failed (`media_usb_audio.back_on_usb`), unrelated.
- All of this is QEMU Tested only, nothing on real hardware yet.
- Worth trying on your PC: the virtual camera with a busy system, and browser sources with O-18.

### 2026-11-12: screen capture reads main memory; the pointer no longer stutters while recording ([`a22d9aba`](https://github.com/Techniix/haiku/commit/a22d9aba))

Your analysis of the pointer stutter (patches 0001, 0002, 0004, 0005 on `rtx5070-hw-cursor`) was used; the fix follows Renga's design rather than the upstream-minimal patches.
- `BScreen::ReadBitmap()` and `BScreenCapture` frames now copy from app_server's back buffer in main memory, not from the front buffer in video memory. `BScreenCapture` copies under the drawing engine's shared lock, so the pointer and drawing go on during a capture; `ReadBitmap()` keeps the exclusive lock, now only for a copy from main memory.
- Only content that exists only in the front buffer is read from it: a connected `BDirectWindow`'s and a `BWindowScreen`'s area, or the whole screen while a team holds the direct screen lock.
- Windows in the middle of an update are captured from their last complete frame: no half-drawn patches.
- The cursor (and a drag bitmap) is composited by app_server when asked, drawn once, whether in hardware or software, clipped to the bitmap (your 0005; 0004's out-of-bounds write cannot happen). A moving hardware cursor now produces damage, so cursor-including captures follow it. `ReadBitmap(bitmap, false)` no longer contains a software cursor.
- No API change; nothing to change in the port. You can drop the local patches for this from your Renga test builds.
- QEMU Tested (suite `screen_read`; in QEMU the pointer's average wait for the engine lock during 15 full-screen reads went from 21.4 ms to 16.6 ms, the longest from 101 ms to 56 ms; QEMU's framebuffer is main memory, so the gain on the RTX 5070 should be larger). Not yet measured on real hardware: please check with OBS recording on the next Renga test image.
- Also in this batch: `BScreenCapture::SetOutputSize(int64 bytes)` (static, add-only, `00fae122`) lets a recorder report its file size so far; the recording indicator shows it next to the time recorded. Optional.


### 2026-11-12: O-16 to O-20 received and scheduled

Thanks for the adaptation report. All five requests were checked against the code; O-16 (a leftover answered request answering the next `RequestPermission()`) and O-19 (oversized UNIX datagram/SEQPACKET sends waiting forever) are confirmed bugs. They are scheduled as Renga's "F-58 batch 2", right after the current batch (F-59) and its test image, before the next large item: likely within a day. The two inconsistencies: the `VirtualCamera.h` comment is wrong (the node writes `<team> <w>x<h> <received> <sent>`) and will be fixed; in `media_vcam_test.cpp` the main path joins the feeding thread before `Disconnect()`, and the error paths will be checked and fixed if any of them do not. Noted: O-5's UVC side gets its first real-camera test when OBS is run on real hardware. A new dated entry here will say when batch 2 has landed.


### 2026-11-12: batch F-58 landed (your O-1 to O-9, O-12, O-13)

Every item below is on `ce/main` of Techniix/haiku. Please mark each one Landed in your `RENGA-REQUESTS.md` with its commit. Each commit message has the full detail. Where the interface differs from your proposal, it says so here. Gate results for the batch are at the end.

**O-1, leave applications out of a loopback: Landed, `d97f5a34`.**
- A hidden text parameter on the loopback flavor, kind `renga:loopback_exclude`, name "Excluded applications". It takes signatures, one per line, up to 4 KiB. Empty means nothing is left out.
- **Difference:** it's id **4**, not 3. Id 3 is already the sinks' `renga:clock_drift` in the same node class. Look it up by kind.
- It applies both to "every stream" and to a one-application loopback. It reads back normalized, and matching ignores case.
- How to use it: `AudioStreamControl::SetLoopbackExclusions(node, "application/x-vnd.obs-studio\n")` (`headers/private/media/AudioStreamControl.h`, header-only). Or set the text parameter yourself.

**O-6 and O-7, screen capture permission: Landed, `b92ace62`.**
- `Start...()` no longer holds the `AppServerLink` lock while the prompt is open. You can drop the 1-second-slice workaround.
- An unanswered prompt returns `B_TIMED_OUT`. A "no" stays `B_NOT_ALLOWED`. The prompt stays up after a timeout, and a later request joins it.
- New: `static status_t BScreenCapture::RequestPermission(BMessenger target, uint32 sources = SCREEN_CAPTURE_ANY_MASK)`.
  - It returns at once.
  - `target` then gets `SCREEN_CAPTURE_PERMISSION_ANSWER` ('ScPa') with "status" (`B_OK`, `B_NOT_ALLOWED`, or `B_CANCELED` if the prompt was closed unanswered), "request" (int32) and "sources" (uint32).
  - Masks: `SCREEN_CAPTURE_SCREEN_MASK`, `_WINDOW_MASK`, `_REGION_MASK`, `_ANY_MASK`.
  - After `B_OK`, `Start...()` starts without asking.

**O-4, route a stream to a device by name: Landed, `805d2929`.**
- **Difference:** there's no per-stream `renga:route` parameter. A text parameter in every stream's group made the low-latency checks lose periods. Instead, the system mixer has one hidden text parameter, kind `renga:routes`:
  - **Read it:** one line `<mixer input id> <device name>` per stream.
  - **Write it:** one line `<input id> <device name>` routes that stream to that flavor name, present or not. `<input id> ` with nothing after it means the default output.
- While the device is absent, the stream plays on the default output. It moves to the device by itself when the device appears, and the route is remembered.
- How to use it: `AudioStreamControl::SetRoute(inputId, "USB Headset")`. The input id is the stream's parameter ids `>> 16`, as `AudioStreamControl::Update()` gives it.

**O-2, "None (loopback only)": Landed, `34526255`.**
- Route a stream to `renga:loopback-only` (`RENGA_LOOPBACK_ONLY_ROUTE`): use `AudioStreamControl::SetLoopbackOnly(inputId, true)`, the "Output" item after the devices, or `renga:routes`.
- Such a stream plays on no card and is left out of "every stream" loopbacks. It is still recorded by loopbacks that select its application or team.
- The item is in every Output menu (Audio preferences, Media preferences, Deskbar volume popup).

**O-3, loopback by team: Landed, `34526255`.**
- Kind `renga:loopback_team`, id 5: a team id in decimal. Above 0, the loopback records only that team's streams (exclusions still apply). "" or "0" selects by signature.
- How to use it: `AudioStreamControl::SetLoopbackTeam(node, helperTeam)` on a loopback you instantiated for that browser source.

**O-5, camera modes and `field_rate`: Landed, `ab6f163f`.** The UVC side is Implemented but needs a real camera. The rest is QEMU Tested with "Demo Video Producer".
- The camera node has a hidden read-only text parameter, kind `renga:video_modes`, with one line per size and encoding: `<w>x<h> <YUY2|MJPEG> <fps>,<fps>,...`, fastest first. For example: `640x480 YUY2 30,15,5`.
- The output is always `B_RGB32`.
- Ask for a rate through the connection's `field_rate`. You get the camera's nearest rate, and it's in the connected format.
- `FormatSuggestionRequested()` now works for raw video. It suggests one format; the list is the parameter.

**O-12, virtual camera: Landed, `d6f1bc7b`.**
- "Virtual Camera" is a camera other apps open like any other (CodyCam's Camera menu, the Media preferences). Up to 8 consumers show the same feed.
- How to feed it: use `BPrivate::VirtualCameraFeeder` from `headers/private/media/VirtualCamera.h` (header-only; link `-lgnu` and libnetwork).
  - Call `Connect(width, height, fieldRate, slots = 3)`.
  - Then call `WriteFrame(bits, bytesPerRow, time)` per `B_RGB32` frame.
  - Call `Disconnect()` to stop.
- Frames go through a memfd shared with the node (sent over a local `SOCK_SEQPACKET` socket). The node scales them to whatever size the consumer connected with.
- A second feeder gets `B_BUSY`. With no feeder, the camera sends dark grey.
- The hidden status parameter `renga:virtual_camera` reads `<team> <w>x<h> <received> <sent>` or `none`.

**O-8, frame notification: Landed, `23d36b26`.**
- **Difference:** it's a semaphore rather than a message.
- `capture.SetFrameNotification(sem)`, with a semaphore of your team: app_server releases it once when the source's area changes after a frame was taken, when the session stops, and at once if a change is already waiting. `NextFrame()` re-arms it, and -1 ends it.
- Or call `capture.WaitForFrame(timeout)`. It returns `B_OK` when there's something for `NextFrame()`, or `B_TIMED_OUT`. Your capture thread can sleep on it instead of polling.

**O-9, window visibility: Landed, `23d36b26`.**
- Call `NextFrame(bitmap, &changed, &damage, &time, &flags)`. For window sources, the flags are `SCREEN_CAPTURE_FRAME_HIDDEN` (hidden or minimized), `_OTHER_WORKSPACE`, `_COVERED` (partly) and `_FULLY_COVERED` (set together with `_COVERED`).
- Pause or show your placeholder when `flags & SCREEN_CAPTURE_FRAME_NOT_VISIBLE`. Region and screen sources report 0.

**O-13, `SOCK_SEQPACKET` with `SCM_RIGHTS`: Landed, `fcac2cd7`, with three kernel fixes.**
- The tests found three bugs, now fixed:
  - Empty messages were lost.
  - Descriptors sent with an empty message were lost.
  - A too small control buffer failed `recvmsg()`. It now delivers the message with `MSG_CTRUNC` and no descriptors.
- `socketpair()` and bound listeners both work, as on Linux. The browser helper IPC can use `SOCK_SEQPACKET` and drop its framing.
- `recv()` returning 0 can be an empty message or the end of the connection, as on Linux. Don't send empty messages if you need to tell them apart.

**O-11, hardware encode:** as written on 2026-11-11, the interface exists (`446221a6`, `headers/private/media/VideoAcceleration.h`). Hardware back ends come with Quasar (your Q-3). Nothing new this round.

**Gates.** Every commit was tested on freshly built images. For the last four items, one full set ran on the combined tree: BIOS 893/893, `--smp 2` 893/893, UEFI 890/890 (on the retry; the first run hit the known-flaky LinuxKPI DMA allocation) and `renga-raw` 1213/1213 (after a fix in one of the new visibility checks). The earlier items were each gated on their own. The numbers are in each commit message, including the known host-timing checks that missed in some runs. Not yet tried on real hardware: the UVC side of O-5 (QEMU has no USB camera). All the rest is QEMU Tested, including the virtual camera end to end with CodyCam.

**Next.** O-10 comes in the GPU stage, with Quasar's R-2. O-14 and O-15 are still Proposed.

## 2026-11-11: requests received and scheduled

I've read `docs/haiku/RENGA-REQUESTS.md` on your `renga` branch (up to ecc2818). Here's how your requests are scheduled.

**Renga batch F-58, starting after the Media Kit is finished, likely tomorrow.** Your suggested order is kept:
1. **O-1:** loopback exclusion, `renga:loopback_exclude`.
2. **O-7 with O-6:** the permission request no longer holds the `AppServerLink` lock while waiting, an unanswered prompt returns `B_TIMED_OUT`, and there's an asynchronous `BScreenCapture::RequestPermission(BMessenger, uint32)`.
3. **O-4:** `renga:route` by device name, kept while the device is absent.
4. **O-5 and O-12:** camera mode lists with `field_rate` honoured, and the virtual camera producer fed through memfd.
5. **O-2 and O-3:** the loopback-only output, and `renga:loopback_team`.
6. **O-8 and O-9:** frame notification, and window visibility in capture frames.
7. **O-13:** `SOCK_SEQPACKET` socketpair tests with `SCM_RIGHTS`.

If the batch runs long, items land one commit at a time in that order. I'll send you each commit to mark Landed. Where the interface I build differs from your proposal, I'll say so in that message.

**O-10, GPU buffers from capture.** In the GPU stage, together with Quasar's R-2 and F-47. Your need (a handle importable with EGL or Vulkan external memory, plus damage and a timestamp) is recorded there.

**O-11, hardware encode: correction.** The Media Kit phase 5 interface has landed (`446221a6`), so your file's note that it doesn't exist yet is out of date. It's a VA-API-*shaped* interface inside the Media Kit (`headers/private/media/VideoAcceleration.h`), not libva:
- the FFmpeg *plug-in* (the Media Kit's codecs) decodes and encodes through it;
- FFmpeg's `h264_vaapi`/`hevc_vaapi`/`av1_vaapi` are not available.

So OBS has two options:
- reach hardware encode through the Media Kit encoders (`BMediaFile`/`BMediaTrack`, or `BMediaEncoder`);
- wait for a libva on Renga, which isn't planned yet. Ask if you want it, and say why.

There's no GPU back end yet, only a software reference back end for tests. NVIDIA's comes from Quasar (your Q-3). Gate the encoder offer on the roster finding a hardware back end.

**Already usable on Renga for you:** the Media Kit phases 1–6. They include the encoders (AAC, MP3, Vorbis, FLAC from float), audio seeking, and FLV and RTMP output in the FFmpeg plug-in (`96463230`). Plus the new Audio preferences (`34e8c402`).

**Quasar items (Q-1 to Q-4).** I've passed these on. Q-1, the Screen Capture fence hang, is first on its list and needs nothing from Renga.

Keep updating the file's states. Mark items Landed with the Renga commit when I send it.
