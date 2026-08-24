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
#include <unordered_set>

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
    void evict_lru();                               // GL-thread: cap resident textures
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::deque<std::string> queue_;
    std::vector<Pending> done_;
    std::unordered_map<std::string, std::unique_ptr<gfx::Texture>> tex_;
    std::unordered_map<std::string, bool> requested_;
    std::unordered_map<std::string, uint64_t> used_;   // url -> last-used tick (LRU)
    uint64_t tick_ = 0;
    static constexpr size_t kMaxTextures = 128;         // ~30MB of mqdefault textures
};

// Async per-channel metadata (video count) for channel tiles. Worker thread calls
// Innertube::channel_info() (which uses its own HttpClient) and caches the result.
class ChannelMetaCache {
public:
    explicit ChannelMetaCache(yt::Innertube* it);
    ~ChannelMetaCache();
    void request(const std::string& channel_id);          // idempotent
    std::string video_count(const std::string& channel_id); // "" until ready
    std::string avatar(const std::string& channel_id);      // avatar URL, "" until ready
private:
    void worker();
    yt::Innertube* it_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, std::string> vcount_;   // id -> "528 videos"
    std::unordered_map<std::string, std::string> avatar_;   // id -> avatar URL
    std::unordered_map<std::string, bool> requested_;
};

// Background restricted-delivery checker for the "hide restricted videos" filter.
// One lightweight /player call per UNIQUE channel (using one of its videos), paced
// serially; verdicts cached in memory and persisted (restricted_cache.json) so a
// channel is only ever checked once. Runs only while the filter is enabled.
class RestrictedCheck {
public:
    explicit RestrictedCheck(yt::Innertube* it);
    ~RestrictedCheck();
    void request(const std::string& channel_id, const std::string& video_id); // idempotent
    int  verdict(const std::string& channel_id);  // 1 restricted, 0 clean, -1 unknown
    void put(const std::string& channel_id, bool restricted);  // play-time verdict
    bool drain_dirty();                           // true once when new verdicts landed
private:
    void worker();
    yt::Innertube* it_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dirty_{false};
    std::mutex m_;
    std::deque<std::pair<std::string, std::string>> queue_;   // (channel_id, video_id)
    std::unordered_map<std::string, int> verdict_;            // seeded from disk cache
    std::unordered_set<std::string> requested_;
};

class App {
public:
    App(const std::string& config_path, gfx::Window* win);
    ~App();
    void set_results(std::vector<yt::SearchResult> r);
    void test_end_playback(int idx);   // TEST-ONLY: simulate EOF of results_[idx]
    void search(const std::string& query);
    void load_home();            // "Home": RSS feed from favorite channels (empty by default)
    void load_favorites();       // favorite channels as tiles (local channels.json)
    void load_watch_later();     // watch-later videos as tiles (local watch_later.json)
    void load_history();         // previously-watched videos as tiles (local history.json)
    void open_main_menu();       // top-level menu (Start button): views + navigation

    // Input actions (mapped from SDL events by the caller).
    enum class Action { None, Up, Down, Left, Right, Select, Back, Search, Menu };
    void input(Action a);

    // Text entry (from a physical keyboard, in Search mode).
    void input_text(const std::string& utf8);
    void backspace();

    void render(gfx::Renderer& rn);     // one frame
    void pump_async();                  // per-loop async work (thumbs + mpv events)
    void on_resize();                   // re-bake fonts for the new window height
    void toggle_view();                 // cycle browse view (Grid -> Carousel -> 3D -> Coverflow)
    void cycle_tab(int dir);            // L/R shoulders: switch channel/Home tab
    enum class ViewMode { Grid, Carousel, Carousel3D, Coverflow };
    bool in_subview() const { return in_channel_view_; }   // channel/playlist view active?
    bool menu_open() const { return menu_open_; }
    bool wants_quit() const { return quit_requested_; }     // set by the menu's Exit item
    bool player_paused() const { return player_.paused(); } // for tests/telemetry

    const yt::SearchResult* selected() const;
    int selected_index() const { return sel_; }

