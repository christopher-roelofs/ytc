#include "player.h"

#ifdef YTC_HAVE_MPV
// ---------------------------------------------------------------------------
// Real implementation: libmpv render API into the current GLES2 context.
// ---------------------------------------------------------------------------
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <SDL.h>
#include "stream.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

namespace ui {

struct Player::Impl {
    mpv_handle* mpv = nullptr;
    mpv_render_context* rctx = nullptr;
    bool loaded = false;
    bool ended = false;
    std::string pending_audio;   // external audio URL to attach on file-loaded
    bool audio_added = false;
    std::string hwdec = "auto-copy-safe";   // decode mode (settings/env); see init()
    int aspect = 0;              // 0 Fit / 1 Zoom / 2 Stretch; see apply_aspect()

    static void* get_proc(void*, const char* name) {
        return SDL_GL_GetProcAddress(name);
    }

    bool init() {
        bool dbg = getenv("YTC_DEBUG");
        if (dbg) std::fprintf(stderr, "[mpv] create...\n");
        mpv = mpv_create();
        if (!mpv) return false;
        // We ALWAYS hand mpv a direct googlevideo URL from our own Innertube
        // client, so disable mpv's built-in ytdl_hook — otherwise it shells out
        // to youtube-dl/yt-dlp on its own (unwanted dependency; 403s + latency;
        // fails outright on the handheld where yt-dlp isn't installed).
        mpv_set_option_string(mpv, "ytdl", "no");
        // The render API (below) IS our video output: force vo=libmpv so mpv renders
        // into our GL context instead of spawning its own window (newer mpv on desktop
        // otherwise opens a separate player window), and config=no so a machine that
        // has mpv installed can't hijack us via a system/user mpv.conf (e.g. vo=gpu).
        mpv_set_option_string(mpv, "vo", "libmpv");
        mpv_set_option_string(mpv, "config", "no");
        // Reasonable defaults for streaming on ARM handhelds.
        mpv_set_option_string(mpv, "vd-lavc-threads", "4");
        // hwdec: default to *-copy so decoded frames are copied to normal
        // textures rather than imported as GPU surfaces into our render-API GL
        // context. Direct surface import (plain "vaapi"/"auto-safe") can produce
        // corrupt/artifacted frames on some drivers (Intel VAAPI + render API).
        // Override with YTC_HWDEC (e.g. "no", "auto", "vaapi").
        // Env var wins (dev/debug escape hatch); otherwise the app-chosen mode
        // (from the Settings "Video Decode" toggle), defaulting to auto-copy-safe.
        const char* hw = getenv("YTC_HWDEC");
        mpv_set_option_string(mpv, "hwdec", hw ? hw : hwdec.c_str());
        mpv_set_option_string(mpv, "cache", "yes");
        // Allow app-local volume above 100% so quiet videos can be amplified
        // without touching the OS mixer (softvol).
        mpv_set_option_string(mpv, "volume-max", "150");
        mpv_set_option_string(mpv, "demuxer-max-bytes", "48MiB");
        // Generous back-buffer: restricted (paced) googlevideo streams 403 re-reads
        // of far-behind ranges, so serve backward seeks from mpv's own cache.
        mpv_set_option_string(mpv, "demuxer-max-back-bytes", "48MiB");
        if (dbg) std::fprintf(stderr, "[mpv] initialize...\n");
        if (mpv_initialize(mpv) < 0) { mpv_destroy(mpv); mpv = nullptr; return false; }
        if (dbg) std::fprintf(stderr, "[mpv] initialize done; creating render ctx...\n");

        mpv_opengl_init_params gl_init{ get_proc, nullptr };
        // NOTE: no ADVANCED_CONTROL — plain mode renders the current frame on
        // our thread with no update-callback threading (simpler + no deadlock).
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
            {MPV_RENDER_PARAM_INVALID, nullptr}};
        if (mpv_render_context_create(&rctx, mpv, params) < 0) {
            mpv_destroy(mpv); mpv = nullptr; return false;
        }
        if (dbg) std::fprintf(stderr, "[mpv] render ctx created\n");
        mpv_request_log_messages(mpv, dbg ? "v" : "error");
        ytn::register_stream(mpv);   // our bounded-range googlevideo fetcher
        apply_aspect();              // fresh instance per video; re-apply the mode
        return true;
    }
    // Fit  = keepaspect + no panscan: whole picture, black bars for non-16:9.
    // Zoom = keepaspect + panscan=1: scale until the screen is filled, cropping
    //        the overflowing edges (a 4:3 video loses some top/bottom).
    // Stretch = keepaspect off: fill the screen, distorting the picture.
    void apply_aspect() {
        if (!mpv) return;
        mpv_set_property_string(mpv, "keepaspect", aspect == 2 ? "no" : "yes");
        mpv_set_property_string(mpv, "panscan",    aspect == 1 ? "1.0" : "0.0");
    }
    void teardown() {
        if (rctx) { mpv_render_context_free(rctx); rctx = nullptr; }
        if (mpv)  { mpv_destroy(mpv); mpv = nullptr; }
        loaded = false; ended = false; audio_added = false;
    }
    ~Impl() { teardown(); }
    double prop_d(const char* name) const {
        double v = 0; mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &v); return v;
    }
    int prop_flag(const char* name) const {
        int v = 0; mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &v); return v;
    }
    long long prop_i(const char* name) const {
        int64_t v = 0; mpv_get_property(mpv, name, MPV_FORMAT_INT64, &v); return (long long)v;
    }
    std::string prop_str(const char* name) const {
        char* s = nullptr;
        if (mpv_get_property(mpv, name, MPV_FORMAT_STRING, &s) < 0 || !s) return "";
        std::string r(s); mpv_free(s); return r;
    }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
