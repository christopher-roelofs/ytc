# ytnative — native YouTube client for Linux handhelds

C++/SDL2/libmpv. No yt-dlp, no Python, no reliance on the device ffmpeg binary.
Single self-contained app. Targets Anbernic-class handhelds (muOS, aarch64).

## Build / test host
- **Orange Pi 5 (RK3588)** at `192.168.86.243`, user `orangepi/orangepi`, passwordless sudo.
- Debian 11, gcc/g++ 10.2, cmake 3.18, 8 cores, 16GB RAM, 198GB free.
- libmpv 2.0.14 + SDL2 2.0.14 + libcurl 7.74 all preinstalled with dev headers.
- GLES 3.2 / EGL 1.5, DRI (`card0/1`), `/dev/mpp_service` (RK3588 VPU) present.
- Deploy: `rsync` to `~/ytnative`, `cmake --build build -j8`.
- Secondary target device: **muOS 2601 handheld** at `192.168.86.244` (root/root), Allwinner H700.

## Status — both milestones PROVEN on hardware (2026-08-22)

### M1: Innertube resolver — DONE
- `src/innertube.{h,cpp}` + `src/http.{h,cpp}` (libcurl) + `third_party/json.hpp`.
- Flow: POST `/youtubei/v1/visitor_id` (session token) -> POST `/youtubei/v1/player`.
- **visitorData token is mandatory** — without it every request is bot-blocked
  ("Sign in to confirm you're not a bot"). This was the key unlock.
- Client fingerprints in `config/clients.json`, tried in order. **VISIONOS 1.02**
  (client id 101) is the working JS-less client as of 2026-08. `android_vr` is now
  bot-blocked; `tv`/`ios` are fallbacks.
- Result on Orange Pi: resolves in **~320ms**, returns full ladder 144p-2160p60
  (H264/VP9/AV1), URL probe returns HTTP 206. (yt-dlp on the same class of device
  took ~28s and only offered up to 480p JS-less.)

### M2: SDL2 + libmpv playback — DONE (headless bench)
- `src/main_play.cpp`: SDL2 owns window/GLES ctx/input; libmpv renders video into
  the GL framebuffer via the render API. Audio via mpv `audio-files` (2nd DASH url).
- KMSDRM backend works; Mali GPU GLES context created.
- **1080p60 H264 decodes at a locked 60.0 fps, 0 dropped frames — in SOFTWARE.**
  8x A76/A55 handle it easily.

### M3: codec-aware selection + search — DONE (2026-08-22)
- `best_video(VideoPrefs)` / `best_audio(AudioPrefs)`: resolution-primary, with a
  `codec_priority` list that is BOTH allowlist and tiebreak. Default allows
  H264/VP9/AV1, preferring H264 at equal height. Verified: cap 2160 now selects
  itag 315 (2160p60 VP9), cap unset still reaches 4K; a stronger device gets the
  full ladder, weaker devices cap via settings.
- `search(query)` via WEB client (id 1) — no bot wall, recursive videoRenderer
  parser robust to layout shifts. Returns id/title/author/length/views/published/
  thumbnail. ~800ms-1.3s. `src/main_search.cpp`, `config` -> `search_client`.
- Iterating on this laptop (g++13, libcurl+SDL2; no libmpv so yt_play is ARM-only),
  every change re-tested on the Orange Pi. Full ARM64 parity confirmed.

### M4: GLES2 UI — DONE (results grid) (2026-08-22)
- `src/gfx.{h,cpp}`: GLES2 (GLSL ES 1.00) toolkit — window/context (offscreen or
  KMSDRM), batched quad+texture renderer, stb_truetype font atlas, PNG screenshot.
  **GLES2 is the project floor** (not all handheld Malis expose GLES3); the UI and
  mpv share ONE ES2 context. No VAOs/instancing/ES3 calls.
- `src/ui.{h,cpp}`: `App` + results grid. Header bar, 3-col card grid, cover-cropped
  thumbnails, duration pills, ellipsized titles, author/views, selection ring,
  scroll, footer hints. Resolution-independent (scales by H/720). Theme struct.
- `ThumbCache`: worker thread downloads thumbnail bytes; GL-thread `pump()` decodes +
  uploads. Non-blocking UI.
- `src/main_ui.cpp`: interactive (gamepad/keyboard -> actions) OR headless
  (`YTNATIVE_SHOT=path` renders a nav sequence to PNGs). Honors `SDL_VIDEODRIVER`.
- **Verified on BOTH**: local Mesa (offscreen, 1280x720) and Orange Pi real Mali GPU
  (KMSDRM, native 1920x1080) — pixel-identical, resolution-independent scaling works.

