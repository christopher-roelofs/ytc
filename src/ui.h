// UI layer: a 10-foot results grid driven by real search + async thumbnails.
// Renders through gfx (GLES2). Input is gamepad/keyboard via SDL. Designed to
// run interactively (KMSDRM on device) or headless (offscreen) for screenshots.
#pragma once
#include "gfx.h"
#include "innertube.h"
#include "http.h"
#include "player.h"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <unordered_map>

namespace ui {

struct Theme {
    gfx::Color bg        = gfx::Color::rgb(0x0f1116);
    gfx::Color panel     = gfx::Color::rgb(0x181b22);
    gfx::Color card      = gfx::Color::rgb(0x1e222b);
    gfx::Color card_sel  = gfx::Color::rgb(0x2b3140);
    gfx::Color accent    = gfx::Color::rgb(0xff3b30); // YouTube-ish red
    gfx::Color text       = gfx::Color::rgb(0xf2f4f8);
    gfx::Color text_dim  = gfx::Color::rgb(0x9aa3b2);
    gfx::Color thumb_bg  = gfx::Color::rgb(0x0a0c10);
};

// Downloads thumbnail bytes on a worker thread; textures are created on the GL
// thread in pump(). GL objects must only touch the render thread.
class ThumbCache {
public:
    ThumbCache();
    ~ThumbCache();
    void request(const std::string& url);          // idempotent
    gfx::Texture* get(const std::string& url);      // nullptr until ready
    void pump(int max_uploads = 2);                 // GL-thread: upload decoded
private:
    struct Pending { std::string url; std::vector<uint8_t> bytes; bool ok; };
    void worker();
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::deque<std::string> queue_;
    std::vector<Pending> done_;
    std::unordered_map<std::string, std::unique_ptr<gfx::Texture>> tex_;
    std::unordered_map<std::string, bool> requested_;
};

class App {
public:
    App(const std::string& config_path, gfx::Window* win);
    ~App();
    void set_results(std::vector<yt::SearchResult> r);
    void search(const std::string& query);

    // Input actions (mapped from SDL events by the caller).
    enum class Action { None, Up, Down, Left, Right, Select, Back, Search };
    void input(Action a);

    // Text entry (from a physical keyboard, in Search mode).
    void input_text(const std::string& utf8);
    void backspace();

    void render(gfx::Renderer& rn);     // one frame
    void pump_async();                  // per-loop async work (thumbs + mpv events)
    void on_resize();                   // re-bake fonts for the new window height
    void toggle_view();                 // switch between grid and carousel browse

    const yt::SearchResult* selected() const;
    int selected_index() const { return sel_; }

    enum class Mode { Grid, Loading, Playing, Search };
    Mode mode() const { return mode_; }

private:
    void ensure_visible();
    void load_fonts();
    int  compute_columns() const;   // responsive column count from window width
    void render_grid(gfx::Renderer& rn);
    void render_carousel(gfx::Renderer& rn);
    void render_player(gfx::Renderer& rn);
    void render_loading(gfx::Renderer& rn);
    void render_search(gfx::Renderer& rn);
    void draw_spinner(gfx::Renderer& rn, float cx, float cy, float r, gfx::Color c);
    void request_playback();            // kick off async resolve of selected video
    void poll_resolve();                // main thread: finish a completed resolve
    void open_search();
    void kb_activate();                 // press the currently-highlighted OSK key
    void submit_search();

    // Result of a background resolve (URLs copied out so no Format* dangles).
    struct ResolveResult {
        bool ok = false;
        std::string status, video_url, audio_url, user_agent, title;
    };

    Theme theme_;
    gfx::Window* win_;
    std::unique_ptr<gfx::Font> font_title_, font_body_, font_small_;
    int fonts_baked_h_ = 0;   // window height the current font atlases were baked for
    yt::Innertube it_;
    ThumbCache thumbs_;
    std::vector<yt::SearchResult> results_;
    std::string query_;
    int sel_ = 0;       // selected card index
    int cols_ = 3;
    float scroll_ = 0;  // pixels
    bool carousel_ = false;       // browse view: false = grid, true = carousel
    float carousel_pos_ = 0;      // eased position (animates toward sel_)

    Mode mode_ = Mode::Grid;
    Player player_;
    std::string now_playing_title_;
    std::string status_msg_;
    unsigned status_until_ = 0;   // SDL_GetTicks() deadline for the status banner

    // Default playback quality cap. All resolutions stay AVAILABLE, but we don't
    // default to 4K (too heavy for most targets). The future settings menu edits
    // this; YTNATIVE_MAXHEIGHT overrides it for testing (0 = uncapped).
    yt::VideoPrefs play_prefs_;

    // On-screen keyboard (Search mode).
    std::string query_input_;
    int kb_row_ = 0, kb_col_ = 0;

    // Async resolve (Select -> background thread -> play on the GL thread).
    std::thread resolve_thread_;
    std::atomic<bool> resolve_running_{false};
    std::atomic<bool> resolve_done_{false};
    std::mutex resolve_m_;
    ResolveResult resolve_result_;
    std::string loading_title_;
};

} // namespace ui
