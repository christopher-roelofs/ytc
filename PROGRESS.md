# ytc — native YouTube client for Linux handhelds

C++/SDL2/libmpv. No yt-dlp, no Python, no reliance on the device ffmpeg binary.
Single self-contained app. Targets Anbernic-class handhelds (muOS, aarch64).

## Build / test host
- **Orange Pi 5 (RK3588)** at `192.168.86.243`, user `orangepi/orangepi`, passwordless sudo.
- Debian 11, gcc/g++ 10.2, cmake 3.18, 8 cores, 16GB RAM, 198GB free.
- libmpv 2.0.14 + SDL2 2.0.14 + libcurl 7.74 all preinstalled with dev headers.
- GLES 3.2 / EGL 1.5, DRI (`card0/1`), `/dev/mpp_service` (RK3588 VPU) present.
- Deploy: `rsync` to `~/ytc`, `cmake --build build -j8`.
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
  (`YTC_SHOT=path` renders a nav sequence to PNGs). Honors `SDL_VIDEODRIVER`.
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
  `YTC_HAVE_MPV` is defined (CMake sets it when libmpv is found), else a no-op
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
- Headless play capture: `YTC_PLAYTEST=1` adds select+settle+screenshot steps;
  run on Pi as `sudo SDL_VIDEODRIVER=kmsdrm`. `YTC_DEBUG=1` prints [play]/[mpv]
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
  STOP(2)/QUIT(3)/REDIRECT(5) (always our own doing). Verified with YTC_SEQTEST
  (plays N grid items in a row) — all now PLAYING.
