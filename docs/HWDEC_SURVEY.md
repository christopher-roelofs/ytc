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
| Powkiddy X55 (ROCKNIX) | `powkiddy,x55` / `rockchip,rk3566` | `mali-blob` (`mali_kbase`) | `rockchip,rk3568-vpu-dec` (Hantro) — H264, **no VP9** |
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

## Next steps

1. Build a v4l2m2m-enabled ffmpeg 6.1 (+ render-only libmpv 0.36 against it)
   on the Pi — same recipe as docs/MPV_BUILD.md plus `--enable-v4l2-m2m`.
2. Hand-test `mpv --hwdec=v4l2m2m-copy` on the RP5 with a real googlevideo
   stream (then the X55, H264 only).
3. Launcher-side selection in YTC.sh via the probe's VERDICT (gpu/soc), with
   the app's Video Decode toggle unhidden where a decoder exists.
4. Decide bundling vs. download-on-demand for the ~20 MB v4l2 lib set.

Device access for future sessions: RP5 at 192.168.86.246, X55 at .36 (both
root/rocknix, DHCP — may move); muOS at .245 (root/root); Pi at .243.