### Dev-loop lessons (portability gotchas found & fixed)
- Thumbnails: the search response thumbnail URLs carry sqp/rs params -> YouTube
  content-negotiates **WebP** (stb can't decode). Fix: build canonical
  `i.ytimg.com/vi/<id>/mqdefault.jpg` (always baseline JPEG). In `innertube.cpp`.
- Font atlas is ASCII 32..126 only -> non-ASCII (e.g. `•`) renders as `?`. Use ASCII
  separators, or extend the atlas later for i18n titles.
- Device SDL is 2.0.14: no `SDL_HINT_VIDEODRIVER` macro (use `setenv`), and no
  `offscreen` video driver (use KMSDRM+sudo for headless capture on the Pi).
- ARM linker needs explicit `Threads::Threads` (std::thread in ThumbCache).

### M5: playback integrated into the UI — DONE (2026-08-22)
- `src/player.{h,cpp}`: `Player` wraps libmpv render API into the app's shared GLES2
  context. Header is mpv-free (opaque Impl); implementation is REAL when
  `YTNATIVE_HAVE_MPV` is defined (CMake sets it when libmpv is found), else a no-op
  STUB so the UI still builds/iterates on machines without libmpv (this laptop).
- App gained `Mode {Grid, Loading, Playing}`. Select on a card -> resolve() ->
  best_video/best_audio -> `player_.play(video_url, audio_url, user_agent)`.
  Playing mode: mpv paints the frame, then the GLES2 overlay draws title + progress
  bar + playhead + time + control hints on top. A=pause, B=back-to-grid, </>=seek 10s.
- **VERIFIED on Orange Pi Mali GPU (KMSDRM):** search -> select -> Big Buck Bunny
  decodes (itag 248 VP9 1080p + itag 140 audio) and renders full-screen with the
  overlay composited; position clock advances smoothly. Screenshot captured.
- **KEY BUG FIXED (hang):** `MPV_RENDER_PARAM_ADVANCED_CONTROL=1` caused a hard hang in
  the render loop under our shared-context/KMSDRM setup. Removing it (plain render mode,
  render current frame on our thread) fixed it. Do NOT re-enable without reason.
- **KEY BUG FIXED (no audio):** setting the separate audio stream via the `audio-files`
  OPTION mangled the URL (mpv stripped `https:` and tried to open `//host/...` as a
  local file -> silent video). Fix: attach it with the `audio-add <url> select`
  COMMAND on MPV_EVENT_FILE_LOADED — the URL is a discrete arg, no `&`/`:` parsing.
  Confirmed: AAC decoder opens, aid=1 selected, pipewire AO opens. Do the same for any
  future external track (subs, alt audio) — command, not path-list option.
- Headless play capture: `YTNATIVE_PLAYTEST=1` adds select+settle+screenshot steps;
  run on Pi as `sudo SDL_VIDEODRIVER=kmsdrm`. `YTNATIVE_DEBUG=1` prints [play]/[mpv]
  breadcrumbs (resolve, mpv init, frame/pos) — invaluable for the hang diagnosis.

### M6: async resolve + loading state — DONE (2026-08-22)
- Select no longer blocks: `request_playback()` spawns a background `std::thread`
  that runs `it_.resolve()` + best_video/best_audio and copies the URLs out into a
  `ResolveResult` (strings, so no Format* dangles). `poll_resolve()` (called from
  `pump_async` on the GL thread) picks up the finished result and calls
  `player_.play()` — mpv is only ever touched on the render thread.
- New `Mode::Loading`: dimmed grid + centered chasing-dot spinner (`draw_spinner`,
  driven by SDL_GetTicks) + "Loading" + title, drawn ON TOP of the flushed grid
  (begin() sets state without clearing). Back cancels (a late result is discarded
  once mode leaves Loading). `~App()` joins the resolve thread.
- Threading note: `it_` (Innertube, single curl handle) is touched by the resolve
  thread only while the main thread doesn't (no concurrent search during Loading);
  ThumbCache has its own HttpClient. One resolve at a time (`resolve_running_`).
- Verified on Pi Mali (KMSDRM): spinner overlay captured (mode=1), then transitions
  to playback (mode=2); render loop never stalls during the ~0.3-1s resolve.

### M7: on-screen keyboard for search — DONE (2026-08-22)
- `Mode::Search` + `Action::Search`. Open from grid with Y (pad) or `/`/Tab (kbd).
- `render_search`: query box w/ blinking caret + D-pad-navigable key grid (digits,
  qwerty-ish letters, space/del/clear/SEARCH). `KB` layout is rows of `Key{label,
  type, ch, span}`; span widens space/SEARCH and is honored in both layout AND nav.
- Dual input: OSK (A=type key, Y=submit, B=cancel, D-pad=move) AND direct typing via
  SDL_TEXTINPUT (SDL_StartTextInput at loop start) — Enter submits, Backspace deletes,
  Esc cancels. Handy on the laptop; the OSK is the handheld path.
- open_search seeds the box with the current query for quick edits. Atlas is ASCII so
  input_text filters to 32..126. submit_search trims + reuses the (synchronous) search.
- Verified via headless OSK screenshot; builds on ARM.
- FUTURE: submit_search is synchronous (~1s freeze) — same async treatment as resolve
  would smooth it; default quality cap belongs in a settings menu (still TODO).

### Playback gotcha — mpv ytdl_hook (fixed 2026-08-22)
- Symptom: some videos (notably vertical Shorts, e.g. itag 780 608x1080) failed with
  `loading failed (reason 4)` and `[mpv:ytdl_hook] HTTP Error 403`.
- Cause: libmpv ships a built-in ytdl_hook (Lua) that auto-runs youtube-dl/yt-dlp on
  URLs, ON BY DEFAULT. Even though WE pass a direct googlevideo URL, mpv invoked
  youtube-dl itself -> 403 -> killed playback. Also an unwanted yt-dlp dependency and
  would fail on the handheld (no yt-dlp installed).
- Fix: `mpv_set_option_string(mpv, "ytdl", "no")` in player.cpp (and main_play.cpp).
  Confirmed the failing Short now plays.

### Selection bug — vertical videos capped too low (fixed 2026-08-22)
- Symptom: vertical Shorts played at 480p even with a 1080p cap (itag 780 not 137).
- Cause: cap compared against raw pixel HEIGHT; a vertical "1080p" is 1080x1920, so
  height 1920 > 1080 cap -> rejected, fell to a low format that fit.
- Fix: `quality_px(f) = min(width,height)` (YouTube labels quality by the SHORTER
  side) used for BOTH the cap and the sort in best_video(). Verified: Leon Short now
  picks itag 137 (1080p); landscape + uncapped unchanged.

### KEY BUG — 2nd+ video "fails" (stale STOP event) (fixed 2026-08-22)
- Symptom: first video plays; every subsequent Select bounces straight back to the
  grid ("multiple videos failed"). Log: `finished playback, success (reason 2)` right
  after each resolve, no playback start.
- Cause: pressing Back calls player_.stop() which queues an mpv END_FILE(reason=STOP).
  While in Grid mode nothing drains mpv events, so that stale STOP sits in the queue;
  on the NEXT play, pump() reads it and set ended=true -> App stops the just-started
  video. First video worked only because there was no prior STOP.
- Fix: in pump(), only END_FILE reason EOF(0) or ERROR(4) counts as "ended"; ignore
  STOP(2)/QUIT(3)/REDIRECT(5) (always our own doing). Verified with YTNATIVE_SEQTEST
  (plays N grid items in a row) — all now PLAYING.
- Test hook: `YTNATIVE_SEQTEST=1 YTNATIVE_SEQN=N YTNATIVE_SHOT=/tmp/x` (needs SHOT to
  enter headless mode) plays N items select->back->next and prints PLAYING/FAIL each.

### KEY BUG — iOS-only videos 403 in mpv (fixed 2026-08-22, big one)
- Symptom: some videos (e.g. copyrighted movie clips like "Every Fight Scene...")
  resolve OK but mpv fails with `https: HTTP error 403 Forbidden`.
- Root cause chain (verified with curl/ffprobe): these videos are UNPLAYABLE on
  VISIONOS/ANDROID_VR ("video not available") and only resolve via the IOS client.
  IOS-issued googlevideo URLs (`c=IOS`) REJECT ffmpeg's open-ended `Range: bytes=N-`
  (403) AND a single full-length range (403) — they require SMALL chunked ranges
  (~1 MiB bounded -> 206). Not IP-bound, not UA-bound (all UAs/families gave 206 with
  a bounded range). ffmpeg can't be forced to chunk via a static header.
- Fix: custom libmpv stream protocol `ytn://` (src/stream.{h,cpp}) via
  mpv_stream_cb_add_ro. read_fn fetches with libcurl using bounded `Range:
  pos-(pos+chunk-1)` (kChunk=1MiB); size from `clen=`; seek supported. player.cpp
  wraps both video+audio URLs as `ytn://<base64url(ua\nurl)>` (HLS .m3u8 NOT wrapped
  — mpv handles live natively). Verified: the iOS-only video now plays; normal
  VISIONOS videos + audio still play. This also centralizes fetching for future
  URL-refresh-on-expiry.

### Video artifacts — Intel-VAAPI-specific, NOT the target (diagnosed 2026-08-22)
- Symptom: on the dev LAPTOP (Intel UHD 620), 2nd+ videos showed artifacts.
- Ruled out: ytn:// data is byte-identical to reference (md5 match); software decode
  is clean (offscreen frame + no decode errors).
- PROVEN on Orange Pi (RK3588, rkmpp hwdec — same class as the handheld target):
  3 videos in a row via SEQTEST screenshots are ALL CLEAN. So the artifacts are an
  Intel VAAPI + mpv-render-API interop bug on the laptop only; real target hw is fine.
- Mitigations kept (help the target + robustness): hwdec default `auto-copy-safe`
  (copy frames, don't import GPU surfaces); FRESH mpv instance per video
  (Player::play tears down + re-inits — reused instance leaked decoder/surface state).
- Laptop dev workaround: `YTNATIVE_HWDEC=no` (software; i7-8650U handles 1080p fine).
- Eventual settings menu should carry a per-device hwdec choice.

### Remaining failure class: live streams / premieres
- Premieres (isUpcoming) genuinely can't play — now show YouTube's reason as a
  centered banner ("This live event will begin in 3 days.") instead of silent fail.
- Live streams (isLive) play via the HLS manifest (added this session).
- `LIVE_STREAM_OFFLINE` for upcoming premieres = genuinely unplayable (show a clear
  message). But actual LIVE videos may also report this via VISIONOS/IOS clients —
  live needs the HLS manifest path (hlsManifestUrl), not adaptiveFormats. TODO:
  detect live, fall back to HLS (mpv plays HLS natively, muxed A/V), and give clear
  UX for true premieres. Also: resolve-failure reasons aren't logged in debug yet
  (only successes print) — add failure logging.

### Controller mappings — SDL_GameControllerDB integrated (2026-08-22)
- `data/gamecontrollerdb.txt` (community DB, 2270 entries) loaded at startup via
  SDL_GameControllerAddMappingsFromFile; `data/gamecontrollerdb_local.txt` loaded
  after (local overrides win). Paths resolve from cwd, then <exe>/data, <exe>/../data,
  /opt/ytnative, /usr/share/ytnative (exe_dir via /proc/self/exe). Env overrides:
  YTNATIVE_GAMEPADDB / YTNATIVE_GAMEPADDB_LOCAL.
- Diagnostics: YTNATIVE_PADTEST=1 = a loop that logs RAW joystick button indices +
  the SDL button each maps to, acting on nothing (so pressing A can't quit). Used to
  build a mapping for an unknown controller.
- Fixed the user's controller: an unbranded X360 clone (guid 030081b85e04...8e02...)
  NOT in the DB, Nintendo-style face layout -> SDL's generic fallback swapped A/B for
  them (confirm was landing on Back -> pressing A quit the app). Local override
  `a:b1,b:b0,x:b3,y:b2` fixes it (confirm=raw b1/east, back=raw b0/south). Confirmed working.
- FUTURE (scalable): the bundled DB handles most mainstream pads; the long tail
  (clones, handheld built-in controls) needs per-GUID entries OR an in-app "swap A/B"
  toggle in the settings menu (more user-friendly than shipping every GUID). On
  handhelds, the device OS usually ships its own gamecontrollerdb for built-in controls.

### Responsive UI + carousel view (2026-08-22)
- Resizable window in windowed mode (SDL_WINDOW_RESIZABLE); on SIZE_CHANGED the
  Window re-queries drawable size and App::on_resize re-flows layout + re-bakes fonts
  (debounced: only when height changes >=24px). YTNATIVE_WINSIZE=WxH sets initial size.
- Responsive columns: compute_columns() = round(width/380), clamped [2,6]. 640->2,
  1280->3, 1920->5. Fewer columns on small screens = bigger cards + text.
- Font readability floor: scale = clamp(H/720, 0.9, 2.2) so text stays legible on
  small handheld screens and isn't oversized at 4K.
- Carousel/coverflow browse view (toggle: V key / Left-Shoulder). render_carousel:
  large centered card + scaled/dimmed neighbors, eased carousel_pos_ animation,
  centered title/author/position. Left/Right = 1D nav, A plays. Headless capture:
  YTNATIVE_CAROUSELSHOT=1. Grid remains the default; carousel is opt-in per session.

### Resolution strategy (see docs/RESOLUTION_STRATEGY.md)
- Decision: STICK with config-swappable JS-less clients (visionos/tv/ios) + ytn://
  bounded-range fetch. SKIP QuickJS/web-client sig-cipher (higher maintenance churn,
  doesn't solve PoTokens, industry moved away from it). Full reasoning + a "when it
  breaks" runbook + login analysis are in docs/RESOLUTION_STRATEGY.md.
- Login = optional FEATURE unlock (subs/history/age-restricted, gentler bot-walling),
  NOT a robustness fix. Real caveats: account-ban risk (use throwaway), credential
  security on-device, clunky OAuth. If added: opt-in, session-only creds, layered on
  top of the JS-less flow.

## Key findings / next steps
1. **hwdec NOT engaging** on the stock libmpv (`hwdec-current` empty). `/dev/mpp_service`
   exists but this 2020-era libmpv build isn't wired to rkmpp. => build our own libmpv
   with rkmpp/v4l2-request hwdec for 4K + to save power on weaker SoCs. This is the
   main argument for bundling a custom libmpv anyway. NOTE: even in SOFTWARE the RK3588
   does 4K60 VP9 at 60fps / only 3 drops over 8s — hwdec would make it trivial + cheap.
2. **YouTube H264 tops out at 1080p** (itag 299); 1440p/4K are VP9/AV1 only. Handled by
   codec-aware selection (M3).
3. **Format selection is a settings concern**: menu -> default max height + codec
   preference maps directly onto VideoPrefs/AudioPrefs.
4. Fingerprints WILL drift — treat `clients.json` as the maintenance surface, crib from
   yt-dlp `yt_dlp/extractor/youtube/_base.py` when extraction breaks.

## TODO
- [x] Extend `best_video()` for VP9/AV1 + codec preference (unlock >1080p).
- [x] Innertube search endpoint (`/search`) for the UI.
- [ ] Innertube browse/continuation (`/next` related, search pagination, channel/home).
- [ ] Cross-compile / bundle static libmpv+ffmpeg with rkmpp & v4l2 hwdec.
- [x] SDL2/GLES2 10-foot UI: thumbnail grid + gamepad nav (results screen).
- [x] On-screen keyboard for search input (Mode::Search; pad OSK + direct typing).
- [x] Wire UI select -> playback: mpv render sharing the UI's GLES2 context;
      overlay (title/progress/time) + controls (pause/seek/back).
- [x] Loading state UX: resolve() runs on a background thread; dimmed-grid spinner;
      Back cancels. Grid render loop stays responsive.
- [ ] Quality picker in the player (VideoPrefs already supports it); settings menu.
- [ ] hwdec: stock libmpv still software-decodes (VP9 1080p played fine); custom
      libmpv with rkmpp for 4K + battery.
- [ ] Video detail screen; buffering indicator; error toast on resolve/play failure.
- [ ] Subscriptions via channel RSS; local history (sqlite/flat); SponsorBlock.
- [ ] URL-expiry (403) refresh: re-resolve + reload at current position.
- [x] PortMaster-style port RUNNING on the muOS H700 device (2026-08-22) — see
      `portmaster/README.md` for the full recipe. Highlights: static curl+mbedtls
      (CA bundle path shared by Debian and muOS), linked against the DEVICE's
      libmpv.so.2 (0.35.1 — newer than the Pi's distro 0.32; SONAME mismatch was
      the trap), font falls back to bundled data/DejaVuSans.ttf, launcher caps
      YTNATIVE_MAXHEIGHT=480. Search + thumbnails (TLS) + GLES2 grid + full mpv
      playback all verified on the RG28XX (640x480, software decode).
      Remaining for a store submission: port.json/screenshot/licenses + other-CFW
      matrix (libmpv presence per CFW; bundling custom mpv+ffmpeg is the fallback).
- [ ] Test playback on the muOS H700 device (weaker; expect ~480p software ceiling).
      ^ DONE as part of the port above — playback confirmed by the user on hardware.

## Layout
```
config/clients.json   client fingerprints (the maintenance surface)
src/http.*            libcurl wrapper (persistent handle, keep-alive)
src/innertube.*       visitor_id + player, format parsing, best_video/best_audio
src/main_resolve.cpp  M1 CLI: resolve + print ladder + probe URL
src/main_play.cpp     M2: SDL2+libmpv player (YTNATIVE_BENCH=<s> for headless bench)
third_party/json.hpp  nlohmann/json single header
```