bool Player::available() { return true; }

bool Player::play(const std::string& video_url, const std::string& audio_url,
                  const std::string& user_agent, double start_seconds, bool paced) {
    // Fresh mpv instance per video: reusing one across videos leaks decoder /
    // hwdec surface-pool state, which corrupts the 2nd+ video (artifacts).
    impl_->teardown();
    if (!impl_->init()) return false;
    mpv_set_option_string(impl_->mpv, "user-agent", user_agent.c_str());
    if (paced) {
        // Restricted delivery: the server 403s reads beyond a sliding window that
        // advances only with (roughly real-time) consumption. Keep the demuxer's
        // readahead modest so prefetch stays inside the window; deep buffering
        // would race ahead and hit the wall (esp. the small audio stream window).
        mpv_set_option_string(impl_->mpv, "demuxer-readahead-secs", "25");
        mpv_set_option_string(impl_->mpv, "cache-secs", "25");
    }
    // Route both streams through our ytc:// protocol (bounded-range libcurl
    // fetch) so iOS-issued URLs — which 403 on ffmpeg's open-ended ranges —
    // play. Live HLS manifests (.m3u8) must NOT be wrapped: mpv handles those.
    bool is_hls = video_url.find(".m3u8") != std::string::npos ||
                  video_url.find("/manifest/hls") != std::string::npos;
    // A local file (offline download): no scheme -> hand the raw path to mpv, unwrapped
    // (the ytc:// range-fetcher only speaks http). The .mp4 is muxed, so no audio track.
    bool is_local = video_url.find("://") == std::string::npos;
    std::string vurl = (is_hls || is_local) ? video_url : ytn::wrap_url(video_url, user_agent);
    impl_->pending_audio = (audio_url.empty() || is_hls || is_local)
                           ? audio_url : ytn::wrap_url(audio_url, user_agent);
    impl_->audio_added = false;
    // Optional resume point: pass mpv a "start=<sec>" loadfile option so it begins
    // there (used when re-resolving the same video at a new quality).
    std::string opts;
    if (start_seconds > 0.5) {
        char b[64]; std::snprintf(b, sizeof b, "start=%.3f", start_seconds); opts = b;
    }
    std::vector<const char*> cmd = {"loadfile", vurl.c_str(), "replace"};
    if (!opts.empty()) cmd.push_back(opts.c_str());
    cmd.push_back(nullptr);
    if (mpv_command(impl_->mpv, cmd.data()) < 0) return false;
    impl_->loaded = true;
    impl_->ended = false;
    return true;
}
void Player::stop() {
    if (!impl_->mpv || !impl_->loaded) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(impl_->mpv, cmd);
    impl_->loaded = false;
}
bool Player::pump() {
    if (!impl_->mpv) return false;
    bool dbg = getenv("YTC_DEBUG");
    while (true) {
        mpv_event* e = mpv_wait_event(impl_->mpv, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        if (e->event_id == MPV_EVENT_END_FILE) {
            // Only a natural EOF or a decode ERROR counts as "playback ended".
            // A STOP (reason 2) is ALWAYS our own doing — our stop() or a new
            // loadfile replacing the previous file — and such an event can leak
            // from the previous video into this session; treating it as an end
            // would instantly kill the newly-started video. So ignore STOP/QUIT.
            auto* ef = (mpv_event_end_file*)e->data;
            if (ef->reason == MPV_END_FILE_REASON_EOF ||
                ef->reason == MPV_END_FILE_REASON_ERROR) {
                impl_->ended = true;
                if (dbg) std::fprintf(stderr, "[mpv] END_FILE reason=%d -> ended\n", ef->reason);
            } else if (dbg) {
                std::fprintf(stderr, "[mpv] END_FILE reason=%d (ignored)\n", ef->reason);
            }
        }
        if (e->event_id == MPV_EVENT_FILE_LOADED &&
            !impl_->pending_audio.empty() && !impl_->audio_added) {
            // "audio-add <url> select" attaches the external track and selects it.
            const char* acmd[] = {"audio-add", impl_->pending_audio.c_str(),
                                  "select", nullptr};
            mpv_command(impl_->mpv, acmd);
            impl_->audio_added = true;
            if (dbg) std::fprintf(stderr, "[mpv] audio-add issued\n");
        }
        if (e->event_id == MPV_EVENT_LOG_MESSAGE && dbg) {
            auto* m = (mpv_event_log_message*)e->data;
            std::fprintf(stderr, "[mpv:%s] %s", m->prefix, m->text);
        }
    }
    return !impl_->ended;
}
void Player::render(int fb_w, int fb_h) {
    if (!impl_->rctx) return;
    mpv_opengl_fbo fbo{0, fb_w, fb_h, 0};
    int flip = 1;
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context_render(impl_->rctx, rp);
}
void Player::toggle_pause() {
    if (!impl_->mpv) return;
    const char* cmd[] = {"cycle", "pause", nullptr};
    mpv_command(impl_->mpv, cmd);
}
void Player::set_pause(bool paused) {
    if (!impl_->mpv) return;
    int flag = paused ? 1 : 0;
    mpv_set_property(impl_->mpv, "pause", MPV_FORMAT_FLAG, &flag);
}
void Player::seek_relative(double seconds) {
    if (!impl_->mpv) return;
    // Seek to a CLAMPED absolute target: a relative forward seek can overshoot the
    // end, which mpv reports as EOF -> we'd tear playback down to the grid. Keep a
    // small margin before the end so rapid fast-forwards never trigger that.
    double pos = impl_->prop_d("time-pos");
    double dur = impl_->prop_d("duration");
    double target = pos + seconds;
    if (target < 0) target = 0;
    if (dur > 0 && target > dur - 1.0) target = dur - 1.0;
    if (target < 0) target = 0;                 // very short clips
    char buf[32]; std::snprintf(buf, sizeof buf, "%.3f", target);
    const char* cmd[] = {"seek", buf, "absolute", nullptr};
    mpv_command(impl_->mpv, cmd);
}
void Player::set_volume(int percent) {
    if (!impl_->mpv) return;
    if (percent < 0) percent = 0;
    if (percent > 150) percent = 150;
    double v = percent;
    mpv_set_property(impl_->mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}
int Player::volume() const {
    if (!impl_->mpv) return 100;
    return (int)(impl_->prop_d("volume") + 0.5);
}
void Player::set_hwdec(const std::string& mode) {
    impl_->hwdec = mode.empty() ? "auto-copy-safe" : mode;
}
void Player::set_aspect(int mode) {
    impl_->aspect = (mode < 0 || mode > 2) ? 0 : mode;
    impl_->apply_aspect();   // live if playing; init() re-applies for later videos
}
void Player::set_speed(double mult) {
    if (!impl_->mpv) return;
    if (mult < 0.25) mult = 0.25;
    if (mult > 4.0) mult = 4.0;
    mpv_set_property(impl_->mpv, "speed", MPV_FORMAT_DOUBLE, &mult);
}
void Player::add_subtitle(const std::string& path) {
    if (!impl_->mpv || path.empty()) return;
    // Async: sub-add opens and parses the file before returning, which stalls the
    // UI thread for a visible beat on weak cores. Fire it and let mpv apply it
    // when ready — the caller only needs the toggle to feel instant.
    const char* cmd[] = {"sub-add", path.c_str(), "select", nullptr};
    mpv_command_async(impl_->mpv, 0, cmd);
    int on = 1; mpv_set_property(impl_->mpv, "sub-visibility", MPV_FORMAT_FLAG, &on);
}
void Player::subtitles_off() {
    if (!impl_->mpv) return;
    int off = 0; mpv_set_property(impl_->mpv, "sub-visibility", MPV_FORMAT_FLAG, &off);
}
bool Player::active() const { return impl_->loaded; }
bool Player::paused() const { return impl_->prop_flag("pause") != 0; }
double Player::position() const { return impl_->prop_d("time-pos"); }
double Player::duration() const { return impl_->prop_d("duration"); }
double Player::cached_until() const { return impl_->prop_d("demuxer-cache-time"); }

std::vector<std::string> Player::stats_lines() const {
    std::vector<std::string> out;
    if (!impl_->mpv || !impl_->loaded) return out;
    const Impl& I = *impl_;
    auto rate = [](long long bps) -> std::string {
        if (bps <= 0) return "n/a";
        char b[32];
        if (bps >= 1000000) std::snprintf(b, sizeof b, "%.2f Mbps", bps / 1e6);
        else                std::snprintf(b, sizeof b, "%lld kbps", bps / 1000);
        return b;
    };
    long long w = I.prop_i("dwidth"), h = I.prop_i("dheight");
    double fps = I.prop_d("estimated-vf-fps");
    if (fps <= 0) fps = I.prop_d("container-fps");
    std::string vcodec = I.prop_str("video-codec");
    if (vcodec.empty()) vcodec = I.prop_str("video-format");
    std::string hwdec = I.prop_str("hwdec-current");
    if (hwdec.empty() || hwdec == "no") hwdec = "software";
    std::string acodec = I.prop_str("audio-codec-name");
    double cache = I.prop_d("demuxer-cache-duration");
    double avsync = I.prop_d("avsync");
    long long dropped = I.prop_i("frame-drop-count");
    long long decdrop = I.prop_i("decoder-frame-drop-count");

    char b[160];
    std::snprintf(b, sizeof b, "Resolution: %lldx%lld @ %.2ffps", w, h, fps); out.push_back(b);
    out.push_back("Video codec: " + (vcodec.empty()?std::string("?"):vcodec));
    out.push_back("Decode: " + hwdec);
    out.push_back("Video bitrate: " + rate(I.prop_i("video-bitrate")));
    out.push_back("Audio: " + (acodec.empty()?std::string("?"):acodec)
                  + "  " + rate(I.prop_i("audio-bitrate")));
    std::snprintf(b, sizeof b, "Dropped frames: %lld (dec %lld)", dropped, decdrop); out.push_back(b);
    std::snprintf(b, sizeof b, "Buffer: %.1fs", cache); out.push_back(b);
    std::snprintf(b, sizeof b, "A/V sync: %+.3fs", avsync); out.push_back(b);
    return out;
}

} // namespace ui

#else
// ---------------------------------------------------------------------------
// Stub: no libmpv in this build (e.g. desktop UI iteration). Playback is inert.
// ---------------------------------------------------------------------------
namespace ui {
struct Player::Impl {};
Player::Player() = default;
Player::~Player() = default;
bool Player::available() { return false; }
bool Player::play(const std::string&, const std::string&, const std::string&, double, bool) { return false; }
void Player::stop() {}
bool Player::pump() { return false; }
void Player::render(int, int) {}
void Player::toggle_pause() {}
void Player::set_pause(bool) {}
void Player::seek_relative(double) {}
void Player::set_volume(int) {}
int Player::volume() const { return 100; }
void Player::set_hwdec(const std::string&) {}
void Player::set_aspect(int) {}
void Player::set_speed(double) {}
void Player::add_subtitle(const std::string&) {}
void Player::subtitles_off() {}
bool Player::active() const { return false; }
bool Player::paused() const { return false; }
double Player::position() const { return 0; }
double Player::duration() const { return 0; }
double Player::cached_until() const { return 0; }
std::vector<std::string> Player::stats_lines() const { return {}; }
} // namespace ui
#endif
