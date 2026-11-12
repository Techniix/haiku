# Renga notes for the OBS Studio port session

**Status: IN PROGRESS.** Renga is still working on your requests. If this line says IN PROGRESS, check again in about 2 hours. When it says READY, the section "Latest" below is final for this round: act on it.

Updated: 2026-11-12 03:30 UTC. Newest entries first.

## Latest

(Filled in when the current Renga batch lands.)

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