    enum class Mode { Grid, Loading, Playing, Search };
    Mode mode() const { return mode_; }

private:
    void ensure_visible();
    void load_fonts();
    int  compute_columns() const;   // responsive column count from window width
    // Grid layout metrics — computed once so render and scrolling never disagree.
    struct GridMetrics { float s, hbar, pad, gutter, cardw, thumbh, meta_h, cardh, rowstep, top, fh, tabs_h; };
    GridMetrics grid_metrics() const;
    void render_grid(gfx::Renderer& rn);
    void render_carousel(gfx::Renderer& rn);
    void render_carousel3d(gfx::Renderer& rn);   // spread 3D carousel
    void render_coverflow(gfx::Renderer& rn);    // traditional stacked coverflow
    // Shared per-tile drawing (grid/carousel/coverflow) — parity by construction.
    void draw_thumb(gfx::Renderer& rn, const yt::SearchResult& v,
                    const gfx::Rect& r, float s, float alpha);
    void draw_meta(gfx::Renderer& rn, const yt::SearchResult& v, int idx,
                   float x, float y, float maxw, float s, float alpha);
    // Precomputed metadata lines per result (composed + humanize_age done ONCE, not
    // every frame). Rebuilt when results_ changes; draw_meta ellipsizes on demand.
    struct TileLines { std::string l1, l2, l3; };
    std::vector<TileLines> tile_lines_;
    std::vector<int> vis_;   // reused scratch for carousel/coverflow visible indices
    void build_tile_lines();
    static TileLines compose_lines(const yt::SearchResult& v, ChannelMetaCache& cmeta);
    void render_browse_chrome(gfx::Renderer& rn, float hy);  // header + tab strip (shared)
    bool browse_empty_overlay(gfx::Renderer& rn);            // centre empty/loading text
    void draw_status_banner(gfx::Renderer& rn, float top_y, float s); // transient msg, clamped
    void render_player(gfx::Renderer& rn);
    void render_loading(gfx::Renderer& rn);
    void render_search(gfx::Renderer& rn);
    void draw_spinner(gfx::Renderer& rn, float cx, float cy, float r, gfx::Color c);
    void request_playback();            // kick off async resolve of selected video
    void save_resume_position();        // persist the current playback position (or clear)
    void render_resume_prompt(gfx::Renderer& rn);
    void start_resolve(const std::string& video_id, const std::string& title, double start_pos);
    void replay_current(double at_seconds);  // re-resolve the playing video, resume at pos
    void poll_resolve();                // main thread: finish a completed resolve
    void open_channel(const yt::SearchResult& ch);   // browse a channel's uploads
    void open_playlist(const yt::SearchResult& pl);  // browse a playlist's videos
    void poll_channel_info();           // GL thread: apply a finished channel-info fetch
    void maybe_load_more();             // kick off next-page fetch when near the bottom
    void poll_more();                   // GL thread: append a finished next page
    void refresh_favorites();           // reload the favorite-id cache from channels.json
    void refresh_watch_later();         // reload the watch-later id cache
    void filter_hidden(std::vector<yt::SearchResult>& items);  // drop restricted/Shorts per settings
    void queue_restricted_checks();     // background-check unknown channels in results_
    void refresh_current_view(bool is_retry = false);  // ASYNC re-fetch of the current list
    void poll_refresh();                // GL thread: apply a finished background refresh
    std::string view_sig() const;       // identity of the current view (discard stale refreshes)
    void open_menu();                   // context options menu for the current item
    void open_settings();               // settings submenu (from the Start menu)
    void open_description(const yt::SearchResult& v);  // scrollable description overlay
    void open_channel_description(const std::string& channel_id, const std::string& name);
    void open_post(const yt::SearchResult& p);   // full post text in the overlay
    void poll_description();            // GL thread: fill the overlay when fetched
    void render_description(gfx::Renderer& rn);
    void menu_activate();               // run the highlighted menu item
    void render_menu(gfx::Renderer& rn);

    // Menu items. The Select-button menu carries per-item actions (Context); the
    // Start-button menu carries top-level navigation (Main).
    enum class MenuAction { FavoriteToggle, WatchLaterToggle, OpenChannel,
                            ShowDescription, ShowChannelDescription, ShowPlaylistDescription,
                            PlayPostVideo,
                            GoHome, GoFavorites, GoWatchLater, GoHistory,
                            GoSettings, CycleMaxQuality, ToggleStats,
                            ToggleHideRestricted, ToggleHideShorts, ToggleAskResume,
                            CycleView, CycleVolume, CycleHwdec, CycleSpeed,
                            ToggleSponsorBlock, CycleCaptions, ToggleAutoplay,
                            CycleHomeSource, ClearHistory, Quit };
    enum class MenuKind { Context, Main, Settings };
    struct MenuItem { std::string label; MenuAction action; };
    void adjust_setting(MenuAction a, int dir);  // Left/Right cycle a setting's value
    void adjust_volume(int delta);               // Up/Down in the player: app-local volume
    void open_search();
    void kb_activate();                 // press the currently-highlighted OSK key
    void submit_search();

