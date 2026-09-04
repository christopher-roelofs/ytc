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

## Outcome (2026-08-31, revised 2026-09-01): shipped for stateful devices; stateless PARKED

**Revision 2026-09-01 — v4l2m2m folded into the base bundle.** It has no
external deps (kernel headers only), so `libs.aarch64` now IS the v4l2 ffmpeg
build and the RP5-class devices get hardware decode with no download and no
prompt. The app detects it standalone (`src/hwdetect.h` ioctl probe +
`avcodec_has_decoder("h264_v4l2m2m")` against the loaded libs) and shows the
Video Decode toggle at startup. The manifest's `v4l2` bundle stays as the
contract record with an EMPTY file list (= built in; `ytc_setup` records
`choice=builtin`). The ytc_setup download flow (dormant, NOT shipped — see docs/HWDEC_SETUP_FLOW.md) is reserved for backends that
carry real extra baggage (rkmpp: librockchip_mpp; request-API: libudev +
patched libmpv) — the PortMaster side. Desktop parity: macOS asserts
VideoToolbox; other desktops learn from mpv's hwdec-current (`hwdec_seen`).

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

## Field reports (2026-09-02, via `tools/gpu_probe.sh` from users)

| Device | CFW / kernel | SoC | GPU | Video decoder (v4l2) | Result |
|---|---|---|---|---|---|
| Retroid Pocket 5 | ROCKNIX 20260701 / 7.0.11 | `qcom,sm8250` | Adreno (msm_dpu, mesa) | `qcom-venus-decoder` | **hardware (verified)** |
| **Mangmi Air X** | ROCKNIX 20260801 / 7.1.2 | `mangmi,sm6115-air-x-mq66` / `qcom,sm6115` | Adreno 610 (msm_dpu, mesa) | `qcom-venus-decoder` (on video1; encoder is video0) | **hardware works but loses to software** — Stats: `Decode: v4l2m2m-copy`, decoder drops 0 at 1080p60 H264, yet ~1400 presentation drops/min and +1 s A/V drift; the owner reports SOFTWARE plays the same 1080p60 video better. Cause: Venus capture buffers are mapped uncached, so copy-mode's per-frame memcpy is slower than the 8 CPU cores decoding into cached memory. **App defaults to Software on `qcom,sm6115`** (toggle available). Zero-copy is the fix. |
| Powkiddy X55 | ROCKNIX 20260701 / 7.0.2 | `rockchip,rk3566` | Mali blob | `rk3568-vpu-dec` (stateless) + `rga` | software (verified) |
| Anbernic RG353M-class ("rgb30" DTB reports `anbernic,rg353m`) | Debian 13 vendor kernel 5.10.226 | `rockchip,rk3566` | Mali blob | none as v4l2 — `/dev/mpp_service` only (vendor rkmpp) | software — rkmpp path, parked |
| Anbernic RG351V | AmberELEC 20250515 / 4.4.189 | `rockchip,rk3326` | Mali blob | none | software |
| Anbernic RG35XX H | muOS 2606 / 4.9.170 | `allwinner,h616` | Mali blob | none | software |
| Anbernic RG40XX V | muOS 2601 / 4.9.170 | `allwinner,h616` | Mali blob | none | software |
| Anbernic RG CubeXX | KNULLI (Batocera 42) / 4.9.170 | `allwinner,h616` | Mali blob | none | software |
| TrimUI Smart Pro S | KNULLI (Batocera 42) / 5.15.147 | `allwinner,a523` (sun55iw3) | Mali blob (sunxi-drm) | none | software |

Takeaways:
- **Detection held on every report** — no false positives; the decoder nodes are
  exactly where hardware decode is plausible. The Mangmi Air X decoder sitting
  on `video1` (encoder on `video0`) is handled because detection matches by
  name, not node index.
