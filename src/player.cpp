#include "player.h"

#ifdef YTNATIVE_HAVE_MPV
// ---------------------------------------------------------------------------
// Real implementation: libmpv render API into the current GLES2 context.
// ---------------------------------------------------------------------------
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <SDL.h>
#include "stream.h"
#include <cstdio>
#include <cstdlib>

namespace ui {

struct Player::Impl {
    mpv_handle* mpv = nullptr;
    mpv_render_context* rctx = nullptr;
    bool loaded = false;
    bool ended = false;
    std::string pending_audio;   // external audio URL to attach on file-loaded
    bool audio_added = false;

    static void* get_proc(void*, const char* name) {
        return SDL_GL_GetProcAddress(name);
    }

    bool init() {
        bool dbg = getenv("YTNATIVE_DEBUG");
        if (dbg) std::fprintf(stderr, "[mpv] create...\n");
        mpv = mpv_create();
        if (!mpv) return false;
        // We ALWAYS hand mpv a direct googlevideo URL from our own Innertube
        // client, so disable mpv's built-in ytdl_hook — otherwise it shells out
        // to youtube-dl/yt-dlp on its own (unwanted dependency; 403s + latency;
        // fails outright on the handheld where yt-dlp isn't installed).
        mpv_set_option_string(mpv, "ytdl", "no");
        // Reasonable defaults for streaming on ARM handhelds.
        mpv_set_option_string(mpv, "vd-lavc-threads", "4");
        // hwdec: default to *-copy so decoded frames are copied to normal
        // textures rather than imported as GPU surfaces into our render-API GL
        // context. Direct surface import (plain "vaapi"/"auto-safe") can produce
        // corrupt/artifacted frames on some drivers (Intel VAAPI + render API).
        // Override with YTNATIVE_HWDEC (e.g. "no", "auto", "vaapi").
        const char* hw = getenv("YTNATIVE_HWDEC");
        mpv_set_option_string(mpv, "hwdec", hw ? hw : "auto-copy-safe");
        mpv_set_option_string(mpv, "cache", "yes");
        mpv_set_option_string(mpv, "demuxer-max-bytes", "48MiB");
        mpv_set_option_string(mpv, "demuxer-max-back-bytes", "16MiB");
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
        return true;
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
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;
bool Player::available() { return true; }

bool Player::play(const std::string& video_url, const std::string& audio_url,
                  const std::string& user_agent) {
    // Fresh mpv instance per video: reusing one across videos leaks decoder /
    // hwdec surface-pool state, which corrupts the 2nd+ video (artifacts).
    impl_->teardown();
    if (!impl_->init()) return false;
    mpv_set_option_string(impl_->mpv, "user-agent", user_agent.c_str());
    // Route both streams through our ytn:// protocol (bounded-range libcurl
    // fetch) so iOS-issued URLs — which 403 on ffmpeg's open-ended ranges —
    // play. Live HLS manifests (.m3u8) must NOT be wrapped: mpv handles those.
    bool is_hls = video_url.find(".m3u8") != std::string::npos ||
                  video_url.find("/manifest/hls") != std::string::npos;
    std::string vurl = is_hls ? video_url : ytn::wrap_url(video_url, user_agent);
    impl_->pending_audio = (audio_url.empty() || is_hls)
                           ? audio_url : ytn::wrap_url(audio_url, user_agent);
    impl_->audio_added = false;
    const char* cmd[] = {"loadfile", vurl.c_str(), nullptr};
    if (mpv_command(impl_->mpv, cmd) < 0) return false;
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
    bool dbg = getenv("YTNATIVE_DEBUG");
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
void Player::seek_relative(double seconds) {
    if (!impl_->mpv) return;
    std::string s = std::to_string(seconds);
    const char* cmd[] = {"seek", s.c_str(), "relative", nullptr};
    mpv_command(impl_->mpv, cmd);
}
bool Player::active() const { return impl_->loaded; }
bool Player::paused() const { return impl_->prop_flag("pause") != 0; }
double Player::position() const { return impl_->prop_d("time-pos"); }
double Player::duration() const { return impl_->prop_d("duration"); }

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
bool Player::play(const std::string&, const std::string&, const std::string&) { return false; }
void Player::stop() {}
bool Player::pump() { return false; }
void Player::render(int, int) {}
void Player::toggle_pause() {}
void Player::seek_relative(double) {}
bool Player::active() const { return false; }
bool Player::paused() const { return false; }
double Player::position() const { return 0; }
double Player::duration() const { return 0; }
} // namespace ui
#endif