    // Result of a background resolve (URLs copied out so no Format* dangles).
    struct ResolveResult {
        bool ok = false;
        bool paced = false;        // restricted delivery: ranges beyond a ~20MiB
                                   // sliding window 403 (some videos, e.g. kids
                                   // content) — seeking must be clamped
        long video_bitrate = 0;    // bits/s of the chosen video format
        std::string status, video_url, audio_url, user_agent, title, description;
    };

    // Description overlay (over the grid or the player).
    bool desc_open_ = false, desc_loading_ = false, desc_paused_ = false;
    // Post-with-video overlay: a selectable video thumbnail on top + the post text
    // below. post_focus_ 0 = video (A plays), 1 = text (Up/Down scrolls).
    bool post_has_video_ = false;
    int  post_focus_ = 0;
    std::string post_thumb_url_;
    std::string desc_title_, desc_text_;
    std::vector<std::string> desc_lines_;   // word-wrapped cache
    float desc_wrap_w_ = 0, desc_scroll_ = 0;
    std::thread desc_thread_;
    std::atomic<bool> desc_running_{false}, desc_done_{false};
    std::mutex desc_m_;
    std::string desc_pending_, desc_pending_id_;
    std::string now_playing_desc_;          // free copy from the playback resolve

    // SponsorBlock: async-fetched skip segments for the playing video.
    bool sponsorblock_ = false;             // setting "sponsorblock" (default off)
    std::vector<yt::SponsorSegment> sb_segments_;
    std::vector<bool> sb_skipped_;          // per-segment: already auto-skipped this play
    std::thread sb_thread_;
    std::atomic<bool> sb_running_{false}, sb_done_{false};
    std::mutex sb_m_;
    std::vector<yt::SponsorSegment> sb_pending_;
    int sb_sig_ = 0;                        // bumped per video; discards stale fetches
    void start_sponsorblock(const std::string& video_id);
    void poll_sponsorblock();

    // Captions/subtitles for the playing video (async track list; on-demand VTT fetch).
    std::vector<yt::CaptionTrack> cc_tracks_;
    int cc_sel_ = 0;                        // 0 = Off, 1..N = cc_tracks_[sel-1]
    std::unordered_map<std::string,std::string> cc_paths_;  // lang_code -> temp VTT path
    std::thread cc_thread_;
    std::atomic<bool> cc_running_{false}, cc_done_{false};
    std::mutex cc_m_;
    std::vector<yt::CaptionTrack> cc_pending_;
    int cc_sig_ = 0;
    // Async VTT download for the selected track (so cycling captions never blocks).
    std::thread cc_dl_thread_;
    std::atomic<bool> cc_dl_running_{false}, cc_dl_done_{false};
    std::string cc_dl_vtt_, cc_dl_lang_;   // pending result + which language it is for
    void start_captions(const std::string& video_id);
    void poll_captions();
    void apply_caption_selection();        // off / cached sub-add / kick async download
    void poll_caption_download();          // install a finished VTT if still selected

    // Autoplay: when a video ends, play the next one automatically.
    bool autoplay_ = false;                // setting "autoplay" (default off)
    int  home_source_ = 0;                 // "home_source": 0 Favorites, 1 Favorites+History
    int  now_playing_index_ = -1;          // index in results_ the playing video came from
    std::thread rel_thread_;               // async /next related fetch (end-of-list fallback)
    std::atomic<bool> rel_running_{false}, rel_done_{false};
    std::mutex rel_m_;
    std::vector<yt::SearchResult> rel_pending_;
    bool rel_autoplay_pending_ = false;    // a related-autoplay fetch is in flight
    // Staged autoplay: after a video ends we wait for the last popup to clear, show an
    // "Up next: <title>" popup on the grid, then start the next video.
    enum class AutoState { None, WaitClear, ShowUpNext };
    AutoState auto_state_ = AutoState::None;
    unsigned auto_show_until_ = 0;
    yt::SearchResult auto_next_item_;
    int auto_next_index_ = -1;
    void handle_playback_ended();          // EOF: arm autoplay / related / back to grid
    int  find_next_playable(int from_index) const;  // next video row after index, or -1
    void arm_upnext(const yt::SearchResult& v, int index);  // stage the up-next sequence
    void step_autoplay();                  // WaitClear -> ShowUpNext -> play (in pump_async)
    void cancel_autoplay();                // any user input during the countdown cancels it
    void start_related_autoplay(const std::string& video_id);
    void poll_related_autoplay();
    void play_item(const yt::SearchResult& v, int index);  // launch a video (no resume prompt)

