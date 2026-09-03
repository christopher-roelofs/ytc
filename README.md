# ytc
An anonymous/unauthenticated YouTube client 

## Hardware video decode

ytc uses hardware video decoding **by default wherever it is real**, and falls
back to software everywhere else — no setup, no downloads.

**How it decides.** At startup the app probes on a background thread, and the
`Video Decode` toggle (Settings ▸ Video, `Hardware` / `Software`) appears only
when hardware decode is actually usable on the machine:

1. **Handhelds** — a stateful V4L2 decoder on the device (verified via ioctl, not
   just by name) plus the `v4l2m2m` decoder in the bundled ffmpeg → mpv
   `hwdec=v4l2m2m-copy`.
2. **Desktops** — ffmpeg is asked to initialize a real hardware decode device
   (VAAPI on Linux, VideoToolbox on macOS, D3D11 on Windows, …) → mpv
   `hwdec=auto-copy-safe`, which picks that backend at play time.

Copy-mode (`-copy`) keeps decoded frames CPU-visible so one GLES2 renderer works
across every GPU vendor, and it falls back to software **per stream** whenever
the hardware can't handle a codec — "Hardware" never breaks playback.

**Where it works.**

| Device class | Status |
|---|---|
| Retroid Pocket 5 / Pocket Mini (Snapdragon 865, Venus) | **Verified** — ~24× less CPU than software at 1080p |
| Snapdragon 8 Gen 2 / 8 Gen 3 handhelds (AYN Odin 2 line, Retroid Pocket 6, AYN Thor, AYANEO Pocket S/S2/ACE/DMG/EVO, KONKR Pocket FIT) | Expected — Qualcomm Iris driver, same stateful V4L2 path |
| Snapdragon 662 handhelds (Mangmi Air X) | **Verified** — decoder keeps up at 1080p60; the copy/render path tops out around 720p60 on this SoC |
| Amlogic S922X (ODROID-Go Ultra, Powkiddy RGB10 Max 3 Pro) | Plausible — needs a device test |
| Linux desktops / laptops with VAAPI (Intel, AMD incl. **Steam Deck**), macOS | Expected — verified on Intel UHD 620 |
| Rockchip RK3326 / RK3566 / RK3588 (RG351x, RG353x, RGB30, X55, GameForce Ace) | Software — stateless decoders, not usable by mainline ffmpeg |
| Allwinner H616/H700 / A133P / A523 (Anbernic RG35XX/RG40XX/RG34XX/CubeXX, TrimUI Brick/Smart Pro) | Software — no usable decoder exposed (confirmed on muOS and KNULLI) |

**Checking what your device does.** During playback, open the options menu and
enable *Stats for Nerds*: the `Decode:` line shows the active method
(`v4l2m2m-copy`, `vaapi-copy`, `videotoolbox-copy`, or `software`). Launching
with `YTC_DEBUG=1` prints the detection verdict as a `[hwdec] …` line.

**Overrides.** `YTC_HWDEC=no` (or any mpv `hwdec` value) in the environment
beats the setting — a developer/launcher escape hatch if a driver misbehaves.
Choosing `Software` in Settings persists across launches.

**Help map more hardware.** Run the report script on any device and share its
output (also saved as `ytc_hw_report.txt`):

```
curl -L https://raw.githubusercontent.com/christopher-roelofs/ytc/main/tools/gpu_probe.sh | sh
```

Details: [docs/HWDEC_SURVEY.md](docs/HWDEC_SURVEY.md) (device survey and
findings), [docs/HWDEC_SETUP_FLOW.md](docs/HWDEC_SETUP_FLOW.md) (the dormant
download flow for backends that need extra libraries),
[docs/MPV_BUILD.md](docs/MPV_BUILD.md) (library build recipe).
