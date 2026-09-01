# Hardware-decode survey (2026-08-31)

Groundwork for an optional hardware-decode bundle. Ran `tools/gpu_probe.sh`
on every target device; raw signals and verdicts below. The probe is POSIX/
busybox sh (muOS and ROCKNIX ship no bash) and keys off, strongest first:

1. `/proc/device-tree/compatible` — SoC vendor/model, and often the exact
   device (`retroidpocket,rp5`, `powkiddy,x55`). Present on every ARM handheld.
2. Driver devnodes — `/dev/kgsl-3d0` (Adreno downstream), `/dev/mali0` (Mali blob).
3. DRM driver in `/sys/class/drm/card*/device/uevent` — `msm*` (Adreno
   mainline; the RP5 reports `msm_dpu`), `panfrost`/`lima` (Mali mainline).
4. `/sys/class/video4linux/video*/name` — the actual decoder hardware.

## Results

| Device | SoC (`compatible`) | GPU verdict | Video decoder |
|---|---|---|---|
| Retroid Pocket 5 (ROCKNIX) | `retroidpocket,rp5` / `qcom,sm8250` | `adreno` (mainline msm + freedreno; no kgsl) | `qcom-venus-decoder` (`/dev/video0`) — H264/HEVC/VP9 |
| Powkiddy X55 (ROCKNIX) | `powkiddy,x55` / `rockchip,rk3566` | `mali-blob` (`mali_kbase`) | `rockchip,rk3568-vpu-dec` (Hantro) — **STATELESS, unusable by mainline ffmpeg**; software decode |
| Anbernic (muOS 2601) | `allwinner,h616` | `mali-blob` | **none exposed** — no hwdec possible on this CFW |
| Orange Pi 5 (build host) | `rockchip,rk3588` | `mali-blob` | `mpp_service` + `video-dec0` (rkmpp, vendor kernel) |

## Conclusions

- **CORRECTION (verified on hardware 2026-08-31):** only STATEFUL v4l2
  decoders work with ffmpeg's `v4l2_m2m` — the OUTPUT queue must accept
  full-frame `H264`. Venus (RP5) is stateful and works (~24x less CPU at
  1080p30 than software, ffmpeg 6.1; ROCKNIX's own ffmpeg 6.0 crashes).
  The X55's `rk3568-vpu-dec` (Hantro) is STATELESS (`H264_SLICE`, V4L2
  Request API) — mainline ffmpeg reports "could not find a valid device"
  and mpv falls back to software. Supporting it would need LibreELEC-style
  request-API ffmpeg patches (out of scope for now). Detection therefore
  probes statefulness via VIDIOC_ENUM_FMT (src/hwdetect.h), not just names.
- The optional `v4l2` bundle (ffmpeg 6.1 + v4l2m2m, mpv `hwdec=v4l2m2m-copy`)
  covers stateful-decoder devices; copy-mode keeps frames CPU-visible so the
  render-only libmpv and GLES2 path are untouched, GPU vendor irrelevant.
- muOS H616/H700 exposes no decode devnodes: keep software decode and hide
  the Video Decode toggle there (detection makes that honest).
- rkmpp (RK3588 vendor path) is out of scope for the port bundle — that's the
  build host, not a target.

## Outcome (2026-08-31): shipped for stateful devices; stateless PARKED

The plan above shipped: ffmpeg 6.1 with `--enable-v4l2-m2m` built on the Pi,
the six libs hosted as GitHub release `hwdec-v1` (hashes pinned in the port's
`data/hwdec_manifest.json`), `ytc_setup` offering the download once per
manifest version, `YTC.sh` putting `libs.hwdec` first on the path when
installed, and the app's Video Decode toggle appearing only where the ioctl
detection AND a v4l2m2m libavcodec both check out. Verified end-to-end on the
RP5 (Venus, `hwdec=v4l2m2m-copy`) and negatively on the X55 and muOS.

### X55 / stateless decoders — parked, and what un-parking would take

The X55's Hantro needs the V4L2 **Request API**: userspace parses the H264
bitstream and submits per-slice parameter sets. Supporting it would require:

1. ffmpeg built with the out-of-tree "v4l2-request" hwaccel patchset (the
   LibreELEC/Kodi stack; NOT mainline as of 2026 despite years of revisions)
   plus a libudev dependency — a patch-rebasing treadmill for every lib bump.
2. A different frame path: the hwaccel outputs DRM_PRIME dmabufs, not
   CPU-visible NV12. mpv 0.36's `drmprime-copy` should in principle copy them
   back for our GLES2 renderer, but mpv on request-api is little-traveled
   (Kodi is the patchset's real consumer) — expect debugging.
3. A third bundle flavor (`v4l2request`) in the manifest, gated on the
   `stateless=` detection that ytc_setup already records in `video_decode`.

Payoff on the X55 itself: H264-only, on a device that software-decodes the
port's default 480p cap acceptably — battery/thermals, not capability.

Revisit triggers: (a) the request-API hwaccel lands in mainline ffmpeg, or
(b) user hardware reports (`tools/gpu_probe.sh`, `stateless=` lines) show
stateless-decoder devices are a large share of the audience.

Device access for future sessions: RP5 at 192.168.86.246, X55 at .36 (both
root/rocknix, DHCP — may move); muOS at .245 (root/root); Pi at .243.