    Theme theme_;
    gfx::Window* win_;
    std::unique_ptr<gfx::Font> font_title_, font_body_, font_small_;
    int fonts_baked_h_ = 0;   // window height the current font atlases were baked for
    yt::Innertube it_;
    ThumbCache thumbs_;
    ChannelMetaCache chan_meta_{&it_};   // async video-count for channel tiles
    std::vector<yt::SearchResult> results_;
    std::string query_;
    std::string view_label_;   // header label for menu views ("Watch Later" etc.); "" = Latest/search
    int sel_ = 0;       // selected card index
    int cols_ = 3;
    float scroll_ = 0;  // pixels
    ViewMode view_mode_ = ViewMode::Grid;   // persisted browse view (settings.json "view")
    float carousel_pos_ = 0;      // smoothly-animated position (carousel + 3D carousel)
    float carousel_vel_ = 0;      // spring velocity (SmoothDamp)
    unsigned anim_last_ms_ = 0;   // last frame time for dt
    void update_carousel_anim();  // critically-damped glide of carousel_pos_ toward sel_

    // Channel/playlist-browse subview (one level deep) + its back state.
    bool in_channel_view_ = false;
    std::string subview_playlist_;      // non-empty => the subview is a PLAYLIST
    // Tab strips (All / Videos / Shorts / Playlists) — channel views AND Home.
    int  chan_tab_ = 0;                 // channel view: 0=All 1=Videos 2=Shorts 3=Playlists
    int  home_tab_ = 0;                 // Home: same indices
    bool tab_focus_ = false;            // d-pad focus is on the tab strip
    void load_channel_tab(int tab);     // ASYNC fetch + show the given channel tab (0..4)
    void load_home_tab(int tab);        // Home: local filter (All/Videos/Shorts) or async playlists
    void apply_home_tab();              // rebuild results_ from home_items_ for home_tab_
    bool channel_tabs_active() const {  // tabs in a channel subview
        return in_channel_view_ && subview_playlist_.empty();
    }
    bool home_tabs_active() const {     // tabs on the Home screen
        return !in_channel_view_ && query_.empty() && view_label_.empty();
    }
    // Home master data (unfiltered) + lazily-fetched playlists.
    std::vector<yt::SearchResult> home_items_;
    std::vector<yt::SearchResult> home_playlists_;
    bool home_playlists_loaded_ = false;
    // Back stack: each subview push (channel or playlist) snapshots the full view
    // state, so channel -> playlist -> Back -> Back unwinds correctly at any depth.
    struct ViewState {
        std::vector<yt::SearchResult> results;
        std::string query, view_label;
        std::string cont_token, cont_endpoint, cont_channel_id;
        bool in_channel_view = false;
        std::string subview_playlist;
        int chan_tab = 0;
        yt::ChannelInfo channel_info;
        int sel = 0;
        float scroll = 0;
    };
    std::vector<ViewState> view_stack_;
    void push_view();                    // snapshot current view onto the stack
    bool pop_view();                     // restore the top snapshot (false if empty)
    std::unordered_set<std::string> fav_ids_;   // cached favorite channel ids

    // Async channel-info fetch (fills the channel-view header without blocking).
    std::thread chinfo_thread_;
    std::atomic<bool> chinfo_running_{false};
    std::atomic<bool> chinfo_done_{false};
    std::mutex chinfo_m_;
    yt::ChannelInfo chinfo_pending_;
    yt::ChannelInfo channel_info_;              // displayed (matches current channel view)

    // Pagination: continuation cursor for the current feed + async "load more".
    std::string cont_token_, cont_endpoint_, cont_channel_id_;
    std::thread more_thread_;
    std::atomic<bool> more_running_{false};
    std::atomic<bool> more_done_{false};
    std::mutex more_m_;
    yt::Innertube::Feed more_pending_;