- Test hook: `YTC_SEQTEST=1 YTC_SEQN=N YTC_SHOT=/tmp/x` (needs SHOT to
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
- Fix: custom libmpv stream protocol `ytc://` (src/stream.{h,cpp}) via
  mpv_stream_cb_add_ro. read_fn fetches with libcurl using bounded `Range:
  pos-(pos+chunk-1)` (kChunk=1MiB); size from `clen=`; seek supported. player.cpp
  wraps both video+audio URLs as `ytc://<base64url(ua\nurl)>` (HLS .m3u8 NOT wrapped
  — mpv handles live natively). Verified: the iOS-only video now plays; normal
  VISIONOS videos + audio still play. This also centralizes fetching for future
  URL-refresh-on-expiry.

### Video artifacts — Intel-VAAPI-specific, NOT the target (diagnosed 2026-08-22)
- Symptom: on the dev LAPTOP (Intel UHD 620), 2nd+ videos showed artifacts.
- Ruled out: ytc:// data is byte-identical to reference (md5 match); software decode
  is clean (offscreen frame + no decode errors).
- PROVEN on Orange Pi (RK3588, rkmpp hwdec — same class as the handheld target):
  3 videos in a row via SEQTEST screenshots are ALL CLEAN. So the artifacts are an
  Intel VAAPI + mpv-render-API interop bug on the laptop only; real target hw is fine.
- Mitigations kept (help the target + robustness): hwdec default `auto-copy-safe`
  (copy frames, don't import GPU surfaces); FRESH mpv instance per video
  (Player::play tears down + re-inits — reused instance leaked decoder/surface state).
- Laptop dev workaround: `YTC_HWDEC=no` (software; i7-8650U handles 1080p fine).
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
  /opt/ytc, /usr/share/ytc (exe_dir via /proc/self/exe). Env overrides:
  YTC_GAMEPADDB / YTC_GAMEPADDB_LOCAL.
- Diagnostics: YTC_PADTEST=1 = a loop that logs RAW joystick button indices +
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
  (debounced: only when height changes >=24px). YTC_WINSIZE=WxH sets initial size.
- Responsive columns: compute_columns() = round(width/380), clamped [2,6]. 640->2,
  1280->3, 1920->5. Fewer columns on small screens = bigger cards + text.
- Font readability floor: scale = clamp(H/720, 0.9, 2.2) so text stays legible on
  small handheld screens and isn't oversized at 4K.
- Carousel/coverflow browse view (toggle: V key / Left-Shoulder). render_carousel:
  large centered card + scaled/dimmed neighbors, eased carousel_pos_ animation,
  centered title/author/position. Left/Right = 1D nav, A plays. Headless capture:
  YTC_CAROUSELSHOT=1. Grid remains the default; carousel is opt-in per session.

### Resolution strategy (see docs/RESOLUTION_STRATEGY.md)
- Decision: STICK with config-swappable JS-less clients (visionos/tv/ios) + ytc://
  bounded-range fetch. SKIP QuickJS/web-client sig-cipher (higher maintenance churn,
  doesn't solve PoTokens, industry moved away from it). Full reasoning + a "when it
  breaks" runbook + login analysis are in docs/RESOLUTION_STRATEGY.md.
- Login = optional FEATURE unlock (subs/history/age-restricted, gentler bot-walling),
  NOT a robustness fix. Real caveats: account-ban risk (use throwaway), credential
  security on-device, clunky OAuth. If added: opt-in, session-only creds, layered on
  top of the JS-less flow.

### KEY BUG — crash on load (unhandled exception from startup search) (fixed 2026-08-22)
- Symptom: app crashed intermittently on launch (esp. before wifi is up).
- Cause: startup `app.search()` -> `Innertube::search` -> `ensure_visitor_data` ->
  `refresh_visitor_data` THROWS on any network hiccup (visitor_id HTTP fail / bad JSON /
  missing token). Nothing caught it -> propagated out of main() -> std::terminate.
  Same latent crash in the background resolve thread (an uncaught exception in a
  std::thread also calls std::terminate).
- Fix: public entry points never throw now. `Innertube::search` wraps its body ->
  returns {} on failure; `Innertube::resolve` wraps -> returns status=NETWORK_ERROR;
  the resolve-thread lambda has a try/catch backstop; `main` builds the App in a
  try/catch (graceful exit on config error, not terminate). Empty startup search shows
  a "check your connection" banner. Verified: forced network failure (dead proxy) now
  exits 0 with "[innertube] search failed", was exit 134 (terminate) before.

### Second device: RockNIX (Adreno) support + render-only libmpv bundle (2026-08-23)
- Target 2: RockNIX handheld (Retroid Pocket, Snapdragon 865 / Adreno), aarch64, at
  192.168.86.249 (root/rocknix; DHCP — was expected at .246). Has SDL2, GLES/EGL,
  ffmpeg 6.x, libass.9, ALSA, and the `mpv` CLI — but NO `libmpv.so`.
- muOS's libmpv can't be reused: it hard-NEEDs libmali.so.0 (Mali blob) — wrong GPU
  vendor for Adreno.
- Fix: built a RENDER-API-ONLY libmpv on the Pi (mpv 0.36, VO backends disabled:
  drm/gbm/egl/jpeg/vaapi/vulkan) so it needs only ffmpeg 6.x + libass + alsa (all
  present on RockNIX) — no libmali/gbm/drm/EGL. Bundled as the port's
  libs.aarch64/libmpv.so.2 on the device; recipe + lib in portmaster/prebuilt/rocknix/.
- IMPORTANT: this libmpv links ffmpeg 6.x; muOS has ffmpeg 4.x, so the bundled lib is
  kept OUT of the shared port/libs.aarch64 (empty there → muOS uses its own system
  libmpv). RockNIX-only deploy adds it.
- Deployed to /roms/ports/ytc + /roms/ports/ytc.sh. Verified at runtime: app + bundled
  libmpv load (ldd clean, ffmpeg/ass/alsa resolve from device), SDL inits, controller
  detected ("Retroid Pocket Gamepad", L/R shoulders mapped). Full playback test is the
  user's (needs the device display via the Ports menu).
- Chosen "bundle libmpv + ffmpeg" but the Pi's ffmpeg 6.1 is unbundleable as-is
  (libavcodec needs librockchip_mpp; libavdevice drags X11/DRM/xcb) → used the device's
  clean ffmpeg instead. A no-rkmpp/no-X11 ffmpeg build would be needed for a truly
  self-contained media stack (deferred).

### SponsorBlock (2026-08-23)
- Auto-skips sponsor/intro/outro/selfpromo/interaction/music_offtopic segments via the
  community API (sponsor.ajay.app). Settings toggle "SponsorBlock: On/Off" (persisted,
  default On).
- Privacy-preserving: queries by the first 4 hex chars of SHA-256(videoId) and filters
  locally, so the exact id never leaves the device. Self-contained SHA-256 in
  innertube.cpp (no crypto dep); Innertube::sponsor_segments(id, categories_csv).
- Async fetch on play (start_sponsorblock, worker thread + sb_sig_ to drop stale);
  poll_sponsorblock() installs segments. pump_async skips when position enters a segment
  (immediate seek to end), once per segment per play, with a "Skipped <category>" toast.
- Validated: C++ output byte-matches the reference API for 9bZkp7q19f0 (2 segments);
  live play test logs "[sponsorblock] skipped music_offtopic [0.0-4.0]".
- Remaining from the selected set: Captions (CC), Up-next + autoplay.

### Playback speed (2026-08-23)
- Player options menu (while playing) gains "Speed: Nx", Left/Right cycles
  0.25/0.5/0.75/1/1.25/1.5/1.75/2x. `Player::set_speed()` -> mpv "speed" property, live.
- Per-video: resets to 1x on each new video (request_playback), but persists across a
  quality re-resolve (replay_current). Applied after play() alongside volume.
- Verified headless (YTC_SPEEDTEST): row shows "1x", Right x2 -> "1.5x".
- Next up (user-selected feature set): SponsorBlock, Captions (CC), Up-next + autoplay.

### UI polish pass (2026-08-23)
Screenshot review + code audit; fixes in src/ui.{cpp,h}:
- Menu popups (render_menu): item labels now ellipsized to the panel width; the footer
  hint is reserved INSIDE the panel (no more grid bleed / last-row collision); Settings
  panel widened (620*s) so "Label: Value" rows have room; volume/hwdec added to the
  "Left/Right" hint set.
- Status banner unified into draw_status_banner(): width clamped to the screen and text
  ellipsized, so "Added <long channel> to favorites" can't run off both edges. Replaced
  5 copies (grid/carousel/3D/coverflow/player).
- Header (render_browse_chrome): count measured first, subtitle ellipsized to the exact
  gap so it never slides under "loading more..."; subtitle x clears the real logo width.
- Description overlay: opaque backdrop (video/controls no longer bleed through the inset
  margins over the player); top clip tightened so scrolled text can't bleed into title.
- draw_meta: 3-line tile text stacked by real line_height() instead of hardcoded 28/52*s
  (they overlapped on sub-720p / 480p handhelds where the font scale is floor-clamped).
- Search input: right-anchored (tail) view + always-visible caret for long queries.

### Clear History + keyboard tab-switch (2026-08-23)
- Clear History: options menu (Select) on a tile in the History view gains a
  "Clear History" item (guarded `view_label_ == "History" && !Playing`) ->
  `Innertube::clear_history()` deletes history.json, view reloads empty. Not shown
  in other views or during playback.
- Tab switching: gamepad L/R shoulders already call `cycle_tab` (device pad maps
  leftshoulder:b6/rightshoulder:b7); added keyboard Q/E as the L/R equivalent for
  local testing. Top-strip d-pad Left/Right still works; both no-op during playback.

### Video Decode toggle in Settings (Hardware/Software) (2026-08-23)
- Need: the Intel-VAAPI-only artifacts on the laptop dev machine (see below) made a
  user-facing decode switch worth having; also useful if a specific device's hwdec is
  flaky. `src/player.{h,cpp}`: `set_hwdec(mode)` stores the mpv `hwdec` string applied
  at the NEXT init (hwdec must be set before mpv_initialize). Precedence in init():
  `YTC_HWDEC` env (dev override) > app setting > built-in default. `auto-copy-safe`
  is still the default -> DEVICE BEHAVIOR UNCHANGED unless the user flips it.
- `src/ui.{h,cpp}`: "Video Decode: Hardware/Software" row in Settings (`hwdec_mode_`,
  persisted "hwdec": 0 hardware / 1 software), Left/Right to toggle. Hardware -> mpv
  "auto-copy-safe" (rkmpp on RK3588, and on H700 stock libmpv it falls back to software
  anyway); Software -> "no". Toggling WHILE a video plays re-resolves it at the current
  position (hwdec is fixed at init, so a fresh stream is needed to apply it live).
- ARM impact: default Hardware = today's behavior. Choosing Software on RK3588 drops
  hardware accel (fine for 1080p H264, drops frames on 4K/high-fps); H700 unchanged.
- Verified headless (`YTC_SETTINGSSHOT`): row shows "Hardware", Right -> "Software",
  settings.json gains "hwdec": 1.

### App-local volume control (independent of system mixer) (2026-08-23)
- Need: some videos are quiet at a comfortable system level; raising the OS volume to
  compensate makes everything else too loud. Wanted per-app volume, boostable.
- `src/player.{h,cpp}`: `set_volume(int %)` / `volume()` drive mpv's `volume` property
  (softvol). `volume-max=150` in init so quiet videos can be amplified above 100%.
  Applied right after every `player_.play()` (covers first play AND quality re-resolve).
- `src/ui.{h,cpp}`: `volume_` (0..150, persisted as settings.json "volume", default 100).
  Up/Down during playback -> `adjust_volume(±5)`: sets mpv live, persists, and pops a
  centered "Volume NN%" overlay (bar on a 0..150 scale with a tick at the 100%
  reference) that fades after ~1.4s. Also a "Volume: NN%" row in the Settings menu,
  adjusted with Left/Right like the other settings (live if a video is playing).
- Verified headless (`YTC_VOLTEST=<id>`): Up x3 -> "Volume 115%" (fill past the
  100% tick), Down x8 -> "Volume 75%" (fill below it). Persists across videos.

### Auto-retry with incremental backoff on network failure (2026-08-23)
- Symptom: waking the device from sleep and opening the app before wifi reconnected
  failed silently — empty grid, no indication anything was wrong, no recovery.
- Root cause: the browse/refresh path treats a failed fetch as "empty feed" and stops.
  There was no distinction between "network down" and "genuinely nothing here".
- Fix (`src/ui.{h,cpp}`): the async refresh now reports success via `Feed::ok` (Home
  kinds use `Innertube::has_visitor_data()` as the network-reachable proxy; channel/
  playlist/search feeds already return `ok=false` on failure). In `poll_refresh`, a
  failed fetch schedules a retry with exponential backoff (1,2,4,8,16s, capped 30s;
  `retry_attempt_`/`retry_at_`/`retry_pending_`). `pump_async` fires the retry via
  `refresh_current_view(is_retry=true)` once `retry_at_` elapses (skipped while a
  video is Playing so it never fights the player). A successful fetch resets the
  backoff. The grid empty-state and browse-empty overlay show "Waiting for
  network... / Reconnecting automatically" while a retry is pending.
- Verified headless: (1) dead-proxy start -> "Waiting for network..." shown, backoff
  attempts fire at 1/2/4/8s (logged); (2) flaky proxy that refuses the first 6s then
  works -> app starts empty, a later backoff retry succeeds and Home populates (55
  results) with no restart or user action. Note: sandboxed *background* tasks get
  isolated loopback, so the app must run foreground to reach a host-namespace proxy.

### Startup "Latest" feed (RSS) + empty state (2026-08-22)
- YouTube anonymous defaults are GONE: FEtrending -> 400, FEwhat_to_watch -> empty
  ("sign in" nudge) across WEB/TV/ANDROID/IOS. So there's no YouTube-provided default
  video list without login. Decision (user): DON'T ship opinionated default channels;
  default to an empty state prompting search.
- Mechanism kept for the future favorites/subscriptions feature: `Innertube::latest()`
  fetches each favorite channel's public RSS (youtube.com/feeds/videos.xml?channel_id=)
  — no login/key, no bot wall — parses Atom entries (videoId/title/author/published),
  merges NEWEST-FIRST, canonical mqdefault thumbnails. Guarded (never throws).
  Favorites live in `config/channels.json`, which SHIPS EMPTY.
- Startup: query arg -> search; no arg -> load_latest() (reads favorites). Empty ->
  clean empty state ("No videos yet / Press Y to search / Latest fills in once you add
  favorite channels"). Header shows "Latest". Verified: empty by default; adding 2 test
  channels produced a correct merged newest-first feed (reverted to empty after).
- NEXT (future): favorites UI to add/remove channels; login -> real subscriptions feed
  the same latest() path. RSS has no duration -> cards show no pill for feed items.

### Channels in search + favorites loop + video age (2026-08-22)
- Search now returns CHANNELS + videos (like the app): parser collects channelRenderer
  AND videoRenderer in document order (channels first). SearchResult gained kind
  (Video/Channel), channel_id (video: uploader; channel: itself), subs_text, and the
  thumbnail doubles as the channel avatar (protocol-relative -> https:; avatars decode
  as JPEG). Video results also carry their uploader channel_id (for favoriting).
- Channel tile: centered avatar (or initial-letter placeholder), name, sub count,
  "Channel · A: open". Selecting a channel (A) opens a one-level "channel view" =
  that channel's uploads via RSS latest({id}); B pops back to the prior results.
- Options menu (Select button / M key) on a highlighted tile OR the open/playing
  video — replaces the old direct X-favorite. Context items: "Add/Remove Channel
  Favorite" (channel + video), "Add/Remove Watch Later" (video), "Go to Channel"
  (video in grid). render_menu = dimmed overlay + item list; A activates, B closes.
  Favorites still drive the Latest feed; favorited items show a "FAV" badge.
- Watch Later persisted in config/watch_later.json (Innertube add/remove/ids, newest
  first); wl-id cache in App. (Viewing the WL list is a future menu/nav item.)
- Verified: menu overlay renders per-context; favorite write + revert confirmed.
- Video AGE now shown as a 3rd meta line "views · age" (humanize_age: search's
  "3 months ago" passes through; RSS ISO -> relative). meta_h bumped to fit.
- Search sort stays RELEVANCE (user decision; the CAI= date-sort param no longer
  reorders reliably on current YouTube anyway).

### Channel info + async fetch (2026-08-22)
- Channel tile now: name / subscriber count / @handle (removed "Channel" text). NOTE:
  search channelRenderer has NO video count — only subs + handle (in oddly-named
  subscriberCountText/videoCountText fields).
- Video count (and full metadata) IS available from the channel's /browse page:
  Innertube::channel_info(id) parses channelMetadataRenderer (name, description) +
  pageHeaderViewModel/contentMetadataViewModel metadataRows ("21.1M subscribers",
  "528 videos", "@handle"). Uses a LOCAL HttpClient (thread-safe) + cached visitorData.
- Fetched ASYNC on open_channel: RSS video list loads first (as before); a bg thread
  fetches channel_info; poll_channel_info (GL thread) fills the channel-view header
  "<name> · <subs> · <videos>" when ready. Never blocks the UI/video load.
- BUG fixed: open_channel took `const SearchResult& ch` into results_[sel_], then
  set_results() move-replaced results_ -> ch dangled -> garbage channel_id (UTF-8
  crash in channel_info). Fix: copy id/name BEFORE set_results.
- find_key must be SCOPED (pageHeaderViewModel first) or it grabs a video lockup's
  contentMetadataViewModel instead of the header's.
- Channel TILE now shows the video count too (below subs): ChannelMetaCache (worker
  thread, mirrors ThumbCache) calls channel_info() per channel result and caches the
  "N videos" string; tile shows it when ready, @handle until then. Requested in
  set_results for channel results.

### Pagination / infinite scroll (2026-08-22)
- Search IS paginated via continuation tokens; channel Videos tab too. RSS Latest is
  NOT (fixed snapshot). YouTube's newer item format is `lockupViewModel` (channel/home
  tabs + continuations) — added a parser (contentId/title/metadataRows/duration badge).
- Innertube: Feed {items, continuation, endpoint, channel_id}. search_feed() /
  channel_feed() (Videos tab, params EgZ2aWRlb3PyBgQKAjoA) / continue_feed(). Unified
  collect_results() handles videoRenderer + channelRenderer + lockupViewModel.
  continue_feed uses a LOCAL HttpClient (worker-thread safe) + cached visitorData.
- App: cont_token_/endpoint_/channel_id cursor; maybe_load_more() triggers when sel is
  within the last 2 rows; poll_more() appends the next page (async, bg thread). "N
  results - loading more..." in the header. Cursor saved/restored across channel-view
  push/pop. Verified: search 26->106+, channel 30->104+.
- BONUS: channel view now loads via channel_feed (browse) instead of RSS -> videos now
  have DURATION + view count (RSS had neither) + pagination.

### Scroll-into-view fix (2026-08-22)
- BUG: selecting a bottom-row tile left it clipped by the footer. Cause:
  ensure_visible() computed cardh with the OLD meta height (74*s) while render used
  100*s -> under-scrolled. Fix: single grid_metrics() helper used by BOTH render_grid
  and ensure_visible (no more drift). ensure_visible reveals the full selected row
  above the pinned footer; row 0 snaps to top (header shows). Bottom limit is
  H - footer - pad (gap so the card + selection ring clear the footer, not flush).

### Scrolling header (2026-08-22)
- The top header (ytc / subtitle / count) now scrolls WITH the grid (drawn at
  y=-scroll_), so it's only visible at the top and slides off as you scroll. Card cull
  changed from `< hbar` to `< 0`. Footer hint bar stays pinned.

### Top-level menu + Watch Later / History / Favorites views (2026-08-22)
- Start button (gamepad) / Tab (keyboard) opens a top-level "Menu" overlay: Latest,
  Favorite Channels, Watch Later, History. Same overlay widget as the Select options
  menu, distinguished by MenuKind {Context, Main}; heading shows "Menu" for Main.
  New MenuActions Go{Latest,Favorites,WatchLater,History} route to the loaders.
- All three lists are OUR OWN local data (no login = no server-side lists): reads of
  channels.json / watch_later.json / history.json. Innertube gained pair accessors
  favorites()/watch_later()/history() returning (id, name|title), plus add_history().
- load_favorites() -> channel tiles (placeholder avatar + async video count via
  ChannelMetaCache); selecting one opens the channel. load_watch_later()/load_history()
  -> video tiles built from id+title (canonical i.ytimg mqdefault thumb; no duration/
  views/age since we only store id+title). Selecting plays.
- History auto-records in poll_resolve() when playback actually starts (add_history:
  move-to-front dedupe, capped 200) -> history.json.
- view_label_ drives the header ("Favorite Channels"/"Watch Later"/"History"); empty
  = Latest/search. Saved/restored across channel-view push/pop (prev_view_label_) so
  Back from a channel returns to the correct list header. Each view has its own empty
  state. Cleared by search()/load_latest().
- Verified headless (YTC_MAINMENUSHOT): menu + all three views render with seeded
  data; live on laptop with a scratch config. Note: em dash / non-ASCII still "???"
  (ASCII-only font atlas — still the pending Unicode item).
- Refinements: (a) FAV badge only on CHANNEL tiles now, not on videos from a favorite
  channel (was cluttering every video tile). (b) Start menu is reachable from ANYWHERE
  (grid/channel/player/loading), not just the grid; Go* items stop the player and drop
  to the grid before loading. (c) B NO LONGER QUITS at the root — it does nothing there,
  still pops channel views / cancels elsewhere. Quitting is now an explicit "Exit" item
  at the bottom of the Start menu -> App::wants_quit(), checked by the main loop.

### "Latest" -> "Home" + channel avatars + B-to-Home (2026-08-22)
- Renamed "Latest" to "Home" everywhere (menu item, header, empty state; method
  load_latest->load_home, MenuAction GoLatest->GoHome). App ALWAYS starts on Home now
  (no-arg launch); a CLI query arg still deep-links to search for testing.
- Channel avatars now render on Favorites tiles. channel_info() gained avatar_url
  (parses channelMetadataRenderer.avatar.thumbnails[].back(), falls back to
  pageHeaderViewModel image sources). ChannelMetaCache caches avatar alongside video
  count; render backfills the tile's empty thumbnail from chan_meta_.avatar(id) and
  requests it via ThumbCache. Search channel tiles already carried an avatar URL, so
  this only affects Favorites (which start from channels.json = id+name only).
- Back-fallback: B with nothing to go back to (search / Favorites / Watch Later /
  History) now returns to Home instead of doing nothing; on Home it's a no-op. Channel
  subview still pops one level first. Implemented in App::input's Back branch.

### Pause-on-menu over the player (2026-08-22)
- Opening a menu (Start main-menu OR Select options menu) while a video is playing now
  PAUSES it; closing without navigating away RESUMES. Player::set_pause(bool) added
  (mpv "pause" property; stub no-op in the no-libmpv build). App tracks menu_paused_:
  set true only if we actually paused (skips if the user had already paused), cleared on
  resume and on nav-away (Go*/Quit stop the player, so no resume). Resume happens in
  both close paths (overlay Back/Menu, and the context-toggle tail of menu_activate).
- Verified headless (YTC_PAUSETEST): playing=0 -> menu open=1 -> menu closed=0.

### Player controls: auto-hide/fade (2026-08-22)
- Tried shrinking the video into a top region with a persistent slim bar (render to a
  sub-rect / offscreen FBO texture) but it was overkill and placement was inconsistent
  between offscreen and the live window. REVERTED to full-screen video.
- Final design: full-screen video (player.render(W,H)), and the ORIGINAL bottom controls
  bar (title / progress+knob / time / hints / centered ‖ pause glyph) now fades. Alpha
  from controls_until_ deadline: shown while menu_open_ OR paused (pinned to now+2600ms
  each frame so it lingers then fades on resume), and revealed for 2600ms after any
  player input (seek/pause) and on playback start; 450ms fade tail. Everything in the
  bar multiplies by alpha (with_a). Plays untouched -> controls fade away.
- Reverted helpers removed: Player::render_region/render_to_texture/frame_texture,
  gfx::Renderer::textured_id. Kept Player::set_pause (pause-on-menu feature).
- Verified headless (YTC_PAUSETEST): faded playing frame (clean picture), paused
  frame (bar + ‖ glyph back). Built on Pi.

### Settings menu — Max Quality (2026-08-22)
- Start menu gained a "Settings" item (before Exit) -> Settings submenu (MenuKind::Settings,
  heading "Settings"). B from Settings returns to the main menu (not close). First setting:
  "Max Quality" — Select cycles Auto(0)/2160/1440/1080/720/480/360, rebuilds the label in
  place, and persists immediately.
- Persistence: Innertube::setting_int/set_setting_int over settings.json (key->int) in
  config_dir_. App loads max_height at startup (default 1080; settings.json overrides;
  YTC_MAXHEIGHT env still wins for testing) into play_prefs_.max_height, which already
  drives best_video() selection.
- Pause-flag fix: returning to the main menu from Settings must not reset menu_paused_
  (open_main_menu now only sets it when playing AND not already paused), else resume-on-close
  was lost. Verified headless (YTC_MAINMENUSHOT): Settings shows "Max Quality: 1080p",
  cycles to 480p, settings.json = {"max_height":480}.
- INTERACTION CHANGE: setting values now change with LEFT/RIGHT (not A). App::adjust_setting
  (action, dir) handles the cycle; Settings-kind menu routes Left=-1/Right=+1 to the
  highlighted item, rebuilds the label, persists. Steps reordered low->high
  {360,480,720,1080,1440,2160,Auto} so Right raises quality. A still nudges forward as a
  convenience. Menu footer shows "Left/Right: change   B: back" in Settings. Same mechanism
  will drive future bool (on/off) settings. Verified: Right x2 from 1080 -> 2160p.

### In-player quality change (re-resolve + resume position) (2026-08-22)
- Options menu (Select on a PLAYING video) now has a "Quality: <label>" row. Left/Right
  cycle it (shares adjust_setting + play_prefs_.max_height + settings.json). Changing it
  sets quality_dirty_; on menu close (B/Menu) the app re-resolves the CURRENT video at the
  new cap and resumes the saved position.
- Position resume: Player::play() gained start_seconds -> loadfile "start=<sec>" option
  (fresh mpv per video, so it begins there). App::start_resolve(id,title,start_pos)
  factored out of request_playback; replay_current(at) re-resolves now_playing_item_ at the
  current position; poll_resolve consumes resume_pos_ into play(). This position tracking is
  also the groundwork for the future "ask to resume" setting.
- A also nudges quality forward (consistent with Settings). Menu footer shows
  "A: select  Left/Right: quality  B: close" when a quality row is present.
- Verified headless (YTC_QUALTEST=dQw4w9WgXcQ): play @cap1080 -> itag137; open menu,
  Right -> 1440p, close -> re-resolve @cap1440 -> itag271 (VP9), still Playing, resumed.

### Stats for Nerds (2026-08-22)
- Player options menu (playing) gained "Stats for Nerds: Enabled/Disabled" toggle. It's a
  value row: Left/Right flips it (A does nothing, consistent with Quality); takes effect
  immediately, no re-resolve. Session-only (not persisted).
- Player::stats_lines() queries mpv: dwidth/dheight, estimated-vf-fps/container-fps,
  video-codec, hwdec-current (->"software" if none), video/audio-bitrate, audio-codec-name,
  frame-drop-count + decoder-frame-drop-count, demuxer-cache-duration, avsync. Added prop_i
  (INT64) + prop_str (STRING, mpv_free'd) helpers. Stub returns {} in no-mpv build.
- render_player draws a translucent top-left panel with the lines while enabled (updates
  live). Menu-open scrim dims it (drawn before the overlay). A-select no-op extended to
  ToggleStats; context footer -> "A: select  Left/Right: change  B: close" when any value row.
- Verified headless (YTC_STATSTEST): overlay shows res/fps, H.264, software, 3.64Mbps,
  aac 128kbps, dropped, buffer, A/V sync; toggle label flips to Enabled.
- PER-VIDEO: stats_for_nerds_ resets to false in request_playback (each NEW video) but is
  left untouched in replay_current, so it persists across a same-video quality re-resolve.

### Seek crash fix — fast-forward past end dropped to grid (2026-08-23)
- BUG: repeated fast-forward (Right) near the end overshot EOF; mpv END_FILE reason=EOF ->
  ended -> pump() false -> mode_=Grid (looked like a crash back to the selection screen).
- Fix: Player::seek_relative now seeks to a CLAMPED ABSOLUTE target (time-pos+delta, floored
  at 0, capped at duration-1.0s) instead of a raw relative seek, so forward seeks can't hit
  EOF. Natural end-of-playback still EOFs correctly (only manual seeks are clamped).
- Verified headless (YTC_SEEKTEST=jNQXAC9IVRw, 18s clip): 12x FF -> still mode=2
  (Playing), no END_FILE. NOTE: headless hooks require YTC_SHOT set (headless=shot!=null).

### RESTRICTED (paced) delivery — the real seek-crash/hang root cause (2026-08-23)
- Investigated with direct curl probes (scratch probe1-8) against resolved URLs. FINDINGS:
  * SOME videos (e.g. kids content, Nd0ekBgPxEI "Pokemon TV") get RESTRICTED delivery:
    range requests beyond a sliding window 403. Window ~= 4% of clen (~20MiB video,
    ~1MiB audio) ahead of the furthest PACED sequential consumption; it does NOT grow
    with idle time nor with fast contiguous reads (anti-leech pacing; these URLs have no
    ratebypass param — but ratebypass is gone from ALL modern URLs, so not a detector).
  * Normal videos are unrestricted (deep ranges 206 everywhere) — via VISIONOS AND IOS.
    Restriction is PER-VIDEO, not per-client. For the restricted video, VISIONOS says
    UNPLAYABLE, TV says "page needs to be reloaded" (also: TV client can't fetch its own
    visitor_id, HTTP 400 — only works with another client's cached vd). Only IOS plays it.
  * Real iOS clients stream these via SABR/UMP (POST protocol) — out of scope.
- Symptom chain: seek beyond window -> 403 -> stream retries -> mpv re-requests forever
  (HANG) or gives up -> END_FILE EOF -> grid ("crash to selection").
- FIXES (all shipped):
  1. Seek DEBOUNCE (App): rapid Left/Right coalesce into one seek ~350ms after the last
     press; progress bar previews the target (pending_seek_/has_pending_seek_).
  2. Paced DETECTION (resolve thread): one 1KiB probe at clen*0.9; 403 => r.paced (skip
     for files < 24MiB). Banner "Limited seeking on this video (restricted stream)".
  3. Paced PLAYBACK (Player::play paced flag): demuxer-readahead-secs=15 + cache-secs=15
     so prefetch never outruns the window (unbounded prefetch hit the AUDIO wall at 1MiB
     during normal playback!). Non-paced keeps deep buffering.
  4. Paced SEEK CLAMP (pump_async): forward target <= played_max_ + (15MiB*8/vbitrate)s;
     clamped => status banner. replay_current (quality change) clamps resume into the
     fresh-URL window the same way. demuxer-max-back-bytes 16->48MiB (backward seeks
     served from mpv cache; server re-reads of far-behind ranges also 403).
  5. Stream retries with 250ms*n backoff remain as safety net.
- Verified headless: restricted video: paced=1, ZERO stream 403s, rapid seeks -> still
  Playing. Normal video: paced=0, unrestricted seeking unchanged.
- ROUND 2 (user still crashed live; multi-wave chained seeks reproduced it headlessly):
  chaining seek-to-cache-edge outruns the real-time pacing, so even SEQUENTIAL reads hit
  the wall eventually. Byte-math clamps can't model the opaque window. Final fixes:
  * Seek clamp is now CACHE-BOUNDED, not bitrate-math: forward seeks only within mpv's
    demuxer cache (Player::cached_until() = demuxer-cache-time, minus 2s), backward ~60s
    (served by the 48MiB back-buffer). No seek can ever trigger an out-of-cache read.
    Paced readahead 15s -> 25s so hops have somewhere to land.
  * Stream WALL = STALL, NOT ERROR: read_fn now retries every ~1.5s for up to 90s (the
    paced window advances ~real-time and the retries themselves count as paced reads —
    verified: reads succeed after 2-3 stall retries). mpv just shows buffering. Registered
    mpv_stream_cb cancel_fn (+ curl XFERINFO abort) so Back/stop/teardown interrupts a
    stalled read instantly (no frozen UI, no blocked join).
  * Verified: 5 waves of chained fast-forwards on the restricted video -> brief stalls,
    reads recover, mode stays Playing throughout.

### Restricted detection WITHOUT probing + "Hide Restricted Videos" setting (2026-08-23)
- FOUND a metadata marker: made-for-kids videos' playabilityStatus contains disabled
  miniplayer/offlineability buttons linking to support answer **9632097** (the
  "made for kids" help article). Validated: present for Pokemon TV + CoComelon (IOS
  responses), absent for normal videos on all clients. VideoInfo.made_for_kids set in
  try_client (ps.dump().find("9632097")). Resolve now uses the marker as the primary
  paced signal — deep-range probe only as fallback for unmarked >24MiB files.
- NO per-video signal exists at LIST time (search results carry no kids flag) and the
  channel /browse page is indistinguishable (isFamilySafe=true everywhere) — so per-tile
  upfront detection is impossible without a /player call per video (bot-bait). Design:
  LEARNED channel filter — restriction is channel-wide in practice; when a paced video
  plays, its channel_id+name persist to restricted_channels.json
  (Innertube::restricted_channel_ids/add_restricted_channel).
- Settings: "Hide Restricted Videos: On/Off" (setting hide_restricted, Left/Right value
  row). When ON, filter_restricted() drops known-restricted channels' videos AND channel
  tiles in set_results + poll_more (+ immediately filters the current list on toggle).
  First encounter still shows a restricted video; after one play its channel is hidden
  everywhere. Verified: kids search 20 -> 10 results with the learned channel hidden.
- Debug hooks: YTC_DUMP_PLAYER=<dir> dumps raw /player JSON per client;
  YTC_DUMP_BROWSE=<dir> dumps raw channel /browse JSON.

### Proactive restricted checks (v2 of the filter) (2026-08-23)
- REPLACED the learned-only restricted_channels.json with a PROACTIVE checker: when
  "Hide Restricted Videos" is ON, RestrictedCheck (worker thread, mirrors
  ChannelMetaCache) judges each UNIQUE channel in the current list via ONE
  Innertube::check_video_restricted() call (IOS-client /player on one of the channel's
  videos; 9632097 marker => 1, status OK no marker => 0, else -1 unknown/no-cache).
  Serial + 250ms pacing between calls (bot-wall caution). Verdicts cached in memory AND
  persisted to restricted_cache.json (both clean+restricted -> each channel checked ONCE
  ever). Checks run ONLY while the setting is on (no cost otherwise).
- Wiring: set_results/poll_more queue checks; pump_async drains dirty verdicts and
  re-filters the visible list live (tiles pop out as judged); toggle-on filters+queues
  immediately; play-time paced detection feeds rcheck_.put() too. FAVORITE channels are
  NEVER hidden (explicit intent overrides; un-favoriting re-applies the filter).
- Verified: kids search 20 results -> 7 after ~20s (29 channels judged: 28 restricted,
  1 "clean" = a knock-off channel that doesn't declare made-for-kids — inherent limit:
  the marker only catches DECLARED kids content). Mix/playlist tiles have no channel ->
  stay. WAITSHOT=<ms> headless hook added (settle + screenshot).
- **SEGFAULT fix (nlohmann pitfall)**: `for (auto& [k,v] : cfg.value("channels", obj).items())`
  dangles — the .items() proxy outlives the TEMPORARY json from value() -> crash on
  iteration (only when the file exists!). Bind the object to a local first, THEN .items().
  Range-for over value(...) directly (no .items()) is safe (lifetime-extended).

### Shorts support + Home feed v2 (Innertube, not RSS) (2026-08-23)
- Shorts are EXPLICITLY typed by the API: `shortsLockupViewModel` renderer (search Shorts
  shelves + channel Shorts tab), videoId at onTap/innertubeCommand/reelWatchEndpoint,
  title/views in overlayMetadata primary/secondaryText. NO date, NO duration, NO channel
  (chan_hint fills channel on channel tabs). Regular videoRenderer items are NEVER Shorts
  (verified: "minecraft" search = 25 shortsLockup + 9 videoRenderer, 0 overlap).
  collect_results now parses them -> SearchResult.is_short. Shorts SHOWN by default
  (search results now include shelf Shorts inline; no badge per user).
- "Hide Shorts: On/Off" setting (hide_shorts, default Off) — filter_restricted renamed
  filter_hidden and applies BOTH rules (restricted-channel rule still exempts favorites;
  Shorts rule doesn't). Verified: Home 60 -> 40 results with hide_shorts=1.
- HOME FEED v2: load_home now uses Innertube::home_feed() instead of RSS latest():
  per favorite channel, 3 PARALLEL jobs (4 workers) — Videos tab browse (rich: duration/
  views/age; params EgZ2aWRlb3PyBgQKAjoA), Shorts tab browse (params
  EgZzaG9ydHPyBgUKA5oBAA==), and RSS (exact ISO dates). Shorts are dated by matching ids
  in RSS (undated/older Shorts dropped); videos get RSS ISO date when available else
  relative text. Merged, sorted by approx_age_secs() (parses ISO via timegm AND relative
  "3 weeks ago"), caps: 10 videos + 5 Shorts per channel, 60 total. latest() (RSS) kept
  as legacy/fallback API. channel_feed refactored -> browse_tab(channel_id, params) with
  LOCAL HttpClient (thread-safe; visitor data pre-warmed once before workers).
- BUILD: ytcore now spawns threads -> Threads::Threads made PUBLIC on ytcore (ARM link
  of yt_search/yt_resolve/yt_play broke with "libpthread DSO missing").
- Debug: YTC_DUMP_SEARCH=<dir> dumps raw /search JSON; WAITSHOT=<ms> settle+shot.
- TOGGLE-OFF FIX: hide filters are destructive (items erased in place), so turning a
  hide setting OFF now calls refresh_current_view() — re-fetches whatever list is
  showing (channel view via channel_feed, label views via their loaders, Home via
  home_feed, else search(query_)). Verified round-trip (YTC_TOGGLETEST):
  Home 60 -> 40 (hide on) -> 60 with Shorts restored (hide off).
### Playlists (2026-08-23)
- Search returns playlists as lockupViewModel with contentType LOCKUP_CONTENT_TYPE_PLAYLIST:
  contentId "PL..." (real playlist) or "RD..." (Mix/radio — NOT browsable, endless; now
  SKIPPED — they previously leaked as broken video tiles with id=RD...!). Fields: title in
  lockupMetadataViewModel, owner = first plain metadataRow part, "N videos" in the
  thumbnailBadge, cover thumb video id extracted from the collectionThumbnail URL (raw URL
  has sqp= => WebP; use canonical /vi/<id>/mqdefault.jpg).
- SearchResult: Kind::Playlist + playlist_id + is_playlist(). parse_lockup also now
  captures AUTHOR for video lockups (first non-views/ago part — playlist pages name the
  uploader per row).
- Innertube::playlist_feed(id) = /browse of "VL"+id, NO params; items are plain video
  lockups (title/author/views/age/duration) parsed by the existing collect_results;
  paginated via the generic continuation (continue_feed works untouched). browse_tab
  generalized: (browse_id, params-nullable, chan_hint) — playlist rows carry no chan_hint.
- UI: playlist tile = cover + "N videos" pill + title/owner/"Playlist". A opens
  open_playlist(): same push/pop subview as channels (subview_playlist_ marks the kind;
  no channel-info fetch; header = playlist title). Back pops. Pagination + async
  refresh_current_view work in playlist views (Kind::Playlist branch). request_playback
  guards empty video_id; options menu skipped on playlist tiles for now.
- Verified (YTC_PLAYLISTTEST): search shows playlist tile ("960 videos"), open ->
  100-item grid w/ full metadata, Back pops. Mix tiles gone from search.

### Channel tabs (All / Videos / Shorts / Playlists) + view back-STACK (2026-08-23)
- Channel views have a tab strip between header and grid. Tab sources (browse params):
  All = no params (home/featured, mixed shelves), Videos = EgZ2aWRlb3PyBgQKAjoA,
  Shorts = EgZzaG9ydHPyBgUKA5oBAA==, Playlists = EglwbGF5bGlzdHPyBgQKAkIA. All parse via
  the existing collect_results (incl. playlist + shorts lockups). Default tab = All.
- Nav: Up from the grid's top row focuses the strip (grid ring hidden); Left/Right
  switch+fetch tabs; Down/A drops back into the grid. Empty tab -> tab_focus_ stays so
  tabs remain reachable (handled BEFORE the n==0 early-return in input()). Empty-state
  text for tabs: "Nothing in this tab / Left/Right: switch tabs".
- RESIZE-SAFE: strip geometry lives in grid_metrics() (tabs_h = 52*s, m.top includes it)
  and is recomputed every frame from live W/H — same as the rest of the grid.
- Async refresh_current_view refreshes the ACTIVE tab (captures chan_tab_).
- BACK-STACK: replaced the single prev_* slot with a proper std::vector<ViewState>
  (results/query/label/cont/subview/chan_tab/channel_info/sel/scroll). push_view() on
  open_channel/open_playlist; pop_view() on Back — so search -> channel -> Playlists tab
  -> playlist -> Back -> Back unwinds correctly (verified via YTC_TABTEST shots).
  Top-level loaders (search/home/favorites/WL/history) clear the stack.

### Views: shared tile renderer + carousel parity + 3D coverflow + L/R tabs (2026-08-23)
- REFACTOR: extracted draw_thumb() (kind-aware thumbnail area: video/short pill, channel
  avatar/letter, playlist cover+count, post image-or-text, FAV badge; alpha-tinted) and
  draw_meta() (3 kind-aware lines) — shared by grid/carousel/coverflow so parity is
  automatic. Grid loop now just calls both. Also render_browse_chrome(hy) = header +
  tab strip (grid passes -scroll_, carousel/coverflow pass 0). browse_empty_overlay()
  = shared centre loading/empty text.
- Carousel brought to full parity: real header+tabs, all tile kinds via draw_thumb, full
  metadata via draw_meta, empty/loading states, tab-focus aware, neighbor pills.
- NEW Coverflow view (render_coverflow): faux-3D horizontal flow — centre tile flat+large
  (full draw_thumb w/ pills+ring), side tiles rotated inward as perspective trapezoids
  (gfx::Renderer::quad4 / textured_quad4 added — arbitrary 4-corner quads), receding +
  dimming by distance. Centre metadata below.
- VIEW MODE: replaced bool carousel_ with ViewMode {Grid,Carousel,Coverflow}, persisted
  in settings.json "view". Settings row "View: Grid/Carousel/Coverflow" (Left/Right).
  toggle_view() (V key) cycles all three. render() dispatches on view_mode_.
- Non-grid input: Left/Right = prev/next item (1-D strip), Up = focus tabs, A = open.
- L/R SHOULDERS = switch tab (cycle_tab): LEFTSHOULDER prev / RIGHTSHOULDER next, on
  channel (0..4) and Home (0..3) tabbed views; keeps focus on content (tab_focus_=false).
  Was: LEFTSHOULDER=toggle_view (now V key / Settings only).
- Verified headless (YTC_VIEW=1/2, PRERIGHT): carousel + coverflow both render all
  tile types with metadata; Settings shows View row; grid pixel-unchanged.
- FOLLOW-UPS (2026-08-23): motion smoothed to a critically-damped spring
  (update_carousel_anim, SmoothDamp, dt-based) — fixed the "settles off-centre then
  snaps" (the 3D view's x had a sign-discontinuous centre-gap term; now x = cx + d*spacing,
  continuous). Split into TWO 3D views: "3D Carousel" (spread, render_carousel3d) and a
  NEW traditional "Coverflow" (render_coverflow: fixed inward tilt, stacked/receding,
  folds flat at centre). ViewMode now Grid/Carousel/Carousel3D/Coverflow (4). Coverflow
  perspective flipped so side cards face INWARD (inner edge tall/near); 3D Carousel left
  facing outward per user. CONFIRMED all 4 views working on-device (muOS .248, 2026-08-23).

### Async startup (2026-08-23)
- Startup felt slow with favorites: load_home() called home_feed() SYNCHRONOUSLY (N
  favorites x 3 network calls: Videos-tab browse + Shorts-tab browse + RSS) BEFORE the
  first frame. Fixed: load_home now clears state + kicks the fetch via
  refresh_current_view() (async RK_HOME) and returns immediately -> window draws
  instantly (header/tabs/footer + "Loading..."), poll_refresh fills home_items_ +
  apply_home_tab when the feed lands. Empty-state shows "Loading..." (no subtext) while
  refresh_running_. Everything else at startup was already cheap (file reads) or async
  (thumbs, chan_meta, restricted checks). Verified: 400ms shot = full chrome + Loading;
  5s shot = 55 tiles populated. CONFIRMED fast on-device (muOS .248, 2 favorites,
  2026-08-23) -> NO startup splash needed (UI-instant + "Loading..." is enough).

### Ask to Resume (2026-08-23)
- Per-video resume positions persisted in resume.json (Innertube resume_pos/
  set_resume_pos/clear_resume_pos; array [{id,pos}], move-to-front, cap 300).
- save_resume_position() writes player_.position() when LEAVING playback (Back, and
  Start-menu nav-away) if 15s < pos < dur-15s; else clears (too early / basically done).
  Natural EOF in pump_async clears (finished). Quality re-resolve (replay_current) does
  NOT touch it — separate resume_pos_ mechanism.
- request_playback: if ask_resume_ && saved pos > 15s, opens a modal resume prompt
  (resume_prompt_*) BEFORE resolving: "Resume from M:SS" / "Start from beginning"
  (Up/Down/Left/Right toggle, A confirm, B cancel=don't play). Resume -> start_resolve
  at the saved pos (uses the existing start-position path); Start over -> clear + play
  from 0. Rendered topmost (render_resume_prompt), input consumed at top of App::input.
- Setting "Ask to Resume: On/Off" (ask_resume, default ON) in Settings; value row
  (Left/Right), persisted. Verified (YTC_RESUMETEST): play->seek 28.6s->Back saves
  pos; replay shows the prompt; Resume -> Playing.

### Show Description (2026-08-23)
- /player's videoDetails.shortDescription is the FULL plain-text description (misnomer);
  now parsed into VideoInfo.description -> ResolveResult -> now_playing_desc_ (free for
  the playing video). Innertube::video_description(id) = one lightweight IOS /player
  call (local HttpClient, thread-safe) for grid tiles.
- Options menu gained "Show Description" on video items (grid tiles AND the playing
  video). Overlay: near-fullscreen panel, greedy word-wrap honoring \n with hard
  UTF-8-boundary breaking for long URLs, Up/Down scrolling (3 lines/press) + scrollbar,
  wrapped-lines cache keyed by wrap width (re-wraps on resize). Grid path fetches async
  ("Loading description..."), poll_description applies; stale results discarded if
  closed. Over the player it inherits the menu's auto-pause (desc_paused_) and resumes
  on close. B/A/Select close. Chapters/timestamps show as plain text (often present in
  descriptions). Verified both paths (YTC_DESCTEST screenshots).

- PLAYLISTS too: playlist options menu gained "Show Description";
  Innertube::playlist_description(id) = VL /browse -> playlistMetadataRenderer.description
  (plain string; also present in microformat + sidebar). open_description branches on
  is_playlist for the async fetch. SHORTS already worked (they're regular videos —
  same /player path). Verified (YTC_PLDESCTEST): "350+ best lofi songs" overlay.

- "Show Channel Description" on video rows INSIDE A PLAYLIST screen only (gate:
  !subview_playlist_.empty() — playlist rows mix uploaders). Same overlay; fetches
  channel_info(id).description async. ENABLER: parse_lockup now extracts the uploader's
  channel browseId from the author part's commandRuns (playlist rows carry it) ->
  playlist rows also gained Add-Channel-to-Favorites + Go-to-Channel in their menu.
  Verified (YTC_CHDESCTEST): row menu shows 5 items; overlay shows the channel's
  description.
- "Show Channel Description" WIDENED to ALL tiles (2026-08-23): every video tile with a
  known channel_id (search/home/channel/playing), playlist tiles (owner browseId now
  extracted in the playlist lockup parse too), and channel tiles (labeled just "Show
  Description" there; routes to the same handler with t.title as the name).
- "Show Playlist Description" also on playlist-screen rows: synthesizes a Playlist
  SearchResult from subview_playlist_ + query_ (the view's title) and reuses
  open_description's playlist path. Row menu is now: Favorites / Watch Later /
  Show Description / Show Channel Description / Show Playlist Description / Go to Channel.

### Channel Posts tab (2026-08-23)
- Channel views have a 5th tab: Posts (community). Innertube::channel_posts_feed(id) =
  browse params "Egljb21tdW5pdHnyBgQKAkoA" -> backstagePostRenderer items, paginated via
  the generic continuation. Anonymous read-only (no like/comment/vote).
- Parse (collect_results): SearchResult Kind::Post with post_text (full, contentText
  runs joined), title = ~200-byte preview (UTF-8-boundary cut), view_count_text =
  "<N> likes" (voteCount.simpleText), published_text, thumbnail = post image
  (backstageImageRenderer, PNG on yt3.ggpht — stb-decodable; //-prefixed URLs get
  https:) else attached video's mqdefault; video_id = attached videoRenderer's id.
  IMPORTANT: recursion skips the backstagePostRenderer subtree (its attached
  videoRenderer would otherwise duplicate as a bare video row).
- Tab strip is 5 tabs for channels, still 4 on Home (ntabs by context; input bound
  `last` likewise). Async tab switching covers Posts (RK_CHANNEL tab==4).
- Post tile: image thumb if present, else the post text word-wrapped INSIDE the thumb
  area; line1 = preview, line2 = "likes - age", line3 = "Post"/"Post - has video".
- A on a post = full text in the description overlay (instant — text already local;
  title "Post - <age> - <N likes>"). Select menu: "Read Post" + "Play Attached Video"
  (request_playback works — the post row carries the attached video_id).
- Verified (TABTEST extended): Kurzgesagt Posts tab = 9 posts w/ likes/ages.
- Post tiles never show "loading...": the text preview fills the thumb area whenever the
  image isn't AVAILABLE (absent, downloading, or failed); the image paints over it once
  the texture is ready.

### Hold-to-seek (2026-08-23)
- Holding d-pad Left/Right during playback now scrubs continuously: main_ui tracks
  CONTROLLERBUTTONDOWN/UP for DPAD_LEFT/RIGHT (controller buttons don't auto-repeat)
  and, after a 350ms hold, injects a seek press every 120ms (50ms once held >2.5s ->
  ~83 then ~200 media-seconds per held second). Feeds the existing accumulate+debounce
  scrubber: the on-screen target/progress preview races while held; ONE absolute seek
  fires ~350ms after release (also keeps paced-stream request patterns tame). Hold
  cancels if the mode leaves Playing or a menu opens. Keyboard arrows already repeat
  via the OS. Acceleration constants live in the main loop (350/120/50/2500).

### Playlists in Watch Later (2026-08-23)
- watch_later.json entries can now be PLAYLISTS: {id, title, playlist:true, thumb,
  author, count} (videos unchanged). add_watch_later gained is_playlist/thumb/author/
  count params; watch_later() now returns ready-to-show SearchResults (kind set, playlist
  tiles carry their "N videos" count + owner); load_watch_later uses it directly.
- Options menu on a playlist tile now offers Add/Remove Watch Later (was: no menu).
  WatchLaterToggle generalized: id = playlist_id|video_id, stores tile metadata at add
  time so the WL view renders full playlist tiles offline. A on a WL playlist opens it.
- Verified (YTC_WLPLTEST): search -> playlist tile -> menu Add -> WL view shows
  playlist tile ("960 videos", owner, Playlist) -> A opens its 100-video grid.

### Unicode text rendering (2026-08-23) — no more "????"
- gfx::Font rewritten: UTF-8 decode (utf8_next, 1-4 byte seqs, bad bytes -> U+FFFD) +
  ON-DEMAND glyph cache. No more eager ASCII-95 bake: any codepoint the .ttf covers is
  rasterized on first use (stbtt_FindGlyphIndex/GetGlyphBitmapBox/MakeGlyphBitmap) into
  a 1024x1024 atlas via a shelf packer + Texture::update (glTexSubImage2D; GL thread
  only — text/text_width/ellipsize all run on the render thread). Atlas full -> reset
  packing, glyphs re-cache lazily (rare).
- MISSING glyphs: emoji-ish codepoints (>=U+1F000, 0x2600-27BF, 0x2B00-2BFF, VS16s,
  ZWJ, keycap) are SKIPPED silently (titles read clean without decorations); other
  missing (e.g. CJK — DejaVu has none) render as tofu U+25A1. Sentinel Glyph{w=-1}
  caches the skip. DejaVu covers Latin+accents+Greek+Cyrillic+punctuation.
- ellipsize now breaks at CODEPOINT boundaries (byte-wise truncation used to split
  UTF-8 sequences -> mojibake at the "..." cut). Renderer::text uses the real atlas
  size (was hardcoded 512). Font keeps ttf bytes + opaque stbtt_fontinfo* (gfx.h stays
  stb-free); ~Font frees it.
- Verified: "Pokémon"/"POKÉMON" (é/É), "Kurzgesagt – In a Nutshell" (en-dash), curly
  apostrophes render; ✨/💕 skipped; 宇宙 -> tofu boxes. 3 fonts x 1024^2 RGBA atlases.

### Home tabs + ASYNC tab switching (2026-08-23)
- Home has the same All/Videos/Shorts/Playlists strip. All/Videos/Shorts are INSTANT
  local filters of home_items_ (the unfiltered master from home_feed(), kept as a
  member; apply_home_tab() rebuilds results_). Playlists = Innertube::home_playlists()
  (channel_playlists_feed per favorite, parallel, ≤12/channel) fetched ASYNC on first
  visit and cached (home_playlists_/loaded flag; cache invalidated by load_home()).
- Channel tab switching is now ASYNC too: load_channel_tab sets chan_tab_ (highlight
  moves same-frame), clears the grid, and routes through refresh_current_view() — the
  same background-fetch machinery as the hide-toggle refresh. Header shows "loading..."
  while refresh_running_; empty-state shows "Loading...".
- refresh machinery: refresh_kind_ (RK_CHANNEL/RK_PLAYLIST/RK_HOME/RK_HOME_PLAYLISTS/
  RK_SEARCH) tells poll_refresh how to apply (RK_HOME updates home_items_ then
  apply_home_tab; RK_HOME_PLAYLISTS caches + shows). view_sig() includes the tab for
  channel views ("ch:<id>:t<n>") and Home playlists ("home:pl") so a stale fetch from a
  previous tab is discarded — and poll_refresh RE-KICKS a fetch if the current view is
  still empty after a discard (rapid tab flipping converges).
- Verified (YTC_HOMETABTEST): Home All 60 -> Shorts 20 (instant) -> Playlists 56
  (async, merged across favorites). Channel TABTEST still passes.
- PAGINATION STATUS (verified 2026-08-23, YTC_VIDPAGE hook): search ✅; channel
  Videos tab ✅ (30->60->90->120 on scroll; Shorts/Playlists tabs same mechanism);
  playlist contents ✅ (100/page). Channel ALL (featured) = one big page (~115) then
  END — featured continuations are shelf-style, no more items (natural end, not a bug).
  HOME = no pagination BY DESIGN (merged snapshot, 10 vids + 5 shorts per favorite,
  60 cap; no single continuation exists for a multi-channel merge). Home ALL contains
  videos+Shorts; playlists only in the Playlists tab (undated -> can't merge by time).
  Channel ALL contains whatever the channel FEATURES (Shorts yes; playlists if featured).
- AUTHOR FILL: channel-tab lockups (and channel Playlists rows) don't name their own
  uploader -> Home tiles lost the channel text after the RSS->Innertube switch. Fixed by
  filling empty authors from the known channel name at every source: home_feed +
  home_playlists (favorites() id->name map), open_channel initial fetch, poll_refresh
  RK_CHANNEL, and poll_more appends (all use the view's channel title). Verified: Home
  tiles show "Pokemon TV"/"Kurzgesagt...", Home playlists show "Veritasium".
- REGRESSION FIX: channel opened FROM a label view (e.g. Favorite Channels) kept
  view_label_ set; refresh_current_view's label shortcut then hijacked tab switches back
  to load_favorites(). Fix: open_channel/open_playlist clear view_label_ (the back STACK
  restores it on pop now), AND the label shortcuts are guarded with !in_channel_view_.
  Verified via YTC_FAVTABTEST (favorites -> channel -> Right = Videos tab).

- ASYNC: refresh_current_view() now fetches on a BACKGROUND thread (refresh_thread_ +
  poll_refresh in pump_async), so the settings-menu label updates the same frame and the
  grid repopulates when the fetch lands. view_sig() (ch:<id>/label:<x>/home/q:<query>)
  captured at spawn and compared at apply — a stale refresh (user navigated away) is
  discarded. Local label views (Favorites/WL/History) stay synchronous (file-only).
  search_feed switched to a LOCAL HttpClient (worker-thread safe). Applies to BOTH
  Hide Shorts and Hide Restricted toggles (shared code path). ~App joins refresh_thread_.

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
- [x] BUILD our own mpv+ffmpeg with rkmpp hwdec — DONE, see docs/MPV_BUILD.md.
      libmpv 2.1.0 (mpv 0.36) + ffmpeg 6.1, built native on Orange Pi 5. VERIFIED
      hwdec=rkmpp: 1080p60 H264 60fps/9-drop, 4K60 VP9 60fps/**1-drop** (vs 57 sw).
- [ ] Bundle the built stack into portmaster/port/libs.aarch64 + relink yt_ui;
      hwdec=rkmpp on Rockchip / software on Allwinner; verify on real devices.
      (SDL2 bundling still per docs/BUNDLING_POLICY.md.)
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
      YTC_MAXHEIGHT=480. Search + thumbnails (TLS) + GLES2 grid + full mpv
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
src/main_play.cpp     M2: SDL2+libmpv player (YTC_BENCH=<s> for headless bench)
third_party/json.hpp  nlohmann/json single header
```
