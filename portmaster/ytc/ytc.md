## Notes

**YTC (Your Tube Client)** is a native, controller-driven YouTube client for
Linux handhelds. It talks to YouTube through its own anonymous Innertube client.

Features: search, Home / channel feeds, Community posts, playlists, Shorts,
comments with replies, favorites, watch-later, history, SponsorBlock,
quality / speed / volume controls, offline downloads, and casting to a linked
device or Chromecast.

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

## Compile

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
