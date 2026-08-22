# Stream Resolution Strategy & Fallbacks

How ytnative turns a `videoId` into playable stream URLs, why it's built this way,
and what to do when YouTube breaks it. Read this before adding a JS engine, a
PoToken provider, or "just switching to the web client."

## What we do now

1. `POST /youtubei/v1/visitor_id` once per session → a `visitorData` token.
   **Mandatory** — without it every player request is bot-walled ("Sign in to
   confirm you're not a bot").
2. `POST /youtubei/v1/player` impersonating a **JS-less client**, trying each in
   order until one returns `playabilityStatus == OK`:
   - **VISIONOS 1.02** (client id 101) — primary; returns direct, unciphered URLs
     and full H264/VP9/AV1 ladder. Working as of 2026-08.
   - **TVHTML5**, **IOS** — fallbacks. IOS is the only client that returns some
     age/region/copyright-restricted videos, but its URLs need special handling
     (see `ytn://` below).
3. Fingerprints live in `config/clients.json` (name, version, UA, header id).
   **This is the maintenance surface.** When a client gets bot-walled, update the
   string(s) — crib current values from yt-dlp's
   `yt_dlp/extractor/youtube/_base.py`. No rebuild needed.
4. Playback fetches through our own `ytn://` libmpv stream (src/stream.cpp) using
   small bounded range requests (~1 MiB). Required for IOS URLs (they 403 on
   ffmpeg's open-ended ranges); harmless for the others. Also the natural home for
   future URL-refresh-on-403.

Why JS-less clients: they hand us URLs that are already unciphered and NOT
`n`-throttled, so we need neither signature-cipher nor n-transform — i.e. no JS
engine. Resolve is one visitor_id + one player POST, ~300 ms.

## Why NOT QuickJS / the web client (decision: skip it)

QuickJS's only job would be to enable the **WEB client**, whose URLs are
signature-ciphered (`s`→`sig`) and `n`-throttled — both requiring execution of
functions extracted from YouTube's obfuscated `base.js`. Assessment:

- **It's a more expensive fragility, not a safer one.** Our JS-less approach breaks
  cheaply (edit a config string). The web-client approach breaks in CODE: the
  regex/AST that *finds* the sig/n functions inside minified base.js is what
  YouTube reshuffles constantly — it's the highest-churn part of yt-dlp. We'd be
  chasing that in C++.
- **It doesn't solve the real future threat: PoTokens.** YouTube is pushing GVS
  **PoToken** (BotGuard attestation) onto more clients/videos. QuickJS + sig/n does
  nothing for this. PoToken generation needs a real browser JS env (Deno + big
  shim, or a headless browser) — far beyond QuickJS. So even after the QuickJS
  work, mandatory PoTokens break the web-client path too.
- **The industry moved toward JS-less, not away.** yt-dlp's own default clients are
  now the JS-less ones for exactly this reason. NewPipe still does sig/n (Android
  constraints) and breaks often.
- Security: running fetched base.js in QuickJS is sandboxed (safe from escape) but
  is a much larger, more dynamic attack surface than parsing JSON.

Robustness ranking, best→worst: (1) config-swappable JS-less clients [what we do],
(2) QuickJS + web-client sig/n, (3) hardcoded single client.

## Authenticated (logged-in) requests

Sending a real Google account's credentials with the Innertube requests (OAuth
token, or `SAPISID`/`__Secure-3PAPISID` cookies + an `Authorization: SAPISIDHASH`
header) is possible. Treat it as an **optional feature unlock, not a robustness
fix**, and default the app to anonymous.

What it gets you:
- Age-restricted, and your own private/unlisted videos.
- Personalized data: real subscriptions, watch history, playlists, "watch later".
- Somewhat gentler bot-walling — a logged-in session looks less like a bot, so it
  can be resolved when anonymous is being challenged. Premium accounts can unlock
  higher tiers / no mid-stream ads.

Why it's NOT the default, and the real caveats:
- **Account-ban risk (the big one).** Driving a real account through a non-official
  client (client impersonation, unusual traffic) risks Google flagging or
  terminating it. This is documented and real (yt-dlp warns about it). If we ever
  support login, use a THROWAWAY/secondary account, never a main one.
- **Credential security on a handheld.** OAuth tokens / Google cookies grant broad
  account access. Storing them on a device that can be lost or shared is a genuine
  risk. Prefer session-only (in-memory) or, if persisted, scoped and clearly
  disclosed. Never sync them anywhere.
- **Onboarding is clunky.** YouTube restricted the old TV/device-code OAuth flow, so
  getting a token now usually means extracting cookies from a browser session —
  awkward on a handheld with no browser.
- **It does not bypass the hard parts.** sig/n and PoToken mechanics are largely the
  same logged-in; login can reduce bot-walling and unlock restricted content but is
  not a substitute for the client strategy above.

If/when implemented: strictly opt-in, throwaway account recommended, credentials
session-only by default, and it layers ON TOP of the JS-less client flow (add the
auth header to the same requests) — it does not replace it.

### Obtaining credentials without an on-device browser (verified 2026-08)

Split by what you're authenticating FOR:

- **Innertube / stream-resolution auth: NO non-browser path.** Google killed the
  OAuth2 device-code flow for this in 2024 (yt-dlp's youtube-oauth2 plugin is now
  OBSOLETE); username/password is dead too. Only working method is `--cookies`, i.e.
  a real browser session. You can run the browser on a PC (export cookies.txt, copy
  to device) — no browser ON the handheld, but a browser somewhere, once. Carries
  ban risk.
- **Subscriptions/playlists metadata: YES, cleanly.** The official YouTube **Data
  API** still supports OAuth 2.0 device flow for TV/limited-input devices (live
  Google feature). Register an app in Google Cloud Console; handheld shows a code;
  user authorizes on their phone; token reads subscriptions + playlists. No
  on-device browser, ToS-compliant, NO ban risk. BUT metadata-only (no stream auth,
  no age-restricted, no full history) and quota-limited.

=> Clean design: keep playback ANONYMOUS (robust default); optionally layer real
subscriptions via the official Data API + device flow (the no-browser, no-ban path).
Reserve cookie-based auth for users who explicitly want authenticated playback and
accept the risk.

## Runbook — when resolution breaks

1. **All videos fail with LOGIN_REQUIRED / "not a bot":** the primary client got
   bot-walled. Update VISIONOS (or the failing client's) version/UA in
   `config/clients.json` from yt-dlp `_base.py`; add/enable another JS-less client.
2. **Some videos fail, most work:** usually per-video (premiere/live → expected;
   iOS-only + 403 → check `ytn://` bounded-range path is active). Not a global
   strategy failure.
3. **Direct URLs 403 broadly / throttled:** a client's URLs may have started
   requiring a GVS PoToken. First try a *different* JS-less client (config). Only if
   ALL JS-less clients require PoToken is the heavyweight path warranted.
4. **Last resort (only if JS-less clients fully collapse):** a **PoToken provider** —
   pragmatically, shell out to an external token service / bgutil-style helper *when
   available*, degrade gracefully when not. Decide this the day it breaks; do NOT
   pre-build QuickJS-sig/n for it — it would not be sufficient anyway.

## Review cadence

YouTube's countermeasures shift every few months. If JS-less resolution is still
holding up, no action. Re-evaluate this doc whenever runbook step 1 or 3 starts
firing frequently. Last reviewed: 2026-08-22 (JS-less/VISIONOS healthy).
