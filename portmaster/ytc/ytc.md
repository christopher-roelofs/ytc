## Notes

**YTC (Your Tube Client)** is a native, controller-driven YouTube client for
Linux handhelds. It talks to YouTube through its own anonymous Innertube client
(no login, no yt-dlp, no Python, no browser) and plays video through a bundled
render-only libmpv + FFmpeg, so it works even on CFWs that ship no libmpv.

Features: search, Home / channel feeds, Community posts, playlists, Shorts,
comments with replies, favorites, watch-later, history, SponsorBlock,
quality / speed / volume controls, offline downloads, and casting to a linked
device or Chromecast.

## Requirements

- A **network connection** (Wi-Fi). YTC streams over HTTPS.
- No YouTube account or login — all access is anonymous.

Ready to run: no game files or extra data need to be copied in.

## Controls

### Browsing

| Button | Action |
|--|--|
| D-Pad / Left Stick | Move / navigate |
| A | Play / open |
| B | Back |
| X | Sort (comments) / clear text (search) |
| Y | Search |
| Select | Options menu (favorite, watch later, download, cast, …) |
| Start | Main menu (Settings, Downloads, Watch Later, History, …) |
| L1 / R1 | Switch tabs (All / Videos / Shorts / Posts / Playlists) |

### Playback

| Button | Action |
|--|--|
| A | Play / pause |
| Left / Right | Seek 10s (hold to scrub, accelerates) |
| Up / Down | Volume |
| Select | Player options (quality, speed, captions, stats) |
| Start | Menu |
| B | Stop / back |

## Licenses

YTC's own code is under the PolyForm Noncommercial License 1.0.0 (`LICENSE`).
Bundled components — libmpv and FFmpeg (LGPL-2.1-or-later), DejaVu Sans,
SDL_GameControllerDB, nlohmann/json, and stb — keep their own licenses; see
`THIRD_PARTY_NOTICES.md`.

## Compile

Built for aarch64 on an Orange Pi 5. The goal is a binary whose only external
dynamic deps are on-device (`libSDL2`, `libGLESv2`, `libpthread/m/c`); curl +
mbedTLS and zlib are linked statically, and a **render-only** libmpv + FFmpeg
(software decode, GPU-agnostic) are bundled in `libs.aarch64/` so playback works
on CFWs that ship no libmpv.

```sh
# libmpv built render-API-only (no drm/wayland/x11/gbm/egl VO backends) so it
# drops libgbm/libEGL/libdrm NEEDEDs and needs only the FFmpeg chain + libass:
#   meson ... -Dlibmpv=true -Dvideo-output-drivers=[]
# then verify:  readelf -d libmpv.so.2 | grep NEEDED

PKG_CONFIG_PATH=$HOME/mpvbuild/prefix-clean/lib/pkgconfig \
cmake -S . -B build-port -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
  -DBUILD_PLAYER=OFF
cmake --build build-port -j"$(nproc)"
```

The stripped binary is installed here as `ytc.aarch64`; the matched
`libmpv.so.2` + FFmpeg 6.x set (`libavcodec.so.60`, `libavformat.so.60`,
`libavutil.so.58`, `libswscale.so.7`, `libswresample.so.4`, `libavfilter.so.9`)
ship in `libs.aarch64/`.
