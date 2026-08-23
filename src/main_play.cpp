// Milestone 2: play a resolved YouTube video via libmpv rendering into an
// SDL2/OpenGL context. Proves the full pipeline with no yt-dlp and no reliance
// on the device ffmpeg *binary* (libmpv links its own ffmpeg libraries).
//
//   ./yt_play <videoId> [max_height]
//
// SDL2 owns the window + GL context + input; libmpv owns streaming, DASH
// demux, decode, and A/V sync, painting frames into our default framebuffer.
#include "innertube.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* config_path() {
    const char* env = std::getenv("YTNATIVE_CONFIG");
    return env ? env : "config/clients.json";
}

static void* get_proc(void*, const char* name) {
    return SDL_GL_GetProcAddress(name);
}

// mpv wakes us via SDL events instead of us polling.
static Uint32 s_wakeup_event = (Uint32)-1;
static void on_mpv_events(void*)      { SDL_Event e{}; e.type = s_wakeup_event; SDL_PushEvent(&e); }
static void on_mpv_render_update(void*){ SDL_Event e{}; e.type = s_wakeup_event; SDL_PushEvent(&e); }

static void die(const char* msg) { std::fprintf(stderr, "%s\n", msg); std::exit(1); }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <videoId> [max_height]\n", argv[0]); return 2; }
    std::string video_id = argv[1];
    int max_height = argc > 2 ? std::atoi(argv[2]) : 0;

    // 1. Resolve stream URLs.
    yt::VideoInfo info;
    try {
        yt::Innertube it(config_path());
        info = it.resolve(video_id);
    } catch (const std::exception& e) { std::fprintf(stderr, "resolve: %s\n", e.what()); return 1; }
    if (!info.ok()) { std::fprintf(stderr, "not playable: %s / %s\n",
                                   info.status.c_str(), info.status_reason.c_str()); return 1; }
    yt::VideoPrefs vprefs;
    vprefs.max_height = max_height;
    const yt::Format* v = info.best_video(vprefs);
    const yt::Format* a = info.best_audio();
    if (!v || v->url.empty()) { std::fprintf(stderr, "no usable H264 video format\n"); return 1; }
    std::printf("Playing '%s' — video itag %d (%s), audio itag %d\n",
                info.title.c_str(), v->itag, v->quality_label.c_str(), a ? a->itag : -1);

    // 2. SDL window + GL context.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) die(SDL_GetError());
    // GLES2 everywhere: the UI renderer (gfx.cpp) and mpv's GL renderer share
    // one ES 2.0 context, the lowest common denominator across handheld Malis.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("ytnative", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) die(SDL_GetError());
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) die(SDL_GetError());
    SDL_GL_SetSwapInterval(1);

    // 3. libmpv core.
    mpv_handle* mpv = mpv_create();
    if (!mpv) die("mpv_create failed");
    mpv_set_option_string(mpv, "user-agent", info.user_agent.c_str());
    mpv_set_option_string(mpv, "ytdl", "no");   // direct URLs only; no youtube-dl
    mpv_set_option_string(mpv, "vd-lavc-threads", "4");
    const char* hw = getenv("YTNATIVE_HWDEC");
    mpv_set_option_string(mpv, "hwdec", hw ? hw : "auto-safe"); // e.g. rkmpp on RK3588
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "demuxer-max-bytes", "48MiB");
    mpv_set_option_string(mpv, "audio-files",
                          a && !a->url.empty() ? a->url.c_str() : "");
    if (mpv_initialize(mpv) < 0) die("mpv_initialize failed");

    // 4. Render context bound to our GL.
    mpv_opengl_init_params gl_init{ get_proc, nullptr };
    int adv = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &adv},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context* rctx = nullptr;
    if (mpv_render_context_create(&rctx, mpv, params) < 0) die("render_context_create failed");

    s_wakeup_event = SDL_RegisterEvents(1);
    mpv_set_wakeup_callback(mpv, on_mpv_events, nullptr);
    mpv_render_context_set_update_callback(rctx, on_mpv_render_update, nullptr);

    // 5. Load the (video-only) URL; audio comes from audio-files above.
    const char* cmd[] = {"loadfile", v->url.c_str(), nullptr};
    mpv_command(mpv, cmd);

    // Optional headless decode benchmark: YTNATIVE_BENCH=<seconds> prints
    // resolution / hwdec / fps then exits. Lets us validate decode on a board
    // with no attached display.
    const char* bench_env = std::getenv("YTNATIVE_BENCH");
    Uint32 bench_ms = bench_env ? (Uint32)(atof(bench_env) * 1000) : 0;
    Uint32 start_ticks = SDL_GetTicks();

    // 6. Event + render loop.
    bool running = true;
    while (running) {
        if (bench_ms && SDL_GetTicks() - start_ticks > bench_ms) {
            char *hwdec = nullptr, *vcodec = nullptr;
            double fps = 0; int64_t w = 0, h = 0, drops = 0;
            mpv_get_property(mpv, "hwdec-current", MPV_FORMAT_STRING, &hwdec);
            mpv_get_property(mpv, "video-codec", MPV_FORMAT_STRING, &vcodec);
            mpv_get_property(mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps);
            mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &w);
            mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &h);
            mpv_get_property(mpv, "frame-drop-count", MPV_FORMAT_INT64, &drops);
            std::printf("BENCH: %lldx%lld  codec=%s  hwdec=%s  fps=%.1f  dropped=%lld\n",
                        (long long)w, (long long)h, vcodec ? vcodec : "?",
                        hwdec && hwdec[0] ? hwdec : "no(software)", fps, (long long)drops);
            mpv_free(hwdec); mpv_free(vcodec);
            break;
        }
        SDL_Event ev;
        if (SDL_WaitEvent(&ev) != 1) break;
        switch (ev.type) {
        case SDL_QUIT: running = false; break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) running = false;
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) running = false;
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                const char* pc[] = {"cycle", "pause", nullptr};
                mpv_command(mpv, pc);
            }
            break;
        default:
            if (ev.type == s_wakeup_event) {
                // Drain mpv events.
                while (true) {
                    mpv_event* e = mpv_wait_event(mpv, 0);
                    if (e->event_id == MPV_EVENT_NONE) break;
                    if (e->event_id == MPV_EVENT_END_FILE) running = false;
                    if (e->event_id == MPV_EVENT_LOG_MESSAGE) {
                        auto* m = (mpv_event_log_message*)e->data;
                        std::fprintf(stderr, "[mpv] %s", m->text);
                    }
                }
            }
            break;
        }
        // Render if mpv has an update.
        uint64_t flags = mpv_render_context_update(rctx);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
            int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
            mpv_opengl_fbo fbo{0, w, h, 0};
            int flip = 1;
            mpv_render_param rp[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                {MPV_RENDER_PARAM_FLIP_Y, &flip},
                {MPV_RENDER_PARAM_INVALID, nullptr}};
            mpv_render_context_render(rctx, rp);
            // (UI overlay would be drawn here, on top of the video.)
            SDL_GL_SwapWindow(win);
        }
    }

    mpv_render_context_free(rctx);
    mpv_destroy(mpv);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