    // Async view refresh (hide-filter toggled off -> re-fetch without blocking the UI).
    std::thread refresh_thread_;
    std::atomic<bool> refresh_running_{false};
    std::atomic<bool> refresh_done_{false};
    std::mutex refresh_m_;
    yt::Innertube::Feed refresh_pending_;
    std::string refresh_sig_;           // view identity when the refresh was started
    int refresh_kind_ = 0;              // what the worker fetched (see refresh enum in .cpp)
    // Auto-retry with incremental backoff when a network fetch fails (e.g. app
    // opened before wifi reconnected after wake).
    bool retry_pending_ = false;
    int  retry_attempt_ = 0;
    unsigned retry_at_ = 0;             // SDL ticks when the next retry fires

    Mode mode_ = Mode::Grid;
    Player player_;
    unsigned controls_until_ = 0;   // deadline (SDL ticks) to keep the info overlay shown
    double resume_pos_ = 0;         // seek here once the next stream loads (quality re-resolve)
    bool quality_dirty_ = false;    // quality changed in the player menu; re-resolve on close
    // Seek debounce: coalesce rapid Left/Right presses into ONE seek so we don't fire a
    // burst of range requests that googlevideo rate-limits (403) -> stall/drop-to-grid.
    double pending_seek_ = 0;       // accumulated relative seek (seconds), not yet applied
    bool   has_pending_seek_ = false;
    unsigned pending_seek_at_ = 0;  // SDL ticks of the last seek keypress
    // Restricted (paced) delivery state for the playing video.
    bool playing_paced_ = false;    // deep ranges 403; clamp seeks to the safe window
    long playing_vbitrate_ = 0;     // bits/s (for byte-window -> seconds math)
    double played_max_ = 0;         // high-water playback position this session
    bool stats_for_nerds_ = false;  // overlay decode/stream stats during playback
    double playback_speed_ = 1.0;   // per-video playback speed (resets to 1.0 each video)
    int  volume_ = 100;             // app-local volume % (0..150), persisted "volume"
    unsigned volume_overlay_until_ = 0;  // deadline to show the volume indicator
    int  hwdec_mode_ = 0;           // 0 = Hardware (auto-copy-safe), 1 = Software; "hwdec"
    std::string now_playing_title_;
    yt::SearchResult now_playing_item_;   // context for the player options menu
    std::string status_msg_;

    // Options / main menu (overlays grid or player).
    bool menu_open_ = false;
    MenuKind menu_kind_ = MenuKind::Context;
    bool quit_requested_ = false;
    bool menu_paused_ = false;   // menu auto-paused the player; resume on close
    int menu_sel_ = 0;
    std::vector<MenuItem> menu_items_;
    yt::SearchResult menu_target_;        // the item the menu acts on
    std::unordered_set<std::string> wl_ids_;   // cached watch-later video ids
    RestrictedCheck rcheck_{&it_};             // per-channel restricted-delivery verdicts
    bool hide_restricted_ = false;             // setting: filter restricted channels from lists
    bool hide_shorts_ = false;                 // setting: filter Shorts from lists
    bool ask_resume_ = true;                   // setting: prompt to resume partially-watched videos

    // Resume prompt (shown before playback when a saved position exists).
    bool resume_prompt_open_ = false;
    int  resume_prompt_sel_ = 0;               // 0 = Resume, 1 = Start over
    double resume_prompt_pos_ = 0;
    yt::SearchResult resume_prompt_item_;
    unsigned status_until_ = 0;   // SDL_GetTicks() deadline for the status banner

    // Default playback quality cap. All resolutions stay AVAILABLE, but we don't
    // default to 4K (too heavy for most targets). The future settings menu edits
    // this; YTC_MAXHEIGHT overrides it for testing (0 = uncapped).
    yt::VideoPrefs play_prefs_;

    // On-screen keyboard (Search mode).
    std::string query_input_;
    int kb_row_ = 0, kb_col_ = 0;
    int  kb_caret_ = 0;        // insertion index into query_input_ (ASCII, byte==char)
    bool kb_shift_ = false;    // one-shot uppercase for the next letter

    // Async resolve (Select -> background thread -> play on the GL thread).
    std::thread resolve_thread_;
    std::atomic<bool> resolve_running_{false};
    std::atomic<bool> resolve_done_{false};
    std::mutex resolve_m_;
    ResolveResult resolve_result_;
    std::string loading_title_;
};

} // namespace ui