- **Zero-copy attempted and PARKED (2026-09-04).** Facts established on the RP5:
  1. Mainline ffmpeg 6.1's `v4l2_m2m` cannot output DRM_PRIME at all (no code
     for it) — mpv reports "Unsupported hwdec: v4l2m2m" and its `v4l2m2m`
     (non-copy) name is only a generic table entry. So zero-copy requires the
     out-of-tree LibreELEC `v4l2-drmprime` ffmpeg patch (libreelec-12.x,
     ffmpeg 6.0.1; 814 lines, only `libavcodec/v4l2_*` + configure; applies
     cleanly on its own, no dependency on the request-API series).
  2. With that patch built (ffmpeg 6.0.1 + libdrm; libmpv 0.36 rebuilt with
     `-Degl=enabled -Ddrm=enabled` -> features `dmabuf-interop-gl drm egl`,
     NEEDED gains libdrm.so.2 + libEGL.so.1 which muOS and ROCKNIX both have),
     the app requesting `hwdec=v4l2m2m,v4l2m2m-copy` **crashed on the RP5**
     (SIGSEGV on the mpv core thread at decoder init) and dmesg showed the
     Venus firmware faulting: `SFR message from FW: Exception ... FA = 0x0
     cause = 0x6`, `System error has occurred, recovery failed to init HFI`.
  3. Bisected with the patched ffmpeg CLI, no mpv, on freshly rebooted
     firmware: plain `h264_v4l2m2m` (software formats) decodes fine, but
     `-init_hw_device drm -hwaccel drm` (the patch's DRM_PRIME export path)
     **hung the decoder in-kernel and the device rebooted** (watchdog). The
     patch was developed against bcm2835-codec / Rockchip / Allwinner
     drivers; Venus's dmabuf-export interplay with it is broken at the
     driver/firmware level (mainline 6.19/7.0 venus). Fixing that is kernel
     work, not app work.
  Also observed: an abrupt userspace exit mid-stream (`-frames:v N`) can
  fault the Venus firmware too ("no valid instance ... session_id:ff") — the
  driver's session teardown is fragile; the app's clean stop path is fine.
  Shipped state is unchanged: mainline 6.1 v4l2m2m-copy bundle, copy-only
  request. Revisit if venus gains robust VIDIOC_EXPBUF/dmabuf support
  upstream or the patchset gets Venus-specific fixes.
- **Copy-mode can be a net LOSS on budget SoCs**: the Air X proves decode is
  not the bottleneck (dec drops 0) — the memcpy out of UNCACHED V4L2 capture
  buffers is, and it loses to 8-core software decode into cached memory. So
  "hardware if available" is not universally right: the app now defaults to
  Software on known slow-copy SoCs (`kSlowCopyPath` in ui.cpp: `qcom,sm6115`),
  keeping the toggle. The real lift is zero-copy — hwdec=v4l2m2m (non-copy)
  emitting DRM_PRIME dmabufs, imported into our GLES context via
  EGL_EXT_image_dma_buf_import (mpv's drmprime interop) — which needs libmpv
  rebuilt with egl+drm enabled and a muOS (libmali/libdrm) compatibility
  check. That is the next hwdec project; it would beat both paths on every
  mesa device including the RP5.
- **GPU load is NOT the indicator** for hardware decode: Venus is a separate
  video engine, and in copy mode the Adreno does nothing special (texture
  upload only). Confirm with Stats for Nerds' `Decode:` line or CPU load.
  Diagnosing the Air X: (1) latest port zip (base libs carry v4l2m2m);
  (2) Settings > Video shows the Video Decode toggle? (absent = detection
  said no: old libs or ioctl-not-stateful); (3) Stats for Nerds `Decode:`
  (`v4l2m2m-copy` vs `software`); (4) over SSH, `YTC_DEBUG=1` launch —
  the `[hwdec]` line plus any mpv `driver decode error` / fallback lines.
  A stateful node that fails at stream-on (firmware, HFI1 quirks on the
  SM6115 Venus) falls back to software silently — mpv's safety net.
- **Allwinner is uniformly software** across H616 (muOS, KNULLI) and the newer
  **A523** (TrimUI Smart Pro S — not A133P as first assumed): no CFW exposes cedrus.
- **Rockchip splits by kernel**: mainline (ROCKNIX) exposes a stateless Hantro;
  vendor 5.10 kernels (that Debian RG353M/RGB30 build) expose only `mpp_service`.
  Neither is usable by our v4l2m2m path; both would need the parked backends
  (request-API and rkmpp respectively). RK3326 on AmberELEC's 4.4 kernel exposes
  nothing at all.
- **ROCKNIX kernels moved fast**: 6.19 in January, 7.0.x/7.1.x by summer 2026.
  Venus on SM8250 has been stable across all of them.

## Ecosystem outlook (researched 2026-08-31 — UNVERIFIED, expectations only)

ROCKNIX (2026 stable) supports ~66 devices across Rockchip (RK3326 / RK3566 /
RK3588 / RK3399), Qualcomm (SM6115 / SM8250 / SM8550 / SM8650), Allwinner
H700, and Amlogic S922X. muOS 2601 covers eleven Anbernic H700 models plus
TrimUI Brick/Smart Pro (Allwinner A133P). Expected hwdec status per family,
assuming our stateful-H264 ioctl detection:

| SoC family | Example devices | Decoder | Expected verdict |
|---|---|---|---|
| Qualcomm SM8250 | Retroid Pocket 5 / Pocket Mini | Venus (stateful) | **works — VERIFIED** |
| Qualcomm SM8550/SM8650 | AYN Odin 2 line, Retroid Pocket 6, AYN Thor, AYANEO Pocket S2/ACE, KONKR Pocket FIT | **Iris** — stateful V4L2, mainline since ~6.15, H264/HEVC/VP9 | **expected to work with the existing v4l2 bundle unchanged** (v4l2-compliance reports it a stateful decoder) |
| Qualcomm SM6115 | Mangmi Air X (and other Adreno 610 handhelds) | Venus (stateful) | works but copy path loses to software — **defaults to Software**; zero-copy needed |
| Amlogic S922X | ODROID-Go Ultra, Powkiddy RGB10 Max 3 Pro | meson vdec — stateful (kernel staging), H264/MPEG2/VP9 | plausible; staging-quality caveats, and mpv#8884 reports poor meson-vdec H264 — needs a real device test |
| Rockchip RK3326/RK3399/RK3566 | RG351x, RG353x, RGB30, X55, ODROID-Go Advance | rkvdec / Hantro — stateless | software (X55 VERIFIED) |
| Rockchip RK3588 | GameForce Ace, CM5 modules | rkvdec2 not mainline (vendor rkmpp only) | software |
| Allwinner H616/H700 / A133P / A523 | Anbernic RG35XX/RG40XX/RG34XX/CubeXX, TrimUI Brick/Smart Pro (S) | cedrus — stateless, and no CFW exposes it | software (VERIFIED on muOS + KNULLI, H616 and A523) |

Headline: the entire modern Qualcomm handheld wave (8 Gen 2/3 class) uses the
stateful Iris driver, so the highest-value upcoming devices should light up
with the bundle we already ship — the ioctl detection will confirm per device
via user hardware reports. The stateless story stays parked as above.

Device access for future sessions: RP5 at 192.168.86.246, X55 at .36 (both
root/rocknix, DHCP — may move); muOS at .245 (root/root); Pi at .243.
