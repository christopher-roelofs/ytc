# Dormant: the ytc_setup hardware-decode download flow (how to re-enable)

Status (2026-09-01): NOT shipped in the port. v4l2m2m has no external deps
and was folded into the base `libs.aarch64`, and the app detects usable
decoders on its own (`src/hwdetect.h` + `ytn::avcodec_has_decoder`), so
every device we know about needs no download. This flow stays in the tree,
compiled (`yt_setup` CMake target) and hardware-tested, for the day a backend
needs libs we can't bundle (rkmpp: `librockchip_mpp`; V4L2 request-API:
`libudev` + patched libmpv). Reviving it is packaging, not code.

## What it is

- **`ytc_setup`** (`src/main_setup.cpp`, links gfx + HttpClient only): runs
  before the app; detects the decoder via `hwdetect::detect()`, resolves a
  bundle via `hwdetect::pick_bundle()` from `data/hwdec_manifest.json`, shows
  a gamepad dialog (Yes / Not now / Never ask again) with the download size,
  downloads each file with progress, verifies SHA-256 (embedded impl), installs
  atomically (`libs.hwdec.part/` -> rename `libs.hwdec/`), and writes
  `./video_decode`. `--yes` / `--never` run headless for testing.
- **Manifest contract** (`data/hwdec_manifest.json`, shipped in the port —
  the pinned hashes travel with the reviewed zip; GitHub release assets are
  dumb storage): per bundle `detect` (`v4l2-stateful` | `v4l2-stateless` |
  `devnode:<path>`), `check` (libavcodec decoder proving the libs loaded),
  `hwdec` (mpv value for Hardware mode), `files[]` {name, sha256, size, url}.
  Empty `files` = built into the base bundle. First matching bundle wins.
  `data/hwdec_version` is the plain-text version YTC.sh compares against.
- **State file** `video_decode` (port root; launcher UX only — THE APP NEVER
  READS IT, it re-probes hardware + loaded libs itself):
  `soc= gpu= decoder= stateless= bundle= hwdec= choice= manifest=` with
  `choice` in installed | declined | never | builtin. "Not now" writes no
  `manifest=` line so the prompt returns next launch; `never` and
  `manifest=unsupported` skip forever; a bumped `hwdec_version` re-offers to
  everyone else.
- **Hosting**: GitHub release on this repo (`hwdec-v1` holds the six v4l2
  ffmpeg 6.1 libs, 18 MB). Build recipe: docs/MPV_BUILD.md.

## To re-enable

1. Package: copy `build-port/yt_setup` to `portmaster/ytc/ytc_setup.aarch64`,
   and `data/hwdec_manifest.json` + `data/hwdec_version` into
   `portmaster/ytc/data/`.
2. Add a bundle with files to the manifest (hashes via `sha256sum`), upload
   the files to a release, bump `hwdec_version`.
3. Restore this block in `portmaster/YTC.sh` after the LD_LIBRARY_PATH /
   SDL_GAMECONTROLLERCONFIG exports:

```sh
# Optional hardware decode: offer the lib download once per manifest version
# (ytc_setup detects the decoder, asks Yes / Not now / Never, writes
# ./video_decode). "Never" and unsupported devices skip forever; "Not now"
# asks again next launch; a bumped data/hwdec_version re-offers to everyone
# else. Installed libs go FIRST on the path so the v4l2 ffmpeg wins.
VDFILE="$GAMEDIR/video_decode"
WANTVER=$(cat "$GAMEDIR/data/hwdec_version" 2>/dev/null)
if [ -x "$GAMEDIR/ytc_setup.${DEVICE_ARCH}" ] && [ -n "$WANTVER" ]; then
  if ! grep -q "^choice=never" "$VDFILE" 2>/dev/null && \
     ! grep -q "^manifest=$WANTVER" "$VDFILE" 2>/dev/null && \
     ! grep -q "^manifest=unsupported" "$VDFILE" 2>/dev/null; then
    "$GAMEDIR/ytc_setup.${DEVICE_ARCH}"
  fi
fi
if grep -q "^choice=installed" "$VDFILE" 2>/dev/null && [ -d "$GAMEDIR/libs.hwdec" ]; then
  export LD_LIBRARY_PATH="$GAMEDIR/libs.hwdec:$LD_LIBRARY_PATH"
fi
```

4. App side needs nothing: it already consults the manifest for bundles that
   have files (`ui.cpp` hwdec detection, step 2) and maps Hardware to the
   bundle's `hwdec` value once `avcodec_has_decoder(check)` passes.

Tested end-to-end 2026-08-31 on the Retroid Pocket 5 (Yes / Not now / Never
/ B-cancel, download + verify + install, re-prompt suppression) and on muOS
and the X55 (silent `unsupported`, no window).
