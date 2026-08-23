# Dependency bundling policy

Principle: **bundle any library that is (a) not reliably present on the target
device, OR (b) whose version/behavior we need to control.** Only use the device's
copy for things that literally ARE the device.

Deciding question per library: *is it part of the device (kernel / GPU driver /
core system), or part of our app's behavior?*
- Device layer → use theirs (we can't or shouldn't override it).
- Behavior layer → bundle our own.

## Never bundle — it IS the device

These must match the running kernel/GPU/system; a bundled copy would break.

- **GPU / display stack:** libGLESv2, libEGL, libdrm, libgbm, the vendor Mali/mesa
  driver. Tied to the kernel + GPU; always the device's.
- **Core system:** libc, libm, libpthread, libdl. (We DO static-link libstdc++ and
  libgcc — those are toolchain, not "the system", and pinning them avoids ABI drift.)

## Bundle our own — not-likely-present OR must-control

- **libmpv + ffmpeg (libav*) — the big one.** Controls codecs, demuxers, protocols,
  hwaccel (rkmpp/v4l2 = hardware decode), and behavior (ytdl_hook, range handling).
  Not reliably present across CFWs and the SONAME/version differs (muOS ships
  `.so.2`/mpv 0.35; others vary or lack it). Both tests fire → **build and bundle our
  own mpv + ffmpeg with rkmpp/v4l2-request.** This single investment gives:
  CFW portability (no dependence on the device shipping libmpv) + hardware decode +
  full behavior control. Supersedes the current "link the device's libmpv.so.2"
  stopgap once built.
- **curl + mbedTLS + zlib — already static.** Innertube is HTTPS and we control cert
  handling (baked CA path). Keep bundled.
- **SDL2 — bundle (control).** Version drift already bit us (device 2.0.14 lacked
  SDL_HINT_VIDEODRIVER and the offscreen driver vs 2.30 on the dev box). Bundling a
  known-good SDL2 gives consistent window/KMSDRM/input/controller behavior across
  CFWs. (It still dlopens the device's GL/DRM/udev/ALSA at runtime — correct: SDL is
  behavior, the drivers under it are the device.) Pragmatic alternative: rely on the
  PortMaster runtime's SDL2; choose that only if binary size matters more than
  version control. Default: bundle.
- **Data / assets:** font (DejaVuSans.ttf), gamecontrollerdb(+local), config — bundle.
  Already done (handhelds don't ship the Debian font path, etc.).

## Current state vs. target

- Now (first port): static curl/mbedTLS/zlib + assets bundled; SDL2, GLESv2 and
  libmpv use the device's (libmpv via a copy of the device's `.so.2` at link time).
- Target: also bundle **our own mpv+ffmpeg (rkmpp/v4l2)** and **SDL2**. Then the
  binary's only device-supplied deps are the GPU/display stack + libc — exactly the
  "it IS the device" set. That maximizes portability and control, and unlocks hwdec.

Last updated 2026-08-22.
