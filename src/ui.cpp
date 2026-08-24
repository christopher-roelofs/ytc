#include "ui.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace ui {

// Debug logging flag, read once (avoid getenv() on the per-frame render path).
static const bool kDbg = getenv("YTC_DEBUG") != nullptr;

// ---------- On-screen keyboard layout ----------
namespace {
enum KType { KCHAR, KSPACE, KDEL, KCLEAR, KSUBMIT };
struct Key { const char* label; KType type; char ch; int span; };
// Rows navigable by D-pad; `span` widens a key (space/search) for layout + nav.
const std::vector<std::vector<Key>> KB = {
    {{"1",KCHAR,'1',1},{"2",KCHAR,'2',1},{"3",KCHAR,'3',1},{"4",KCHAR,'4',1},{"5",KCHAR,'5',1},
     {"6",KCHAR,'6',1},{"7",KCHAR,'7',1},{"8",KCHAR,'8',1},{"9",KCHAR,'9',1},{"0",KCHAR,'0',1}},
    {{"q",KCHAR,'q',1},{"w",KCHAR,'w',1},{"e",KCHAR,'e',1},{"r",KCHAR,'r',1},{"t",KCHAR,'t',1},
     {"y",KCHAR,'y',1},{"u",KCHAR,'u',1},{"i",KCHAR,'i',1},{"o",KCHAR,'o',1},{"p",KCHAR,'p',1}},
    {{"a",KCHAR,'a',1},{"s",KCHAR,'s',1},{"d",KCHAR,'d',1},{"f",KCHAR,'f',1},{"g",KCHAR,'g',1},
     {"h",KCHAR,'h',1},{"j",KCHAR,'j',1},{"k",KCHAR,'k',1},{"l",KCHAR,'l',1}},
    {{"z",KCHAR,'z',1},{"x",KCHAR,'x',1},{"c",KCHAR,'c',1},{"v",KCHAR,'v',1},{"b",KCHAR,'b',1},
     {"n",KCHAR,'n',1},{"m",KCHAR,'m',1}},
    {{"space",KSPACE,' ',4},{"del",KDEL,0,2},{"clear",KCLEAR,0,2},{"SEARCH",KSUBMIT,0,2}},
};
} // namespace

// Relative "age" for display. Search results already come as "3 months ago"
// (pass through). RSS gives ISO-8601 UTC (has 'T') -> compute relative age.
static std::string humanize_age(const std::string& p) {
    if (p.empty()) return "";
    if (p.find('T') == std::string::npos) return p;   // already human (search)
    int y, mo, d, h, mi, se;
    if (std::sscanf(p.c_str(), "%d-%d-%dT%d:%d:%d", &y,&mo,&d,&h,&mi,&se) != 6) return "";
    std::tm tm{}; tm.tm_year=y-1900; tm.tm_mon=mo-1; tm.tm_mday=d;
    tm.tm_hour=h; tm.tm_min=mi; tm.tm_sec=se;
    time_t t = timegm(&tm), now = time(nullptr);
    long diff = (long)(now - t); if (diff < 0) diff = 0;
    auto fmt = [](long n, const char* u){ std::string s = std::to_string(n)+" "+u;
        if (n != 1) s += "s"; return s + " ago"; };
    if (diff < 60)          return "just now";
    if (diff < 3600)        return fmt(diff/60, "minute");
    if (diff < 86400)       return fmt(diff/3600, "hour");
    if (diff < 86400*7)     return fmt(diff/86400, "day");
    if (diff < 86400*30)    return fmt(diff/(86400*7), "week");
    if (diff < 86400*365)   return fmt(diff/(86400*30), "month");
    return fmt(diff/(86400*365), "year");
}

// ---------- ThumbCache ----------
ThumbCache::ThumbCache() { thread_ = std::thread([this]{ worker(); }); }
ThumbCache::~ThumbCache() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
void ThumbCache::request(const std::string& url) {
    if (url.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (requested_.count(url)) return;
    requested_[url] = true;
    queue_.push_back(url);
}
gfx::Texture* ThumbCache::get(const std::string& url) {
    auto it = tex_.find(url);
    if (it == tex_.end()) return nullptr;
    used_[url] = ++tick_;            // mark most-recently-used (visible this frame)
    return it->second.get();
}
// Evict least-recently-used textures once over the cap. A scrolled-back tile just
// re-downloads. Called on the GL thread (pump) so freeing GL textures is safe.
void ThumbCache::evict_lru() {
    while (tex_.size() > kMaxTextures) {
        auto victim = tex_.begin(); uint64_t oldest = UINT64_MAX;
        for (auto it = tex_.begin(); it != tex_.end(); ++it) {
            uint64_t u = used_.count(it->first) ? used_[it->first] : 0;
            if (u < oldest) { oldest = u; victim = it; }
        }
        std::string url = victim->first;
        tex_.erase(victim);
        used_.erase(url);
        { std::lock_guard<std::mutex> lk(m_); requested_.erase(url); }  // allow re-request
    }
}
void ThumbCache::worker() {
    HttpClient http;
    while (!stop_) {
        std::string url;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { url = queue_.front(); queue_.pop_front(); } }
        if (url.empty()) { SDL_Delay(10); continue; }
        auto r = http.get(url);
        if (getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[thumb] GET %.50s -> %ld %zub\n", url.c_str(), r.status, r.body.size());
        Pending p{url, {}, r.ok()};
        if (r.ok()) p.bytes.assign(r.body.begin(), r.body.end());
        std::lock_guard<std::mutex> lk(m_);
        done_.push_back(std::move(p));
    }
}
void ThumbCache::pump(int max_uploads) {
    std::vector<Pending> ready;
    { std::lock_guard<std::mutex> lk(m_);
      for (int i = 0; i < max_uploads && !done_.empty(); ++i) {
          ready.push_back(std::move(done_.back())); done_.pop_back(); } }
    for (auto& p : ready) {
        std::unique_ptr<gfx::Texture> t;
        if (p.ok && !p.bytes.empty())
            t = gfx::Texture::from_encoded(p.bytes.data(), p.bytes.size());
        if (getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[thumb] decode %.40s -> %s\n", p.url.c_str(), t ? "OK" : "FAIL");
        if (t) { tex_[p.url] = std::move(t); used_[p.url] = ++tick_; }
        else { std::lock_guard<std::mutex> lk(m_); requested_.erase(p.url); }  // failed -> allow retry
    }
    evict_lru();
}

// ---------- ChannelMetaCache ----------
ChannelMetaCache::ChannelMetaCache(yt::Innertube* it) : it_(it) {
    thread_ = std::thread([this]{ worker(); });
}
ChannelMetaCache::~ChannelMetaCache() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
void ChannelMetaCache::request(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (requested_.count(id)) return;
    requested_[id] = true;
    queue_.push_back(id);
}
std::string ChannelMetaCache::video_count(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = vcount_.find(id);
    return it == vcount_.end() ? std::string() : it->second;
}
std::string ChannelMetaCache::avatar(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = avatar_.find(id);
    return it == avatar_.end() ? std::string() : it->second;
}
void ChannelMetaCache::worker() {
    while (!stop_) {
        std::string id;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { id = queue_.front(); queue_.pop_front(); } }
        if (id.empty()) { SDL_Delay(20); continue; }
        yt::ChannelInfo info = it_->channel_info(id);   // own HttpClient inside
        std::lock_guard<std::mutex> lk(m_);
        vcount_[id] = info.video_count_text;            // "" if not found
        avatar_[id] = info.avatar_url;                  // "" if not found
    }
}

// ---------- RestrictedCheck ----------
RestrictedCheck::RestrictedCheck(yt::Innertube* it) : it_(it) {
    for (auto& [id, restricted] : it_->restricted_cache())   // persisted verdicts
        verdict_[id] = restricted ? 1 : 0;
    thread_ = std::thread([this]{ worker(); });
}
RestrictedCheck::~RestrictedCheck() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}
void RestrictedCheck::request(const std::string& channel_id, const std::string& video_id) {
    if (channel_id.empty() || video_id.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (verdict_.count(channel_id) || requested_.count(channel_id)) return;
    requested_.insert(channel_id);
    queue_.emplace_back(channel_id, video_id);
}
int RestrictedCheck::verdict(const std::string& channel_id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = verdict_.find(channel_id);
    return it == verdict_.end() ? -1 : it->second;
}
void RestrictedCheck::put(const std::string& channel_id, bool restricted) {
    if (channel_id.empty()) return;
    { std::lock_guard<std::mutex> lk(m_); verdict_[channel_id] = restricted ? 1 : 0; }
    it_->set_restricted_cached(channel_id, restricted);
    dirty_ = true;
}
bool RestrictedCheck::drain_dirty() { return dirty_.exchange(false); }
void RestrictedCheck::worker() {
    while (!stop_) {
        std::pair<std::string, std::string> job;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { job = queue_.front(); queue_.pop_front(); } }
        if (job.first.empty()) { SDL_Delay(50); continue; }
        int v = it_->check_video_restricted(job.second);   // own HttpClient inside
        {
            std::lock_guard<std::mutex> lk(m_);
            if (v >= 0) verdict_[job.first] = v;
            else requested_.erase(job.first);   // unknown: allow a later retry
        }
        if (v >= 0) {
            it_->set_restricted_cached(job.first, v == 1);
            dirty_ = true;
        }
        SDL_Delay(250);   // pace the /player calls (avoid burst -> bot-wall risk)
    }
}

// ---------- App ----------
void App::load_fonts() {
    // Portable font lookup: env override, then the port/app-local copy in
    // data/, then the Debian system path (dev machines). Handhelds don't
    // ship DejaVu — the port bundles it in data/.
    auto pick_font = []() -> std::string {
        const char* env = std::getenv("YTC_FONT");
        std::string cands[] = { env ? std::string(env) : std::string(),
                                "data/DejaVuSans.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" };
        for (const auto& c : cands) {
            if (c.empty()) continue;
            if (std::ifstream(c).good()) return c;
        }
        return cands[2];
    };
    const std::string ttf_path = pick_font();
    const char* ttf = ttf_path.c_str();
    // Font scale is NOT pure-linear with height: a floor keeps text readable on
    // small handheld screens (held close), and a cap avoids oversized text on 4K.
    float s = win_->height() / 720.f;
    if (s < 0.9f) s = 0.9f;
    if (s > 2.2f) s = 2.2f;
    font_title_ = gfx::Font::load(ttf, 34 * s);
    font_body_  = gfx::Font::load(ttf, 22 * s);
    font_small_ = gfx::Font::load(ttf, 18 * s);
    fonts_baked_h_ = win_->height();
}

App::GridMetrics App::grid_metrics() const {
    GridMetrics m;
    m.s = win_->height() / 720.f;
    m.hbar = 84 * m.s;
    m.pad = 28 * m.s;
    m.gutter = 22 * m.s;
    m.cardw = (win_->width() - m.pad*2 - m.gutter*(cols_-1)) / cols_;
    m.thumbh = m.cardw * 9.f / 16.f;
    m.meta_h = 100 * m.s;                 // title + author + "views · age"
    m.cardh = m.thumbh + m.meta_h;
    m.rowstep = m.cardh + m.gutter;
    // Channel views carry a tab strip between the header and the grid; its height
    // scales with the window (recomputed every frame -> resize-safe).
    m.tabs_h = (channel_tabs_active() || home_tabs_active()) ? 52 * m.s : 0;
    m.top = m.hbar + m.tabs_h + m.pad;
    m.fh = 44 * m.s;
    return m;
}

int App::compute_columns() const {
    // Aim for ~380px-wide cards; fewer columns on small/narrow screens means
    // bigger cards, thumbnails and text. Clamped to a sensible range.
    int c = (int)std::lround(win_->width() / 380.0);
    if (c < 2) c = 2;
    if (c > 6) c = 6;
    return c;
}

void App::on_resize() {
    cols_ = compute_columns();
    // Re-bake fonts only when the height changed enough to matter (atlas bakes
    // are not free, and resize events fire continuously during a drag).
    int h = win_->height();
    if (h > 0 && std::abs(h - fonts_baked_h_) >= 24) load_fonts();
}

App::App(const std::string& config_path, gfx::Window* win)
    : win_(win), it_(config_path) {
    load_fonts();
    cols_ = compute_columns();
    view_mode_ = (ViewMode)it_.setting_int("view", 0);   // 0 grid / 1 carousel / 2 coverflow
    if (getenv("YTC_CAROUSEL")) view_mode_ = ViewMode::Carousel;
    if (const char* vm = getenv("YTC_VIEW")) view_mode_ = (ViewMode)atoi(vm);
    refresh_favorites();
    refresh_watch_later();
    hide_restricted_ = it_.setting_int("hide_restricted", 0) != 0;
    hide_shorts_ = it_.setting_int("hide_shorts", 0) != 0;
    ask_resume_ = it_.setting_int("ask_resume", 1) != 0;   // default ON
    volume_ = it_.setting_int("volume", 100);              // app-local playback volume %
    if (volume_ < 0) volume_ = 0; if (volume_ > 150) volume_ = 150;
    hwdec_mode_ = it_.setting_int("hwdec", 0) ? 1 : 0;     // 0 hardware / 1 software
    player_.set_hwdec(hwdec_mode_ ? "no" : "auto-copy-safe");
    sponsorblock_ = it_.setting_int("sponsorblock", 0) != 0;   // default OFF
    autoplay_ = it_.setting_int("autoplay", 0) != 0;          // default OFF
    home_source_ = it_.setting_int("home_source", 0) ? 1 : 0; // 0 favorites / 1 +history

    // Default quality cap: 1080p (not 4K). Persisted in settings.json (Settings menu);
    // YTC_MAXHEIGHT overrides for testing (0 = uncapped).
    play_prefs_.max_height = it_.setting_int("max_height", 1080);
    if (const char* mh = getenv("YTC_MAXHEIGHT")) play_prefs_.max_height = atoi(mh);
}

App::~App() {
    if (resolve_thread_.joinable()) resolve_thread_.join();
    if (chinfo_thread_.joinable()) chinfo_thread_.join();
    if (more_thread_.joinable()) more_thread_.join();
    if (refresh_thread_.joinable()) refresh_thread_.join();
    if (desc_thread_.joinable()) desc_thread_.join();
    if (sb_thread_.joinable()) sb_thread_.join();
    if (cc_thread_.joinable()) cc_thread_.join();
    if (cc_dl_thread_.joinable()) cc_dl_thread_.join();
    if (rel_thread_.joinable()) rel_thread_.join();
}

void App::set_results(std::vector<yt::SearchResult> r) {
    filter_hidden(r);
    results_ = std::move(r);
    sel_ = 0; scroll_ = 0;
    build_tile_lines();          // precompute metadata lines once (not per frame)
    // Thumbnails are requested lazily per-viewport by the renderers (bounded memory);
    // channel metadata is small, so fetch it up front.
    for (const auto& v : results_)
        if (v.is_channel()) chan_meta_.request(v.channel_id);   // async video count
    queue_restricted_checks();   // judge unknown channels in the background
}
void App::search(const std::string& query) {
    query_ = query;
    view_label_.clear();
    in_channel_view_ = false;
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    auto f = it_.search_feed(query);      // never throws (guarded in Innertube)
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    set_results(std::move(f.items));
    if (results_.empty()) {
        status_msg_ = "No results — check your connection, then press Y to search";
        status_until_ = SDL_GetTicks() + 8000;
    }
}
void App::load_home() {
    query_.clear();                       // empty query + empty view_label_ => "Home"
    view_label_.clear();
    in_channel_view_ = false;
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    cont_token_.clear();                  // merged home feed isn't paginated
    home_tab_ = 0;
    home_playlists_.clear(); home_playlists_loaded_ = false;
    home_items_.clear();
    results_.clear(); sel_ = 0; scroll_ = 0;
    // Fetch the merged home feed ASYNCHRONOUSLY (N favorites x 3 network calls) so
    // the window draws immediately; poll_refresh fills home_items_ + tiles when ready.
    refresh_current_view();               // RK_HOME (view_label_/query_ empty)
}

// Canonical thumbnail for a video id (our stored lists only keep id + title).
static std::string thumb_for(const std::string& video_id) {
    return "https://i.ytimg.com/vi/" + video_id + "/mqdefault.jpg";
}

void App::load_favorites() {
    query_.clear(); view_label_ = "Favorite Channels";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& [id, name] : it_.favorites()) {
        yt::SearchResult r;
        r.kind = yt::SearchResult::Kind::Channel;
        r.channel_id = id;
        r.title = name;                    // subs/video-count fill in async (chan_meta_)
        items.push_back(std::move(r));
    }
    set_results(std::move(items));
}

void App::load_watch_later() {
    query_.clear(); view_label_ = "Watch Later";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    // Entries carry their own kind + metadata (videos AND playlists).
    set_results(it_.watch_later());
}

void App::load_history() {
    query_.clear(); view_label_ = "History";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& [id, title] : it_.history()) {
        yt::SearchResult r;
        r.video_id = id; r.title = title;
        r.thumbnail_url = thumb_for(id);
        items.push_back(std::move(r));
    }
    set_results(std::move(items));
}
void App::refresh_favorites() {
    fav_ids_.clear();
    for (auto& id : it_.favorite_channel_ids()) fav_ids_.insert(id);
}
void App::refresh_watch_later() {
    wl_ids_.clear();
    for (auto& id : it_.watch_later_ids()) wl_ids_.insert(id);
}
// Drop items per the hide settings: restricted channels (checked or learned) and/or
// Shorts. FAVORITE channels are never hidden by the restricted rule — favoriting is
// explicit user intent (their videos still play, with limited seeking) — but the
// Shorts rule applies everywhere.
void App::filter_hidden(std::vector<yt::SearchResult>& items) {
    if (!hide_restricted_ && !hide_shorts_) return;
    items.erase(std::remove_if(items.begin(), items.end(),
        [&](const yt::SearchResult& r) {
            if (hide_shorts_ && r.is_short) return true;
            return hide_restricted_ && !r.channel_id.empty() &&
                   fav_ids_.count(r.channel_id) == 0 &&
                   rcheck_.verdict(r.channel_id) == 1;
        }), items.end());
}
// Identity of the current view, used to discard a background refresh whose view
// the user has already navigated away from. Includes the active tab where content
// differs per tab (channel tabs; Home's async Playlists tab).
std::string App::view_sig() const {
    if (in_channel_view_) {
        std::string id = !channel_info_.channel_id.empty() ? channel_info_.channel_id
                                                           : cont_channel_id_;
        if (subview_playlist_.empty()) return "ch:" + id + ":t" + std::to_string(chan_tab_);
        return "pl:" + id;
    }
    if (!view_label_.empty()) return "label:" + view_label_;
    if (query_.empty()) return home_tab_ == 3 ? "home:pl" : "home";
    return "q:" + query_;
}

// What a background refresh fetched (stored in refresh_kind_ for poll_refresh).
enum { RK_CHANNEL, RK_PLAYLIST, RK_HOME, RK_HOME_PLAYLISTS, RK_SEARCH };

// Re-fetch whatever list is currently showing (hide-filter toggles, tab switches).
// Network views fetch on a BACKGROUND thread — the UI updates instantly and
// poll_refresh applies the new list when it arrives.
void App::refresh_current_view(bool is_retry) {
    if (!is_retry) { retry_pending_ = false; retry_attempt_ = 0; }  // fresh user action
    // Local (file-backed) views are instant — no thread needed. (Guarded on NOT
    // being in a subview: a lingering label must never hijack a channel refresh.)
    if (!in_channel_view_) {
        if (view_label_ == "Favorite Channels") { load_favorites(); return; }
        if (view_label_ == "Watch Later")       { load_watch_later(); return; }
        if (view_label_ == "History")           { load_history(); return; }
    }
    if (refresh_running_) return;              // one refresh at a time
    int kind = in_channel_view_
                  ? (!subview_playlist_.empty() ? RK_PLAYLIST : RK_CHANNEL)
                  : query_.empty() ? (home_tab_ == 3 ? RK_HOME_PLAYLISTS : RK_HOME)
                                   : RK_SEARCH;
    std::string id = kind == RK_PLAYLIST ? subview_playlist_
                   : !channel_info_.channel_id.empty() ? channel_info_.channel_id
                                                       : cont_channel_id_;
    if ((kind == RK_CHANNEL || kind == RK_PLAYLIST) && id.empty()) return;
    std::string query = query_;
    int tab = chan_tab_;
    refresh_sig_ = view_sig();
    refresh_kind_ = kind;
    refresh_running_ = true; refresh_done_ = false;
    if (refresh_thread_.joinable()) refresh_thread_.join();
    refresh_thread_ = std::thread([this, kind, id, query, tab]() {
        yt::Innertube::Feed f;
        try {   // all paths use thread-local HttpClients inside Innertube
            if (kind == RK_CHANNEL) {
                if (tab == 0)      f = it_.channel_all_feed(id);
                else if (tab == 2) f = it_.channel_shorts_feed(id);
                else if (tab == 3) f = it_.channel_playlists_feed(id);
                else if (tab == 4) f = it_.channel_posts_feed(id);
                else               f = it_.channel_feed(id);
            }
            else if (kind == RK_PLAYLIST)        f = it_.playlist_feed(id);
            else if (kind == RK_HOME) { f.items = it_.home_feed({}, 120, home_source_ == 1);
                // ok unless we have favourites but couldn't reach the network
                f.ok = it_.favorite_channel_ids().empty() || it_.has_visitor_data(); }
            else if (kind == RK_HOME_PLAYLISTS){ f.items = it_.home_playlists();
                f.ok = it_.favorite_channel_ids().empty() || it_.has_visitor_data(); }
            else                                 f = it_.search_feed(query);
        } catch (...) {}   // never let an exception escape the thread
        { std::lock_guard<std::mutex> lk(refresh_m_); refresh_pending_ = std::move(f); }
        refresh_running_ = false;
        refresh_done_ = true;
    });
}

void App::poll_refresh() {
    if (!refresh_done_.exchange(false)) return;
    if (refresh_thread_.joinable()) refresh_thread_.join();
    yt::Innertube::Feed f;
    { std::lock_guard<std::mutex> lk(refresh_m_); f = std::move(refresh_pending_); }
    bool fetch_ok = f.ok;
    if (view_sig() != refresh_sig_) {
        // Stale (user switched tab/view while fetching). If the current view is
        // sitting empty waiting for content, kick off the right fetch now.
        if (results_.empty()) refresh_current_view();
        return;
    }
    switch (refresh_kind_) {
        case RK_HOME:                        // refresh of the Home master feed
            home_items_ = std::move(f.items);
            cont_token_.clear();
            apply_home_tab();                // re-apply the active All/Videos/Shorts filter
            break;
        case RK_HOME_PLAYLISTS:
            home_playlists_ = f.items;       // cache (pre-filter copy)
            home_playlists_loaded_ = true;
            cont_token_.clear();
            set_results(std::move(f.items));
            break;
        default:
            cont_token_ = f.continuation; cont_endpoint_ = f.endpoint;
            cont_channel_id_ = f.channel_id;
            set_results(std::move(f.items));
            // Channel tabs: rows omit the uploader; the view title is the channel name.
            if (refresh_kind_ == RK_CHANNEL)
                for (auto& r : results_)
                    if (r.author.empty()) r.author = query_;
            break;
    }
    // Network fetch failed (e.g. opened before wifi reconnected) -> auto-retry with
    // incremental backoff (1,2,4,8,16, capped 30s) until it succeeds.
    if (!fetch_ok) {
        retry_attempt_ = std::min(retry_attempt_ + 1, 6);
        unsigned delay = 1000u << (retry_attempt_ - 1);
        if (delay > 30000) delay = 30000;
        retry_at_ = SDL_GetTicks() + delay;
        retry_pending_ = true;
    } else {
        retry_pending_ = false; retry_attempt_ = 0;
    }
}

// Queue background checks for channels we haven't judged yet (uses one of the
// channel's videos as the probe target; channel-only tiles wait for a video).
void App::queue_restricted_checks() {
    if (!hide_restricted_) return;
    for (const auto& r : results_)
        if (!r.channel_id.empty() && !r.video_id.empty())
            rcheck_.request(r.channel_id, r.video_id);
}

// ---------- Top-level menu (Start button) ----------
void App::open_main_menu() {
    if (menu_open_) return;               // don't stack on an already-open menu
    menu_kind_ = MenuKind::Main;
    menu_items_.clear();
    menu_items_.push_back({"Home",              MenuAction::GoHome});
    menu_items_.push_back({"Favorite Channels", MenuAction::GoFavorites});
    menu_items_.push_back({"Watch Later",       MenuAction::GoWatchLater});
    menu_items_.push_back({"History",           MenuAction::GoHistory});
    menu_items_.push_back({"Settings",          MenuAction::GoSettings});
    menu_items_.push_back({"Exit",              MenuAction::Quit});
    menu_sel_ = 0;
    // Opening a menu over the player pauses it; resumed when the menu closes without
    // navigating away. Don't clear menu_paused_ if we're already paused (e.g. returning
    // from the Settings submenu), or we'd forget to resume on close.
    if (mode_ == Mode::Playing && !player_.paused()) { player_.set_pause(true); menu_paused_ = true; }
    menu_open_ = true;
}

// Human label for a max-height cap (0 = uncapped).
static std::string quality_label(int h) {
    if (h <= 0) return "Auto (highest)";
    return std::to_string(h) + "p";
}

// ---------- Settings submenu ----------
void App::open_settings() {
    menu_kind_ = MenuKind::Settings;
    menu_items_.clear();
    menu_items_.push_back({"Max Quality:  " + quality_label(play_prefs_.max_height),
                           MenuAction::CycleMaxQuality});
    menu_items_.push_back({"Volume:  " + std::to_string(volume_) + "%",
                           MenuAction::CycleVolume});
    menu_items_.push_back({std::string("Video Decode:  ")
                           + (hwdec_mode_ ? "Software" : "Hardware"),
                           MenuAction::CycleHwdec});
    menu_items_.push_back({std::string("Hide Paced Videos:  ")
                           + (hide_restricted_ ? "On" : "Off"),
                           MenuAction::ToggleHideRestricted});
    menu_items_.push_back({std::string("Hide Shorts:  ")
                           + (hide_shorts_ ? "On" : "Off"),
                           MenuAction::ToggleHideShorts});
    menu_items_.push_back({std::string("Ask to Resume:  ")
                           + (ask_resume_ ? "On" : "Off"),
                           MenuAction::ToggleAskResume});
    menu_items_.push_back({std::string("Home Feed:  ")
                           + (home_source_ == 1 ? "Favorites + History" : "Favorites"),
                           MenuAction::CycleHomeSource});
    menu_items_.push_back({std::string("Autoplay:  ")
                           + (autoplay_ ? "On" : "Off"),
                           MenuAction::ToggleAutoplay});
    menu_items_.push_back({std::string("SponsorBlock:  ")
                           + (sponsorblock_ ? "On" : "Off"),
                           MenuAction::ToggleSponsorBlock});
    const char* vname[] = {"Grid", "Carousel", "3D Carousel", "Coverflow"};
    menu_items_.push_back({std::string("View:  ") + vname[(int)view_mode_],
                           MenuAction::CycleView});
    menu_sel_ = 0;
    // (Do not re-pause here; if opened from the player the main menu already did.)
    menu_open_ = true;
}

// Change a setting's value by dir (-1 = Left, +1 = Right) and persist it.
void App::adjust_setting(MenuAction a, int dir) {
    if (a == MenuAction::ToggleStats) {
        stats_for_nerds_ = !stats_for_nerds_;
        int keep = menu_sel_; open_menu(); menu_sel_ = keep;   // rebuild label in place
        return;
    }
    if (a == MenuAction::ToggleHideRestricted) {
        hide_restricted_ = !hide_restricted_;
        it_.set_setting_int("hide_restricted", hide_restricted_ ? 1 : 0);
        if (hide_restricted_) {
            filter_hidden(results_);      // apply known verdicts now...
            if (sel_ >= (int)results_.size())
                sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            queue_restricted_checks();        // ...and start judging the rest
        } else if (mode_ != Mode::Playing) {
            refresh_current_view();           // bring the hidden items back
        }
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleHideShorts) {
        hide_shorts_ = !hide_shorts_;
        it_.set_setting_int("hide_shorts", hide_shorts_ ? 1 : 0);
        if (hide_shorts_) {
            filter_hidden(results_);          // Shorts vanish from the current list
            if (sel_ >= (int)results_.size())
                sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
        } else if (mode_ != Mode::Playing) {
            refresh_current_view();           // Shorts come back
        }
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleAskResume) {
        ask_resume_ = !ask_resume_;
        it_.set_setting_int("ask_resume", ask_resume_ ? 1 : 0);
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleVolume) {
        int v = volume_ + dir * 5;
        if (v < 0) v = 0;
        if (v > 150) v = 150;
        volume_ = v;
        it_.set_setting_int("volume", volume_);
        if (mode_ == Mode::Playing) player_.set_volume(volume_);   // live if playing
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleAutoplay) {
        autoplay_ = !autoplay_;
        it_.set_setting_int("autoplay", autoplay_ ? 1 : 0);
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleHomeSource) {
        home_source_ = home_source_ ? 0 : 1;
        it_.set_setting_int("home_source", home_source_);
        // If we're viewing Home, re-fetch with the new source.
        if (query_.empty() && view_label_.empty() && !in_channel_view_)
            refresh_current_view();
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleSponsorBlock) {
        sponsorblock_ = !sponsorblock_;
        it_.set_setting_int("sponsorblock", sponsorblock_ ? 1 : 0);
        if (sponsorblock_ && mode_ == Mode::Playing)      // fetch now for the current video
            start_sponsorblock(now_playing_item_.video_id);
        else if (!sponsorblock_) { std::lock_guard<std::mutex> lk(sb_m_);
            sb_segments_.clear(); sb_skipped_.clear(); }
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptions) {
        int n = (int)cc_tracks_.size();
        if (n > 0) {
            cc_sel_ = (cc_sel_ + dir + (n + 1)) % (n + 1);   // 0=Off, 1..n tracks; wraps
            apply_caption_selection();
        }
        int keep = menu_sel_; open_menu(); menu_sel_ = keep;   // rebuild label
        return;
    }
    if (a == MenuAction::CycleSpeed) {
        static const double steps[] = {0.25,0.5,0.75,1.0,1.25,1.5,1.75,2.0};
        int n = (int)(sizeof(steps)/sizeof(steps[0])), cur = 3;   // default 1.0x
        for (int i = 0; i < n; ++i) if (std::abs(steps[i]-playback_speed_) < 0.01) { cur = i; break; }
        cur = (cur + dir + n) % n;
        playback_speed_ = steps[cur];
        if (mode_ == Mode::Playing) player_.set_speed(playback_speed_);
        int keep = menu_sel_; open_menu(); menu_sel_ = keep;    // rebuild label in place
        return;
    }
    if (a == MenuAction::CycleHwdec) {
        hwdec_mode_ = hwdec_mode_ ? 0 : 1;                 // toggle Hardware/Software
        it_.set_setting_int("hwdec", hwdec_mode_);
        player_.set_hwdec(hwdec_mode_ ? "no" : "auto-copy-safe");
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        // hwdec is fixed at mpv init, so a live change needs a fresh stream: re-resolve
        // the current video at its position (applies the new decoder immediately).
        if (mode_ == Mode::Playing) { menu_open_ = false; menu_paused_ = false;
                                      replay_current(player_.position()); }
        return;
    }
    if (a == MenuAction::CycleView) {
        int nv = ((int)view_mode_ + (dir < 0 ? 3 : 1)) % 4;
        view_mode_ = (ViewMode)nv;
        it_.set_setting_int("view", nv);
        carousel_pos_ = sel_;
        int keep = menu_sel_; open_settings(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleMaxQuality) {
        // Ordered low->high so Right raises quality; Auto (highest) sits at the top end.
        static const int steps[] = {360, 480, 720, 1080, 1440, 2160, 0 /* Auto */};
        int n = (int)(sizeof(steps)/sizeof(steps[0]));
        int cur = 0;
        for (int i = 0; i < n; ++i) if (steps[i] == play_prefs_.max_height) { cur = i; break; }
        cur = ((cur + dir) % n + n) % n;
        play_prefs_.max_height = steps[cur];
        it_.set_setting_int("max_height", play_prefs_.max_height);
        // Rebuild the current menu so the label updates in place.
        int keep = menu_sel_;
        if (menu_kind_ == MenuKind::Settings) open_settings();
        else open_menu();
        menu_sel_ = keep;
        // In the player options menu, mark the change so we re-resolve the current
        // video (at the new quality, resuming the position) when the menu closes.
        if (menu_kind_ == MenuKind::Context && mode_ == Mode::Playing) quality_dirty_ = true;
    }
    // Future bool settings toggle here (dir flips the value).
}

// ---------- Resume prompt ----------
void App::render_resume_prompt(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.72f));
    float pw = std::min(560.f*s, W*0.82f), ph = 250*s;
    float px = (W-pw)/2, py = (H-ph)/2;
    rn.quad({px-20*s, py-20*s, pw+40*s, ph+40*s}, theme_.panel);
    rn.quad({px-20*s, py-20*s, pw+40*s, 4*s}, theme_.accent);
    rn.text(*font_body_, font_body_->ellipsize(resume_prompt_item_.title, pw), px, py, theme_.text);
    int m = (int)resume_prompt_pos_/60, sec = (int)resume_prompt_pos_%60;
    char t[32]; std::snprintf(t, sizeof t, "Resume from %d:%02d", m, sec);
    const char* opts[2] = {t, "Start from beginning"};
    float iy = py + 60*s, ih = 56*s, gap = 10*s;
    for (int i = 0; i < 2; ++i) {
        bool sel = (i == resume_prompt_sel_);
        rn.quad({px, iy, pw, ih}, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({px, iy, 4*s, ih}, theme_.accent);
        rn.text(*font_body_, opts[i], px + 18*s, iy + (ih-font_body_->line_height())/2 + 3*s,
                sel ? theme_.text : theme_.text_dim);
        iy += ih + gap;
    }
    rn.text(*font_small_, "Up/Down: choose    A: confirm    B: cancel",
            px, iy + 8*s, theme_.text_dim);
    rn.end();
}

// ---------- Description overlay ----------
void App::open_description(const yt::SearchResult& v) {
    desc_title_ = v.title;
    desc_text_.clear();
    desc_lines_.clear();
    desc_scroll_ = 0; desc_wrap_w_ = 0;
    post_has_video_ = false;   // plain description, not the post-with-video layout
    desc_open_ = true;
    // Playing video: the description arrived with the resolve — no fetch needed.
    if (mode_ == Mode::Playing && !v.is_playlist() &&
        v.video_id == now_playing_item_.video_id) {
        desc_text_ = now_playing_desc_.empty() ? "(no description)" : now_playing_desc_;
        desc_loading_ = false;
        return;
    }
    // Grid tile: fetch async (one lightweight call; playlists via their VL browse).
    desc_loading_ = true;
    if (desc_running_) return;                   // a fetch is already in flight
    bool is_pl = v.is_playlist();
    std::string id = is_pl ? v.playlist_id : v.video_id;
    { std::lock_guard<std::mutex> lk(desc_m_); desc_pending_id_ = id; }
    desc_running_ = true; desc_done_ = false;
    if (desc_thread_.joinable()) desc_thread_.join();
    desc_thread_ = std::thread([this, id, is_pl]() {
        std::string d;
        try { d = is_pl ? it_.playlist_description(id)
                        : it_.video_description(id); } catch (...) {}
        { std::lock_guard<std::mutex> lk(desc_m_); desc_pending_ = std::move(d); }
        desc_running_ = false;
        desc_done_ = true;
    });
}
// SponsorBlock: fetch skip segments for the playing video off-thread. The result is
// applied in poll_sponsorblock() only if the signal still matches (video unchanged).
void App::start_sponsorblock(const std::string& video_id) {
    { std::lock_guard<std::mutex> lk(sb_m_); sb_segments_.clear(); sb_skipped_.clear(); }
    if (!sponsorblock_ || video_id.empty()) return;
    if (sb_running_) return;   // previous fetch still in flight; don't block the UI thread
    int sig = ++sb_sig_;
    if (sb_thread_.joinable()) sb_thread_.join();   // finished -> instant
    sb_running_ = true; sb_done_ = false;
    sb_thread_ = std::thread([this, video_id, sig]() {
        std::vector<yt::SponsorSegment> segs;
        try { segs = it_.sponsor_segments(video_id,
                  "sponsor,selfpromo,interaction,intro,outro,music_offtopic"); } catch (...) {}
        { std::lock_guard<std::mutex> lk(sb_m_);
          if (sig == sb_sig_) sb_pending_ = std::move(segs); }
        sb_running_ = false; sb_done_ = true;
    });
}
void App::poll_sponsorblock() {
    if (!sb_done_.exchange(false)) return;
    std::lock_guard<std::mutex> lk(sb_m_);
    sb_segments_ = std::move(sb_pending_);
    sb_pending_.clear();
    sb_skipped_.assign(sb_segments_.size(), false);
}

// Captions: fetch the track list off-thread when a video starts.
void App::start_captions(const std::string& video_id) {
    { std::lock_guard<std::mutex> lk(cc_m_); cc_tracks_.clear(); }
    cc_sel_ = 0; cc_paths_.clear();
    if (video_id.empty()) return;
    if (cc_running_) return;   // previous track-list fetch still in flight; don't block UI
    int sig = ++cc_sig_;
    if (cc_thread_.joinable()) cc_thread_.join();   // finished -> instant
    cc_running_ = true; cc_done_ = false;
    cc_thread_ = std::thread([this, video_id, sig]() {
        std::vector<yt::CaptionTrack> t;
        try { t = it_.caption_tracks(video_id); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cc_m_); if (sig == cc_sig_) cc_pending_ = std::move(t); }
        cc_running_ = false; cc_done_ = true;
    });
}
void App::poll_captions() {
    if (!cc_done_.exchange(false)) return;
    std::lock_guard<std::mutex> lk(cc_m_);
    cc_tracks_ = std::move(cc_pending_);
    cc_pending_.clear();
}
// Apply the current caption selection WITHOUT blocking: Off hides; an already-cached
// track is added instantly; an un-cached track is downloaded on a worker thread and
// installed later by poll_caption_download().
void App::apply_caption_selection() {
    if (cc_sel_ <= 0 || cc_sel_ > (int)cc_tracks_.size()) { player_.subtitles_off(); return; }
    const yt::CaptionTrack& t = cc_tracks_[cc_sel_ - 1];
    auto it = cc_paths_.find(t.language_code);
    if (it != cc_paths_.end()) { player_.add_subtitle(it->second); return; }  // cached
    // Not cached: fetch off-thread. Hide until it arrives; show a brief status.
    player_.subtitles_off();
    status_msg_ = "Loading captions..."; status_until_ = SDL_GetTicks() + 4000;
    std::string url = t.base_url, lang = t.language_code;
    if (cc_dl_thread_.joinable()) cc_dl_thread_.join();
    cc_dl_running_ = true; cc_dl_done_ = false;
    cc_dl_thread_ = std::thread([this, url, lang]() {
        std::string vtt;
        try { vtt = it_.caption_vtt(url); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cc_m_); cc_dl_vtt_ = std::move(vtt); cc_dl_lang_ = lang; }
        cc_dl_running_ = false; cc_dl_done_ = true;
    });
}
// Install a finished VTT — but only if that language is still the selection.
void App::poll_caption_download() {
    if (!cc_dl_done_.exchange(false)) return;
    std::string vtt, lang;
    { std::lock_guard<std::mutex> lk(cc_m_); vtt = std::move(cc_dl_vtt_); lang = cc_dl_lang_;
      cc_dl_vtt_.clear(); }
    bool still_selected = cc_sel_ > 0 && cc_sel_ <= (int)cc_tracks_.size() &&
                          cc_tracks_[cc_sel_ - 1].language_code == lang;
    if (vtt.empty()) {
        if (still_selected) { status_msg_ = "Captions unavailable";
            status_until_ = SDL_GetTicks() + 2500; cc_sel_ = 0; player_.subtitles_off(); }
        return;
    }
    std::string path = "/tmp/ytc_cc_" + lang + ".vtt";
    { std::ofstream o(path, std::ios::binary); o << vtt; }
    cc_paths_[lang] = path;
    if (still_selected) player_.add_subtitle(path);   // user may have moved on -> just cache
}

// Channel description in the same overlay (used from playlist rows, where each
// video names a different uploader).
void App::open_channel_description(const std::string& channel_id, const std::string& name) {
    desc_title_ = name;
    desc_text_.clear();
    desc_lines_.clear();
    desc_scroll_ = 0; desc_wrap_w_ = 0;
    post_has_video_ = false;   // plain description, not the post-with-video layout
    desc_open_ = true;
    desc_loading_ = true;
    if (desc_running_) return;
    std::string id = channel_id;
    desc_running_ = true; desc_done_ = false;
    if (desc_thread_.joinable()) desc_thread_.join();
    desc_thread_ = std::thread([this, id]() {
        std::string d;
        try { d = it_.channel_info(id).description; } catch (...) {}
        { std::lock_guard<std::mutex> lk(desc_m_); desc_pending_ = std::move(d); }
        desc_running_ = false;
        desc_done_ = true;
    });
}

// Community post: the full text is already on the tile — instant overlay.
void App::open_post(const yt::SearchResult& p) {
    desc_title_ = p.published_text.empty() ? "Post"
                                           : ("Post  -  " + p.published_text
                                              + (p.view_count_text.empty() ? ""
                                                 : "  -  " + p.view_count_text));
    desc_text_ = p.post_text.empty() ? "(no text)" : p.post_text;
    desc_lines_.clear(); desc_wrap_w_ = 0; desc_scroll_ = 0;
    desc_loading_ = false;
    // Attached video: show its thumbnail (selected) on top; A plays it, Down -> text.
    post_has_video_ = !p.video_id.empty();
    post_thumb_url_ = p.thumbnail_url;
    post_focus_ = 0;
    if (post_has_video_ && !post_thumb_url_.empty()) thumbs_.request(post_thumb_url_);
    desc_open_ = true;   // desc_paused_ is managed by the caller (menu path)
}

void App::poll_description() {
    if (!desc_done_.exchange(false)) return;
    if (desc_thread_.joinable()) desc_thread_.join();
    std::string d;
    { std::lock_guard<std::mutex> lk(desc_m_); d = std::move(desc_pending_); }
    if (!desc_open_ || !desc_loading_) return;   // overlay closed meanwhile — discard
    desc_text_ = d.empty() ? "(no description)" : d;
    desc_lines_.clear(); desc_wrap_w_ = 0;       // re-wrap on next render
    desc_loading_ = false;
}

// Greedy word-wrap honoring \n; very long unbreakable words (URLs) hard-break at
// UTF-8 boundaries so nothing overflows the panel.
static std::vector<std::string> wrap_text(const gfx::Font& f, const std::string& text,
                                          float max_w) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos
                                                                     : nl - pos);
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos
                                                                       : sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (f.text_width(cand) <= max_w) {
                line = std::move(cand);
            } else if (!line.empty()) {
                lines.push_back(line);
                line = word;
            }
            if (line.empty() ? false : f.text_width(line) > max_w) {
                // Single word wider than the panel: hard-break at UTF-8 boundaries.
                std::string chunk;
                for (size_t b = 0; b < line.size(); ) {
                    size_t adv = 1;
                    uint8_t c = (uint8_t)line[b];
                    if ((c >> 5) == 0x6) adv = 2;
                    else if ((c >> 4) == 0xE) adv = 3;
                    else if ((c >> 3) == 0x1E) adv = 4;
                    std::string next = chunk + line.substr(b, adv);
                    if (!chunk.empty() && f.text_width(next) > max_w) {
                        lines.push_back(chunk);
                        chunk.clear();
                        next = line.substr(b, adv);
                    }
                    chunk = std::move(next);
                    b += adv;
                }
                line = std::move(chunk);
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        lines.push_back(line);                    // may be "" (blank paragraph line)
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

void App::render_description(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    // Opaque backdrop: this overlay can sit over the player, whose video frame and
    // control bar would otherwise bleed through the panel's inset margins.
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg);
    float mx = 70*s, my = 46*s;
    float pw = W - mx*2, ph = H - my*2;
    rn.quad({mx, my, pw, ph}, theme_.panel);
    rn.quad({mx, my, pw, 4*s}, theme_.accent);
    float pad = 26*s;
    float tx = mx + pad;
    rn.text(*font_body_, font_body_->ellipsize(desc_title_, pw - pad*2),
            tx, my + 16*s, theme_.text);
    float body_top = my + 62*s, body_bot = my + ph - 46*s;
    float wrap_w = pw - pad*2;
    // Post with an attached video: a selectable thumbnail on top (A plays it).
    if (post_has_video_) {
        float tw = std::min(pw - pad*2, 360*s);
        float th = tw * 9.f/16.f;
        gfx::Rect vr{tx, body_top, tw, th};
        if (post_focus_ == 0)   // selection border
            rn.quad({vr.x-4*s, vr.y-4*s, vr.w+8*s, vr.h+8*s}, theme_.accent);
        rn.quad(vr, theme_.thumb_bg);
        gfx::Texture* vt = post_thumb_url_.empty() ? nullptr : thumbs_.get(post_thumb_url_);
        if (vt) rn.textured_cover(vr, *vt, {1,1,1,1});
        // Play glyph in the corner.
        rn.text(*font_small_, post_focus_ == 0 ? "Press A to play" : "",
                vr.x, vr.y + vr.h + 6*s, theme_.text_dim);
        body_top = vr.y + vr.h + 56*s;   // text starts well below the hint
    }
    if (desc_loading_) {
        rn.text(*font_small_, "Loading description...", tx, body_top, theme_.text_dim);
    } else {
        if (desc_lines_.empty() || desc_wrap_w_ != wrap_w) {
            desc_lines_ = wrap_text(*font_small_, desc_text_, wrap_w);
            desc_wrap_w_ = wrap_w;
        }
        float lh = font_small_->line_height() + 4*s;
        float total = desc_lines_.size() * lh;
        float view = body_bot - body_top;
        float max_scroll = total > view ? total - view : 0;
        if (desc_scroll_ < 0) desc_scroll_ = 0;
        if (desc_scroll_ > max_scroll) desc_scroll_ = max_scroll;
        float y = body_top - desc_scroll_;
        for (const auto& l : desc_lines_) {
            // Only draw lines whose top is at/below body_top so a scrolled line never
            // bleeds up into the title/divider (there's no GL scissor here).
            if (y >= body_top - 0.5f && y <= body_bot - lh * 0.4f)
                rn.text(*font_small_, l, tx, y, theme_.text_dim);
            y += lh;
        }
        if (max_scroll > 0) {   // scrollbar
            float track_h = view, knob_h = view * (view / total);
            if (knob_h < 24*s) knob_h = 24*s;
            float ky = body_top + (track_h - knob_h) * (desc_scroll_ / max_scroll);
            rn.quad({mx + pw - 8*s, body_top, 4*s, track_h}, theme_.card_sel);
            rn.quad({mx + pw - 8*s, ky, 4*s, knob_h}, theme_.accent);
        }
    }
    rn.text(*font_small_, post_has_video_ ? "A: play video    Up/Down: move    B: close"
                                          : "Up/Down: scroll    B: close",
            tx, my + ph - 34*s, theme_.text_dim);
    rn.end();
}

// ---------- Context options menu ----------
void App::open_menu() {
    menu_kind_ = MenuKind::Context;
    // Target: the playing video (in player) or the highlighted grid item.
    if (mode_ == Mode::Playing) menu_target_ = now_playing_item_;
    else { const yt::SearchResult* v = selected(); if (!v) return; menu_target_ = *v; }

    menu_items_.clear();
    const yt::SearchResult& t = menu_target_;
    if (t.is_post()) {
        menu_items_.push_back({"Read Post", MenuAction::ShowDescription});
        if (!t.video_id.empty())
            menu_items_.push_back({"Play Attached Video", MenuAction::PlayPostVideo});
    } else if (t.is_playlist()) {
        bool wl = wl_ids_.count(t.playlist_id) > 0;
        menu_items_.push_back({wl ? "Remove from Watch Later"
                                  : "Add to Watch Later", MenuAction::WatchLaterToggle});
        menu_items_.push_back({"Show Description", MenuAction::ShowDescription});
        if (!t.channel_id.empty())
            menu_items_.push_back({"Show Channel Description",
                                   MenuAction::ShowChannelDescription});
    } else if (t.is_channel()) {
        bool fav = fav_ids_.count(t.channel_id) > 0;
        menu_items_.push_back({fav ? "Remove Channel from Favorites"
                                   : "Add Channel to Favorites", MenuAction::FavoriteToggle});
        menu_items_.push_back({"Show Description", MenuAction::ShowChannelDescription});
    } else {
        if (!t.channel_id.empty()) {
            bool fav = fav_ids_.count(t.channel_id) > 0;
            menu_items_.push_back({fav ? "Remove Channel from Favorites"
                                       : "Add Channel to Favorites", MenuAction::FavoriteToggle});
        }
        bool wl = wl_ids_.count(t.video_id) > 0;
        menu_items_.push_back({wl ? "Remove from Watch Later"
                                  : "Add to Watch Later", MenuAction::WatchLaterToggle});
        if (!t.video_id.empty())
            menu_items_.push_back({"Show Description", MenuAction::ShowDescription});
        // Channel description on every video tile with a known uploader.
        if (!t.channel_id.empty())
            menu_items_.push_back({"Show Channel Description",
                                   MenuAction::ShowChannelDescription});
        // The browsed playlist's description (playlist screens only).
        if (!subview_playlist_.empty() && mode_ != Mode::Playing)
            menu_items_.push_back({"Show Playlist Description",
                                   MenuAction::ShowPlaylistDescription});
        // Quality + Speed + Stats (Left/Right) — only for the playing video.
        if (mode_ == Mode::Playing) {
            menu_items_.push_back({"Quality:  " + quality_label(play_prefs_.max_height),
                                   MenuAction::CycleMaxQuality});
            char sb[24]; std::snprintf(sb, sizeof sb, "Speed:  %gx", playback_speed_);
            menu_items_.push_back({sb, MenuAction::CycleSpeed});
            std::string cc = "Captions:  ";
            if (cc_tracks_.empty()) cc += cc_running_ ? "loading..." : "none";
            else if (cc_sel_ <= 0) cc += "Off";
            else cc += cc_tracks_[cc_sel_ - 1].name;
            menu_items_.push_back({cc, MenuAction::CycleCaptions});
            menu_items_.push_back({std::string("Stats for Nerds:  ")
                                   + (stats_for_nerds_ ? "Enabled" : "Disabled"),
                                   MenuAction::ToggleStats});
        }
        if (mode_ != Mode::Playing && !t.channel_id.empty())
            menu_items_.push_back({"Go to Channel", MenuAction::OpenChannel});
    }
    // Clear the whole watch history — only from a tile in the History view.
    if (mode_ != Mode::Playing && view_label_ == "History")
        menu_items_.push_back({"Clear History", MenuAction::ClearHistory});
    if (menu_items_.empty()) return;
    menu_sel_ = 0;   // adjust_setting saves/restores this across in-place rebuilds
    // Pause the player while the options menu is up (resumed on close). Don't re-pause
    // if we already paused it (rebuild during a quality change).
    if (mode_ == Mode::Playing && !player_.paused()) { player_.set_pause(true); menu_paused_ = true; }
    menu_open_ = true;
}
void App::menu_activate() {
    if (menu_sel_ < 0 || menu_sel_ >= (int)menu_items_.size()) return;
    const yt::SearchResult t = menu_target_;   // copy (open_channel may replace results)
    switch (menu_items_[menu_sel_].action) {
        case MenuAction::FavoriteToggle: {
            std::string name = t.is_channel() ? t.title : t.author;
            if (fav_ids_.count(t.channel_id)) { it_.remove_favorite(t.channel_id);
                status_msg_ = "Removed " + name + " from favorites"; }
            else { it_.add_favorite(t.channel_id, name);
                status_msg_ = "Added " + name + " to favorites"; }
            status_until_ = SDL_GetTicks() + 4000; refresh_favorites();
            // Un-favoriting a restricted channel makes it hideable again.
            if (hide_restricted_ && mode_ != Mode::Playing) {
                filter_hidden(results_);
                if (sel_ >= (int)results_.size())
                    sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            }
            break;
        }
        case MenuAction::WatchLaterToggle: {
            std::string wid = t.is_playlist() ? t.playlist_id : t.video_id;
            if (wl_ids_.count(wid)) { it_.remove_watch_later(wid);
                status_msg_ = "Removed from Watch Later"; }
            else { it_.add_watch_later(wid, t.title, t.is_playlist(),
                                       t.thumbnail_url, t.author, t.view_count_text);
                status_msg_ = "Added to Watch Later"; }
            status_until_ = SDL_GetTicks() + 4000; refresh_watch_later();
            break;
        }
        case MenuAction::ShowDescription: {
            menu_open_ = false;
            // If the menu auto-paused the player, keep it paused while reading —
            // the description overlay inherits the resume-on-close duty.
            desc_paused_ = menu_paused_;
            menu_paused_ = false;
            if (t.is_post()) open_post(t);
            else open_description(t);
            return;
        }
        case MenuAction::PlayPostVideo:
            menu_open_ = false;
            menu_paused_ = false;
            request_playback();          // selected() is the post; its video_id plays
            return;
        case MenuAction::ShowChannelDescription: {
            menu_open_ = false;
            desc_paused_ = menu_paused_;
            menu_paused_ = false;
            open_channel_description(t.channel_id,
                                     t.is_channel() ? t.title : t.author);
            return;
        }
        case MenuAction::ShowPlaylistDescription: {
            menu_open_ = false;
            desc_paused_ = menu_paused_;
            menu_paused_ = false;
            yt::SearchResult pl;                    // the playlist being browsed
            pl.kind = yt::SearchResult::Kind::Playlist;
            pl.playlist_id = subview_playlist_;
            pl.title = query_;                      // view header = playlist title
            open_description(pl);
            return;
        }
        case MenuAction::OpenChannel: {
            menu_open_ = false;
            yt::SearchResult ch;              // synthesize a channel target from the video
            ch.kind = yt::SearchResult::Kind::Channel;
            ch.channel_id = t.channel_id;
            ch.title = t.is_channel() ? t.title : t.author;
            open_channel(ch);
            return;
        }
        // Top-level navigation (Start menu). Reachable from anywhere, so leave the
        // player and return to the grid before swapping in the new list.
        case MenuAction::GoHome:
        case MenuAction::GoFavorites:
        case MenuAction::GoWatchLater:
        case MenuAction::GoHistory: {
            MenuAction act = menu_items_[menu_sel_].action;
            menu_open_ = false;
            menu_paused_ = false;             // navigating away; no player to resume
            if (mode_ == Mode::Playing) { save_resume_position(); player_.stop(); }
            in_channel_view_ = false;
            mode_ = Mode::Grid;
            if      (act == MenuAction::GoHome)       load_home();
            else if (act == MenuAction::GoFavorites)  load_favorites();
            else if (act == MenuAction::GoWatchLater) load_watch_later();
            else                                      load_history();
            return;
        }
        case MenuAction::GoSettings:
            open_settings();              // switch this overlay to the settings submenu
            return;
        case MenuAction::ClearHistory:
            menu_open_ = false; menu_paused_ = false;
            it_.clear_history();
            load_history();               // reload the (now empty) view
            status_msg_ = "History cleared";
            status_until_ = SDL_GetTicks() + 3000;
            return;
        case MenuAction::CycleMaxQuality:
        case MenuAction::ToggleStats:
        case MenuAction::ToggleHideRestricted:
        case MenuAction::ToggleHideShorts:
        case MenuAction::ToggleAskResume:
        case MenuAction::CycleView:
        case MenuAction::CycleVolume:
        case MenuAction::CycleHwdec:
        case MenuAction::CycleSpeed:
        case MenuAction::ToggleSponsorBlock:
        case MenuAction::CycleCaptions:
        case MenuAction::ToggleAutoplay:
        case MenuAction::CycleHomeSource:
            return;   // value rows change with Left/Right only; A does nothing
        case MenuAction::Quit:
            menu_open_ = false;
            menu_paused_ = false;
            quit_requested_ = true;
            return;
    }
    // Context action that keeps us on the player (favorite / watch-later toggle):
    // close the menu and resume playback if we auto-paused it.
    menu_open_ = false;
    if (menu_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
    menu_paused_ = false;
}
// Snapshot the current view onto the back stack (before entering a subview).
void App::push_view() {
    ViewState v;
    v.results = results_; v.query = query_; v.view_label = view_label_;
    v.cont_token = cont_token_; v.cont_endpoint = cont_endpoint_;
    v.cont_channel_id = cont_channel_id_;
    v.in_channel_view = in_channel_view_;
    v.subview_playlist = subview_playlist_;
    v.chan_tab = chan_tab_;
    v.channel_info = channel_info_;
    v.sel = sel_; v.scroll = scroll_;
    view_stack_.push_back(std::move(v));
}
bool App::pop_view() {
    if (view_stack_.empty()) return false;
    ViewState v = std::move(view_stack_.back());
    view_stack_.pop_back();
    results_ = std::move(v.results); query_ = v.query; view_label_ = v.view_label;
    cont_token_ = v.cont_token; cont_endpoint_ = v.cont_endpoint;
    cont_channel_id_ = v.cont_channel_id;
    in_channel_view_ = v.in_channel_view;
    subview_playlist_ = v.subview_playlist;
    chan_tab_ = v.chan_tab;
    channel_info_ = v.channel_info;
    sel_ = v.sel; scroll_ = v.scroll;
    tab_focus_ = false;
    build_tile_lines();          // recompute metadata lines for the restored list
    // Thumbnails re-requested lazily per-viewport by the renderer.
    return true;
}

void App::open_channel(const yt::SearchResult& ch) {
    if (ch.channel_id.empty()) return;
    // Copy id/name FIRST: `ch` references results_[sel_], which set_results() below
    // move-replaces — reading ch after that is a use-after-free.
    std::string id = ch.channel_id;
    std::string name = ch.title;
    push_view();                                        // Back returns here
    in_channel_view_ = true;
    view_label_.clear();   // subview owns the header; the stack restores the label
    subview_playlist_.clear();                          // this subview is a channel
    chan_tab_ = 0; tab_focus_ = false;                  // land on the "All" tab
    query_ = name;                                      // header shows the channel name now
    channel_info_ = {}; channel_info_.channel_id = id;  // subs/videos fill in async
    auto f = it_.channel_all_feed(id);                   // home/featured (mixed, paginated)
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    set_results(std::move(f.items));
    for (auto& r : results_)                             // channel-tab rows omit the uploader
        if (r.author.empty()) r.author = name;
    if (results_.empty()) {
        status_msg_ = "No recent uploads (or channel unavailable)";
        status_until_ = SDL_GetTicks() + 5000;
    }
    // Fetch full channel metadata (subs, video count) in the background so it
    // never holds up the video list / UI.
    if (!chinfo_running_) {
        chinfo_running_ = true; chinfo_done_ = false;
        if (chinfo_thread_.joinable()) chinfo_thread_.join();
        chinfo_thread_ = std::thread([this, id]() {
            yt::ChannelInfo info;
            try { info = it_.channel_info(id); } catch (...) {}
            { std::lock_guard<std::mutex> lk(chinfo_m_); chinfo_pending_ = std::move(info); }
            chinfo_running_ = false; chinfo_done_ = true;
        });
    }
}
// Switch to a channel tab: the highlight moves NOW, the content fetches in the
// background (poll_refresh applies it) so the UI never blocks on the network.
void App::load_channel_tab(int tab) {
    std::string id = channel_info_.channel_id;
    if (id.empty() || tab < 0 || tab > 4) return;
    chan_tab_ = tab;
    tab_focus_ = false;                                 // first video selected on load
    results_.clear(); cont_token_.clear();
    sel_ = 0; scroll_ = 0;
    refresh_current_view();
}

// Switch a Home tab. All/Videos/Shorts are instant local filters of the fetched
// home feed; Playlists fetches (async) on first visit and is cached after.
void App::load_home_tab(int tab) {
    if (tab < 0 || tab > 3) return;
    home_tab_ = tab;
    tab_focus_ = false;                                 // first video selected on load
    cont_token_.clear();
    if (tab == 3) {
        if (home_playlists_loaded_) {
            auto copy = home_playlists_;
            set_results(std::move(copy));
        } else {
            results_.clear(); sel_ = 0; scroll_ = 0;
            refresh_current_view();          // async home_playlists fetch
        }
    } else {
        apply_home_tab();
    }
}

// Rebuild results_ from the Home master list for the active All/Videos/Shorts tab.
void App::apply_home_tab() {
    std::vector<yt::SearchResult> items;
    for (const auto& r : home_items_) {
        if (home_tab_ == 1 && r.is_short) continue;     // Videos only
        if (home_tab_ == 2 && !r.is_short) continue;    // Shorts only
        items.push_back(r);
    }
    set_results(std::move(items));
}

// Open a playlist's contents as a subview — same push/pop mechanics as a channel,
// but no async channel-info fetch (a playlist header is just its title).
void App::open_playlist(const yt::SearchResult& pl) {
    if (pl.playlist_id.empty()) return;
    // Copy FIRST (pl references results_[sel_], replaced by set_results below).
    std::string id = pl.playlist_id;
    std::string name = pl.title;
    push_view();                                        // Back returns here
    in_channel_view_ = true;
    view_label_.clear();   // subview owns the header; the stack restores the label
    subview_playlist_ = id;
    tab_focus_ = false;                                 // playlists have no tab strip
    query_ = name;                                      // header shows the playlist title
    channel_info_ = {}; channel_info_.channel_id = id;  // view identity (no async fetch)
    auto f = it_.playlist_feed(id);                      // paginated via continuation
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    set_results(std::move(f.items));
    if (results_.empty()) {
        status_msg_ = "Playlist unavailable";
        status_until_ = SDL_GetTicks() + 5000;
    }
}
void App::maybe_load_more() {
    if (cont_token_.empty() || more_running_) return;
    int n = (int)results_.size();
    if (n == 0 || sel_ < n - cols_ * 2) return;   // only when near the last rows
    more_running_ = true; more_done_ = false;
    if (more_thread_.joinable()) more_thread_.join();
    yt::Innertube::Feed cursor;
    cursor.continuation = cont_token_; cursor.endpoint = cont_endpoint_;
    cursor.channel_id = cont_channel_id_;
    more_thread_ = std::thread([this, cursor]() {
        yt::Innertube::Feed next;
        try { next = it_.continue_feed(cursor); } catch (...) {}
        { std::lock_guard<std::mutex> lk(more_m_); more_pending_ = std::move(next); }
        more_running_ = false; more_done_ = true;
    });
}
void App::poll_more() {
    if (!more_done_.exchange(false)) return;
    if (more_thread_.joinable()) more_thread_.join();
    yt::Innertube::Feed next;
    { std::lock_guard<std::mutex> lk(more_m_); next = std::move(more_pending_); }
    filter_hidden(next.items);
    for (auto& v : next.items) {
        thumbs_.request(v.thumbnail_url);
        if (v.is_channel()) chan_meta_.request(v.channel_id);
    }
    size_t added = next.items.size();
    results_.insert(results_.end(), std::make_move_iterator(next.items.begin()),
                    std::make_move_iterator(next.items.end()));
    if (channel_tabs_active())         // channel-tab rows omit the uploader
        for (auto& r : results_)
            if (r.author.empty()) r.author = query_;
    build_tile_lines();                // refresh metadata-line cache for the grown list
    cont_token_ = next.continuation;   // new token, or "" at the end of the feed
    queue_restricted_checks();
    if (getenv("YTC_DEBUG"))
        std::fprintf(stderr, "[more] +%zu -> total %zu (%s)\n", added, results_.size(),
                     cont_token_.empty() ? "END" : "more available");
}

void App::poll_channel_info() {
    if (!chinfo_done_.exchange(false)) return;
    if (chinfo_thread_.joinable()) chinfo_thread_.join();
    yt::ChannelInfo info;
    { std::lock_guard<std::mutex> lk(chinfo_m_); info = std::move(chinfo_pending_); }
    // Apply only if still viewing that channel.
    if (in_channel_view_ && info.channel_id == channel_info_.channel_id)
        channel_info_ = std::move(info);
}

const yt::SearchResult* App::selected() const {
    if (sel_ < 0 || sel_ >= (int)results_.size()) return nullptr;
    return &results_[sel_];
}
void App::input(Action a) {
    // Resume prompt consumes input while open (a pre-playback modal).
    if (resume_prompt_open_) {
        switch (a) {
            case Action::Up: case Action::Down:
            case Action::Left: case Action::Right:
                resume_prompt_sel_ ^= 1; break;      // toggle Resume / Start over
            case Action::Select: {
                resume_prompt_open_ = false;
                yt::SearchResult it = resume_prompt_item_;
                double at = resume_prompt_sel_ == 0 ? resume_prompt_pos_ : 0;
                if (resume_prompt_sel_ == 1) it_.clear_resume_pos(it.video_id);
                start_resolve(it.video_id, it.title, at);
                break;
            }
            case Action::Back: resume_prompt_open_ = false; break;   // cancel: don't play
            default: break;
        }
        return;
    }
    // Description overlay consumes input while open (topmost).
    if (desc_open_) {
        float step = font_small_ ? (font_small_->line_height() + 4) * 3 : 60;
        auto close_desc = [&]{
            desc_open_ = false; desc_loading_ = false;
            if (desc_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
            desc_paused_ = false;
        };
        // Post with an attached video: navigate between the video (top) and the text.
        if (post_has_video_) {
            switch (a) {
                case Action::Up:
                    if (post_focus_ == 1) { if (desc_scroll_ > 0) desc_scroll_ -= step;
                                            else post_focus_ = 0; }
                    break;
                case Action::Down:
                    if (post_focus_ == 0) post_focus_ = 1; else desc_scroll_ += step;
                    break;
                case Action::Select:
                    if (post_focus_ == 0) {          // play the attached video
                        close_desc();
                        request_playback();          // selected() is the post; plays its video
                    }
                    break;
                case Action::Back:
                    // On the text: first B returns to the video; a second B closes.
                    if (post_focus_ == 1) { post_focus_ = 0; desc_scroll_ = 0; }
                    else close_desc();
                    break;
                case Action::Menu:  close_desc(); break;
                default: break;
            }
            return;
        }
        switch (a) {
            case Action::Up:     desc_scroll_ -= step; break;
            case Action::Down:   desc_scroll_ += step; break;
            case Action::Back:
            case Action::Menu:
            case Action::Select: close_desc(); break;
            default: break;
        }
        return;
    }
    // Options menu overlay consumes input while open.
    if (menu_open_) {
        int n = (int)menu_items_.size();
        switch (a) {
            case Action::Up:     if (menu_sel_ > 0) menu_sel_--; break;
            case Action::Down:   if (menu_sel_ + 1 < n) menu_sel_++; break;
            case Action::Left:
            case Action::Right: {
                if (menu_sel_ >= n) break;
                MenuAction act = menu_items_[menu_sel_].action;
                if (act == MenuAction::CycleMaxQuality || act == MenuAction::ToggleStats ||
                    act == MenuAction::ToggleHideRestricted || act == MenuAction::ToggleHideShorts ||
                    act == MenuAction::ToggleAskResume || act == MenuAction::CycleView ||
                    act == MenuAction::CycleVolume || act == MenuAction::CycleHwdec ||
                    act == MenuAction::CycleSpeed || act == MenuAction::ToggleSponsorBlock ||
                    act == MenuAction::CycleCaptions || act == MenuAction::ToggleAutoplay ||
                    act == MenuAction::CycleHomeSource)
                    adjust_setting(act, a == Action::Right ? +1 : -1);
                break;
            }
            case Action::Select: menu_activate(); break;
            case Action::Back:
            case Action::Menu:
                if (menu_kind_ == MenuKind::Settings) {   // Settings -> back to main menu
                    menu_open_ = false; open_main_menu();
                    break;
                }
                // Close the options menu. If the quality changed while playing, re-resolve
                // the current video at the new quality and resume the position.
                menu_open_ = false;
                if (quality_dirty_ && mode_ == Mode::Playing) {
                    quality_dirty_ = false; menu_paused_ = false;
                    replay_current(player_.position());
                } else {
                    if (menu_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
                    menu_paused_ = false;
                }
                break;
            default: break;
        }
        return;
    }
    if (mode_ == Mode::Playing) {
        controls_until_ = SDL_GetTicks() + 2600;   // any input reveals the info overlay
        switch (a) {
            case Action::Select: player_.toggle_pause(); break;
            case Action::Menu:   open_menu(); break;   // options for the playing video
            case Action::Back:   save_resume_position(); player_.stop(); mode_ = Mode::Grid; break;
            // Accumulate seeks; the actual seek fires from pump_async once the presses
            // settle (debounce). Rapid taps => one seek, not a burst of range requests.
            case Action::Left:   pending_seek_ -= 10; has_pending_seek_ = true;
                                 pending_seek_at_ = SDL_GetTicks(); break;
            case Action::Right:  pending_seek_ += 10; has_pending_seek_ = true;
                                 pending_seek_at_ = SDL_GetTicks(); break;
            case Action::Up:     adjust_volume(+5); break;   // app-local volume
            case Action::Down:   adjust_volume(-5); break;
            default: break;
        }
        return;
    }
    if (mode_ == Mode::Loading) {
        // Back cancels the pending resolve (poll_resolve discards a late result
        // once mode has left Loading); the worker still finishes in the bg.
        if (a == Action::Back) { mode_ = Mode::Grid; status_msg_ = "cancelled"; }
        return;
    }
    if (mode_ == Mode::Search) {
        switch (a) {
            case Action::Left:  if (kb_col_ > 0) kb_col_--; break;
            case Action::Right: if (kb_col_ < (int)KB[kb_row_].size()-1) kb_col_++; break;
            case Action::Up:    if (kb_row_ > 0) kb_row_--; break;
            case Action::Down:  if (kb_row_ < (int)KB.size()-1) kb_row_++; break;
            case Action::Select: kb_activate(); break;
            case Action::Search: submit_search(); break;      // Y submits
            case Action::Back:   mode_ = Mode::Grid; break;    // cancel
            default: break;
        }
        if (kb_col_ >= (int)KB[kb_row_].size()) kb_col_ = KB[kb_row_].size()-1;
        return;
    }
    // Grid / carousel browse. Any user input here cancels a pending autoplay countdown.
    cancel_autoplay();
    if (a == Action::Search) { open_search(); return; }
    if (a == Action::Menu) { open_menu(); return; }   // options for the highlighted item
    if (a == Action::Back) {
        // First B jumps to the top of the list (first tile selected); only a second B
        // (already at the first tile) does the view's normal back.
        if (!results_.empty() && sel_ != 0) {
            sel_ = 0; scroll_ = 0; carousel_pos_ = 0; ensure_visible();
            return;
        }
        if (pop_view()) return;                    // unwind one subview level
        // Nothing to go back to: a top-level view that isn't Home falls back to
        // Home; on Home itself, Back does nothing (never quits).
        if (!query_.empty() || !view_label_.empty()) load_home();
        return;
    }
    // Tabs (channel views AND Home) are switched only with the L/R shoulders
    // (cycle_tab); the d-pad stays entirely in the grid.
    int n = (int)results_.size();
    if (n == 0) return;
    auto activate = [&]{
        const yt::SearchResult* v = selected();
        if (v && v->is_channel()) open_channel(*v);        // open channel -> its uploads
        else if (v && v->is_playlist()) open_playlist(*v); // open playlist -> its videos
        else if (v && v->is_post()) open_post(*v);         // read the full post
        else request_playback();                            // play video
    };
    if (view_mode_ != ViewMode::Grid) {   // carousel & coverflow are 1-D strips
        switch (a) {
            case Action::Left:  if (sel_ > 0) sel_--; break;
            case Action::Right: if (sel_ + 1 < n) sel_++; break;
            case Action::Select: activate(); break;
            default: break;
        }
        return;   // carousel_pos_ eases toward sel_ in render_carousel/coverflow
    }
    switch (a) {
        case Action::Left:  if (sel_ % cols_ != 0) sel_--; break;
        case Action::Right: if (sel_ % cols_ != cols_-1 && sel_+1 < n) sel_++; break;
        case Action::Up:    if (sel_ - cols_ >= 0) sel_ -= cols_; break;
        case Action::Down:  if (sel_ + cols_ < n) sel_ += cols_; break;
        case Action::Select: activate(); break;
        default: break;
    }
    ensure_visible();
}

static std::string sb_category_label(const std::string& c) {
    if (c == "sponsor")        return "sponsor";
    if (c == "selfpromo")      return "self-promo";
    if (c == "interaction")    return "interaction reminder";
    if (c == "intro")          return "intro";
    if (c == "outro")          return "outro";
    if (c == "music_offtopic") return "non-music section";
    if (c == "preview")        return "recap";
    if (c == "filler")         return "filler";
    return c.empty() ? "segment" : c;
}

void App::pump_async() {
    thumbs_.pump();
    poll_resolve();
    poll_channel_info();
    poll_more();
    poll_refresh();
    poll_description();
    poll_sponsorblock();
    poll_captions();
    poll_caption_download();
    poll_related_autoplay();
    step_autoplay();
    // SponsorBlock: auto-skip when playback enters a segment. Uses an immediate
    // (non-debounced) seek to the segment end; each segment skips at most once per
    // play so a deliberate seek back in doesn't fight the user.
    if (sponsorblock_ && mode_ == Mode::Playing && !menu_open_ &&
        !sb_segments_.empty() && !player_.paused()) {
        double pos = player_.position();
        for (size_t i = 0; i < sb_segments_.size(); ++i) {
            const auto& sg = sb_segments_[i];
            if (i < sb_skipped_.size() && !sb_skipped_[i] &&
                pos >= sg.start && pos < sg.end - 0.20) {
                player_.seek_relative(sg.end - pos);
                sb_skipped_[i] = true;
                played_max_ = std::max(played_max_, sg.end);
                if (getenv("YTC_DEBUG"))
                    std::fprintf(stderr, "[sponsorblock] skipped %s [%.1f-%.1f]\n",
                                 sg.category.c_str(), sg.start, sg.end);
                status_msg_ = "Skipped " + sb_category_label(sg.category);
                status_until_ = SDL_GetTicks() + 1600;
                break;
            }
        }
    }
    // Fire a scheduled network-retry once its backoff elapses.
    if (retry_pending_ && !refresh_running_ && mode_ != Mode::Playing &&
        SDL_GetTicks() >= retry_at_) {
        retry_pending_ = false;
        refresh_current_view(/*is_retry=*/true);
    }
    maybe_load_more();
    // Apply freshly-arrived restricted verdicts to the visible list.
    if (hide_restricted_ && rcheck_.drain_dirty() && mode_ != Mode::Playing) {
        size_t before = results_.size();
        filter_hidden(results_);
        if (results_.size() != before) {
            if (sel_ >= (int)results_.size())
                sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            ensure_visible();
        }
    }
    if (mode_ == Mode::Playing) {
        // Track the furthest position actually played (anchors the paced-seek window).
        double p = player_.position();
        if (p > played_max_) played_max_ = p;
        // Fire a debounced seek once the rapid presses settle (~350ms quiet).
        if (has_pending_seek_ && SDL_GetTicks() - pending_seek_at_ >= 350) {
            double delta = pending_seek_;
            // Restricted (paced) streams 403 any read outside the server's opaque
            // sliding window, so ONLY allow seeks inside mpv's OWN cache — those
            // never touch the network. Forward: up to the buffered end; backward:
            // bounded by the back-buffer (approximate, conservative).
            if (playing_paced_) {
                double target = p + delta;
                if (delta > 0) {
                    double limit = player_.cached_until() - 2.0;   // stay inside cache
                    if (limit < p) limit = p;
                    if (target > limit) {
                        delta = limit - p;
                        status_msg_ = "Seeking limited on this video (paced stream)";
                        status_until_ = SDL_GetTicks() + 4000;
                    }
                } else if (delta < 0) {
                    double floor_t = p - 60.0;                     // ~back-buffer span
                    if (floor_t < 0) floor_t = 0;
                    if (target < floor_t) {
                        delta = floor_t - p;
                        status_msg_ = "Seeking limited on this video (paced stream)";
                        status_until_ = SDL_GetTicks() + 4000;
                    }
                }
                if (delta > -0.5 && delta < 0.5) delta = 0;        // nothing meaningful left
            }
            if (delta != 0) player_.seek_relative(delta);
            has_pending_seek_ = false; pending_seek_ = 0;
        }
        if (!player_.pump()) {       // playback ended (EOF/error)
            handle_playback_ended();  // autoplay next / related, or back to grid
        }
    }
}

void App::toggle_view() {
    view_mode_ = (ViewMode)(((int)view_mode_ + 1) % 4);   // Grid->Carousel->3D->Coverflow
    it_.set_setting_int("view", (int)view_mode_);
    carousel_pos_ = sel_; carousel_vel_ = 0;
}
// Critically-damped spring (Unity-style SmoothDamp) glide of carousel_pos_ toward
// the selected index — frame-rate independent, velocity-continuous, so a press
// spins smoothly into place and rapid presses flow together without restarting.
void App::update_carousel_anim() {
    unsigned now = SDL_GetTicks();
    float dt = anim_last_ms_ ? (now - anim_last_ms_) / 1000.f : 0.016f;
    anim_last_ms_ = now;
    if (dt > 0.05f) dt = 0.05f;                 // clamp after a hitch / view switch
    const float smoothTime = 0.20f;             // ~time to settle; higher = slower spin
    float target = (float)sel_;
    float omega = 2.f / smoothTime;
    float x = omega * dt;
    float ex = 1.f / (1.f + x + 0.48f*x*x + 0.235f*x*x*x);
    float change = carousel_pos_ - target;
    float temp = (carousel_vel_ + omega * change) * dt;
    carousel_vel_ = (carousel_vel_ - omega * temp) * ex;
    carousel_pos_ = target + (change + temp) * ex;
    if (std::abs(carousel_pos_ - target) < 0.001f && std::abs(carousel_vel_) < 0.02f) {
        carousel_pos_ = target; carousel_vel_ = 0;
    }
}
// L/R shoulders: switch the active tab (channel: 0..4, Home: 0..3). No-op elsewhere
// or while a menu/overlay is up.
void App::cycle_tab(int dir) {
    if (menu_open_ || desc_open_ || resume_prompt_open_ || mode_ != Mode::Grid) return;
    if (channel_tabs_active()) {
        int t = chan_tab_ + dir; if (t < 0 || t > 4) return;
        load_channel_tab(t);
    } else if (home_tabs_active()) {
        int t = home_tab_ + dir; if (t < 0 || t > 3) return;
        load_home_tab(t);
    } else return;
    tab_focus_ = false;   // shoulder-switch keeps focus on content, not the strip
}

void App::render(gfx::Renderer& rn) {
    if (mode_ == Mode::Playing)      render_player(rn);
    else if (mode_ == Mode::Search)  render_search(rn);
    else {
        switch (view_mode_) {
            case ViewMode::Carousel:   render_carousel(rn); break;
            case ViewMode::Carousel3D: render_carousel3d(rn); break;
            case ViewMode::Coverflow:  render_coverflow(rn); break;
            default:                   render_grid(rn); break;
        }
        if (mode_ == Mode::Loading) render_loading(rn);
    }
    if (menu_open_) render_menu(rn);   // overlay on top of whatever view
    if (desc_open_) render_description(rn);   // description floats above everything
    if (resume_prompt_open_) render_resume_prompt(rn);
}

void App::render_menu(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.72f));
    int n = (int)menu_items_.size();
    // Settings rows carry a "Label:  Value" so they need more room than the
    // context menu; widen that panel to keep values from crowding the edge.
    float iw = std::min((menu_kind_ == MenuKind::Settings ? 620.f : 560.f)*s, W*0.86f);
    float ih = 58*s, gap = 8*s;
    float title_h = 56*s, foot_h = 34*s;                 // reserve the footer inside the panel
    // Clamp to the screen and scroll the item list when it's too tall to fit.
    float margin = 22*s;
    float items_budget = (float)H - 2*margin - 48*s - title_h - foot_h;
    int max_vis = std::max(1, (int)(items_budget / (ih + gap)));
    int vis = std::min(n, max_vis);
    int first = 0;
    if (n > vis) {                                        // keep the selection in view
        first = menu_sel_ - vis / 2;
        if (first < 0) first = 0;
        if (first > n - vis) first = n - vis;
    }
    float ph = title_h + vis*(ih+gap) + foot_h;
    float px = (W-iw)/2, py = (H-ph)/2;
    rn.quad({px-24*s, py-24*s, iw+48*s, ph+48*s}, theme_.panel);
    rn.quad({px-24*s, py-24*s, iw+48*s, 4*s}, theme_.accent);
    std::string heading = (menu_kind_ == MenuKind::Main) ? "Menu"
                        : (menu_kind_ == MenuKind::Settings) ? "Settings"
                          : font_body_->ellipsize(menu_target_.title, iw - 4*s);
    rn.text(*font_body_, heading, px, py, theme_.text_dim);
    // Scroll hints (more items above/below the window).
    if (first > 0) rn.text(*font_small_, "▲", px + iw - 18*s, py, theme_.text_dim);
    if (first + vis < n) rn.text(*font_small_, "▼", px + iw - 18*s,
                                 py + title_h + vis*(ih+gap) - 20*s, theme_.text_dim);
    float iy = py + title_h;
    for (int k = 0; k < vis; ++k) {
        int i = first + k;
        bool sel = (i == menu_sel_);
        rn.quad({px, iy, iw, ih}, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({px, iy, 4*s, ih}, theme_.accent);
        rn.text(*font_body_, font_body_->ellipsize(menu_items_[i].label, iw - 36*s),
                px + 18*s, iy + (ih-font_body_->line_height())/2 + 3*s,
                sel ? theme_.text : theme_.text_dim);
        iy += ih + gap;
    }
    bool has_value = false;
    for (auto& it : menu_items_)
        if (it.action == MenuAction::CycleMaxQuality || it.action == MenuAction::ToggleStats ||
            it.action == MenuAction::ToggleHideRestricted || it.action == MenuAction::ToggleHideShorts ||
            it.action == MenuAction::ToggleAskResume || it.action == MenuAction::CycleView ||
            it.action == MenuAction::CycleVolume || it.action == MenuAction::CycleHwdec)
            has_value = true;
    const char* foot = (menu_kind_ == MenuKind::Settings)
                     ? "Left/Right: change    B: back"
                     : has_value ? "A: select    Left/Right: change    B: close"
                     : "A: select    B: close";
    rn.text(*font_small_, foot, px, iy + 10*s, theme_.text_dim);
    rn.end();
}

// Draw the thumbnail area for one item into rect r (kind-aware), tinted by alpha.
// Shared by grid / carousel / coverflow.
void App::draw_thumb(gfx::Renderer& rn, const yt::SearchResult& v,
                     const gfx::Rect& r, float s, float alpha) {
    auto A  = [&](gfx::Color c){ return c.with_a(c.a * alpha); };
    gfx::Color tint{1,1,1,alpha};
    rn.quad(r, A(theme_.thumb_bg));

    if (v.is_channel()) {
        float av = r.h * 0.82f;
        gfx::Rect ar{r.x + (r.w-av)/2, r.y + (r.h-av)/2, av, av};
        std::string url = v.thumbnail_url;
        if (url.empty()) { url = chan_meta_.avatar(v.channel_id);
            if (!url.empty()) thumbs_.request(url); }
        if (auto* tex = url.empty() ? nullptr : thumbs_.get(url)) rn.textured_cover(ar, *tex, tint);
        else {
            rn.quad(ar, A(theme_.card_sel));
            std::string ini(1, v.title.empty() ? '?' : (char)std::toupper((unsigned char)v.title[0]));
            rn.text(*font_title_, ini, ar.x + av/2 - font_title_->text_width(ini)/2,
                    ar.y + av/2 - font_title_->line_height()/2, A(theme_.text));
        }
    } else if (v.is_post()) {
        gfx::Texture* ptex = v.thumbnail_url.empty() ? nullptr : thumbs_.get(v.thumbnail_url);
        if (ptex) rn.textured_cover(r, *ptex, tint);
        else {
            rn.quad(r, A(theme_.card_sel));
            float lw = r.w - 24*s, ly = r.y + 12*s, lh = font_small_->line_height() + 4*s;
            int maxl = (int)((r.h - 24*s) / lh);
            size_t i2 = 0; int ln = 0; const std::string& text = v.title;
            while (ln < maxl && i2 < text.size()) {
                size_t start = i2; std::string line;
                while (i2 < text.size() && text[i2] != '\n') {
                    size_t adv = 1; uint8_t c = (uint8_t)text[i2];
                    if ((c>>5)==0x6) adv=2; else if ((c>>4)==0xE) adv=3; else if ((c>>3)==0x1E) adv=4;
                    std::string cand = line + text.substr(i2, adv);
                    if (font_small_->text_width(cand) > lw) break;
                    line = std::move(cand); i2 += adv;
                }
                if (i2 < text.size() && text[i2]=='\n') ++i2;
                if (line.empty() && start==i2) break;
                rn.text(*font_small_, line, r.x + 12*s, ly, A(theme_.text_dim));
                ly += lh; ++ln;
            }
        }
    } else {
        // video / short / playlist: cover image + a corner pill.
        if (auto* tex = v.thumbnail_url.empty() ? nullptr : thumbs_.get(v.thumbnail_url))
            rn.textured_cover(r, *tex, tint);
        else rn.text(*font_small_, "loading...", r.x + 12*s, r.y + r.h/2 - 9*s, A(theme_.text_dim));
        // No badge on Shorts; videos show duration, playlists show the item count.
        std::string pill = v.is_short ? std::string()
                         : v.is_playlist() ? v.view_count_text
                                           : v.length_text;
        if (!pill.empty()) {
            float pw = font_small_->text_width(pill) + 12*s;
            gfx::Rect pr{r.x + r.w - pw - 8*s, r.y + r.h - 26*s, pw, 22*s};
            rn.quad(pr, gfx::Color{0,0,0,0.75f*alpha});
            rn.text(*font_small_, pill, pr.x + 6*s, pr.y + 2*s, A(theme_.text));
        }
    }
    // FAV badge only on favorite CHANNEL tiles.
    if (v.is_channel() && !v.channel_id.empty() && fav_ids_.count(v.channel_id)) {
        float bw = font_small_->text_width("FAV") + 12*s;
        rn.quad({r.x + 6*s, r.y + 6*s, bw, 22*s}, A(theme_.accent));
        rn.text(*font_small_, "FAV", r.x + 12*s, r.y + 8*s, A(theme_.bg));
    }
}

// Compose the raw metadata lines for one result (no ellipsize — that's width-dependent
// and done at draw time). Called once per item when results_ changes, so humanize_age
// and the string building don't run every frame.
App::TileLines App::compose_lines(const yt::SearchResult& v, ChannelMetaCache& cmeta) {
    TileLines t;
    if (v.is_channel()) {
        t.l1 = v.title; t.l2 = v.subs_text;
        t.l3 = cmeta.video_count(v.channel_id); if (t.l3.empty()) t.l3 = v.author;
    } else if (v.is_post()) {
        t.l1 = v.title.substr(0, v.title.find('\n'));
        t.l2 = v.view_count_text;
        if (!v.published_text.empty()) t.l2 += (t.l2.empty()?"":"   -   ") + v.published_text;
        t.l3 = v.video_id.empty() ? "Post" : "Post - has video";
    } else if (v.is_playlist()) {
        t.l1 = v.title; t.l2 = v.author; t.l3 = "Playlist";
    } else {
        t.l1 = v.title; t.l2 = v.author;
        std::string age = humanize_age(v.published_text);
        t.l3 = v.view_count_text;
        if (!age.empty()) t.l3 += (t.l3.empty()?"":"   -   ") + age;
    }
    return t;
}
void App::build_tile_lines() {
    tile_lines_.clear();
    tile_lines_.reserve(results_.size());
    for (const auto& v : results_) tile_lines_.push_back(compose_lines(v, chan_meta_));
}
// Draw the 3 metadata lines for item idx at (x,y), width maxw, tinted by alpha. Uses
// the precomputed raw lines and only allocates a truncated string when text overflows.
void App::draw_meta(gfx::Renderer& rn, const yt::SearchResult& v, int idx,
                    float x, float y, float maxw, float s, float alpha) {
    gfx::Color tc = theme_.text.with_a(alpha), dc = theme_.text_dim.with_a(alpha);
    TileLines local;
    const TileLines* t;
    if (idx >= 0 && idx < (int)tile_lines_.size()) t = &tile_lines_[idx];
    else { local = compose_lines(v, chan_meta_); t = &local; }   // fallback
    // Channel video-count may arrive after the cache was built — refresh l3 live.
    std::string chan_l3;
    if (v.is_channel()) { chan_l3 = chan_meta_.video_count(v.channel_id);
        if (chan_l3.empty()) chan_l3 = t->l3; }
    const std::string& l1 = t->l1;
    const std::string& l2 = t->l2;
    const std::string& l3 = v.is_channel() ? chan_l3 : t->l3;
    float lh1 = font_body_->line_height(), lh2 = font_small_->line_height();
    float y2 = y + lh1 + 4*s, y3 = y2 + lh2 + 3*s;
    // Emit without copying when the line already fits (ellipsize() copies unconditionally).
    auto emit = [&](gfx::Font& f, const std::string& raw, float yy, gfx::Color c) {
        if (raw.empty()) return;
        if (f.text_width(raw) <= maxw) rn.text(f, raw, x, yy, c);
        else rn.text(f, f.ellipsize(raw, maxw), x, yy, c);
    };
    emit(*font_body_,  l1, y,  tc);
    emit(*font_small_, l2, y2, dc);
    emit(*font_small_, l3, y3, dc);
}

// Header bar (title + subtitle + count) and the channel/Home tab strip, drawn at
// vertical offset hy (grid passes -scroll_ so it slides away; carousel/coverflow
// pass 0 so it stays pinned). Shared by all browse views.
void App::render_browse_chrome(gfx::Renderer& rn, float hy) {
    const int W = win_->width();
    float s = win_->height() / 720.f;
    float hbar = 84 * s;
    rn.quad({0, hy, (float)W, hbar}, theme_.panel);
    rn.quad({0, hy+hbar-3*s, (float)W, 3*s}, theme_.accent);
    rn.text(*font_title_, "YTC", 32*s, hy+24*s, theme_.text);
    std::string sub;
    if (in_channel_view_) {
        sub = query_;
        std::string extra = channel_info_.subs_text;
        if (!channel_info_.video_count_text.empty())
            extra += (extra.empty() ? "" : "   -   ") + channel_info_.video_count_text;
        if (!extra.empty()) sub += "    -    " + extra;
    } else if (!view_label_.empty()) sub = view_label_;
    else if (query_.empty())         sub = "Home";
    else                             sub = "search: " + query_;
    // Right-aligned count/status; measure it first so the subtitle can be ellipsized
    // to the exact gap and never slides under it (the count grows with "loading more...").
    std::string count = std::to_string(results_.size()) + " results";
    if (more_running_) count += "  -  loading more...";
    if (refresh_running_) count = "loading...";
    float count_w = font_small_->text_width(count);
    rn.text(*font_small_, count, W - count_w - 32*s, hy+34*s, theme_.text_dim);
    // Subtitle starts clear of the logo (its real width), ends clear of the count.
    float sub_x = std::max(240*s, 32*s + font_title_->text_width("YTC") + 28*s);
    float sub_w = (W - count_w - 32*s - 20*s) - sub_x;
    if (sub_w > 40*s)
        rn.text(*font_body_, font_body_->ellipsize(sub, sub_w), sub_x, hy+32*s, theme_.text_dim);

    if (channel_tabs_active() || home_tabs_active()) {
        int active_tab = channel_tabs_active() ? chan_tab_ : home_tab_;
        GridMetrics tm = grid_metrics();
        float ty0 = hy + hbar;
        rn.quad({0, ty0, (float)W, tm.tabs_h}, theme_.panel.with_a(0.6f));
        static const char* kTabs[] = {"All", "Videos", "Shorts", "Playlists", "Posts"};
        int ntabs = channel_tabs_active() ? 5 : 4;
        float tx = 32 * s;
        for (int i = 0; i < ntabs; ++i) {
            float tw = font_body_->text_width(kTabs[i]);
            float chip_w = tw + 36 * s, chip_h = tm.tabs_h - 12 * s;
            gfx::Rect chip{tx, ty0 + 6 * s, chip_w, chip_h};
            bool st = (i == active_tab);
            // Active tab is always the red accent (via D-pad on the strip OR the L/R
            // shoulders) — no focus outline.
            if (st) rn.quad(chip, theme_.accent);
            rn.text(*font_body_, kTabs[i], tx + 18 * s,
                    ty0 + (tm.tabs_h - font_body_->line_height()) / 2 + 2 * s,
                    st ? theme_.text : theme_.text_dim);
            tx += chip_w + 10 * s;
        }
    }
}

void App::render_grid(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);

    // Header bar — part of the scrolled content, so it slides off the top as you
    // scroll (only visible at the top). Footer stays pinned.
    float hbar = 84 * s;
    render_browse_chrome(rn, -scroll_);   // header + tab strip (scrolls away)

    // Empty state: no videos yet (default startup, or an empty search). Guide the
    // user to search; the Latest feed populates once favorite channels are added.
    if (results_.empty()) {
        const char* l1; const char* l2 = "Press Y  (or / )  to search"; const char* l3;
        if (retry_pending_ || (refresh_running_ && retry_attempt_ > 0)) {
            l1 = "Waiting for network...";
            l2 = "Reconnecting automatically";
            l3 = "";
        } else if (channel_tabs_active() ||
            (home_tabs_active() && !home_items_.empty()) ||
            (home_tabs_active() && refresh_running_)) {
            l1 = refresh_running_ ? "Loading..." : "Nothing in this tab";
            l2 = refresh_running_ ? "" : "L/R shoulders: switch tabs";
            l3 = "";
        } else if (view_label_ == "Favorite Channels") {
            l1 = "No favorite channels yet";
            l2 = "Open a channel, press Select, and Add to Favorites";
            l3 = "Your favorites show up here for quick access";
        } else if (view_label_ == "Watch Later") {
            l1 = "Watch Later is empty";
            l2 = "Press Select on a video and Add to Watch Later";
            l3 = "Saved videos show up here";
        } else if (view_label_ == "History") {
            l1 = "No watch history yet";
            l2 = "Videos you play show up here";
            l3 = "History is stored locally on this device";
        } else {
            l1 = query_.empty() ? "No videos yet" : "No results";
            l3 = query_.empty()
                ? "Your Home feed fills in once you add favorite channels"
                : "Try a different search, or check your connection";
        }
        rn.text(*font_title_, l1, (W - font_title_->text_width(l1))/2, H*0.40f, theme_.text);
        rn.text(*font_body_,  l2, (W - font_body_->text_width(l2))/2,  H*0.40f + 52*s, theme_.accent);
        rn.text(*font_small_, l3, (W - font_small_->text_width(l3))/2, H*0.40f + 92*s, theme_.text_dim);
        // Footer hints still useful.
        float fh0 = 44*s;
        rn.quad({0, H-fh0, (float)W, fh0}, theme_.panel);
        rn.text(*font_small_, "Y: search    Start: menu    V: grid/carousel",
                32*s, H - fh0 + 12*s, theme_.text_dim);
        rn.end();
        return;
    }

    // Grid geometry (shared with ensure_visible via grid_metrics()).
    GridMetrics m = grid_metrics();
    float pad = m.pad, top = m.top, gutter = m.gutter, cardw = m.cardw,
          thumbh = m.thumbh, cardh = m.cardh, rowstep = m.rowstep;

    // Iterate only the rows in (and just around) the viewport instead of all N — the
    // prefetch band is ~2 rows, so ±3 rows of margin covers it.
    int ncards = (int)results_.size();
    int first_row = std::max(0, (int)((scroll_ - top) / rowstep) - 3);
    int last_row  = (int)((scroll_ - top + H) / rowstep) + 3;
    int first_i = first_row * cols_;
    int last_i  = std::min(ncards, (last_row + 1) * cols_);
    for (int i = first_i; i < last_i; ++i) {
        int col = i % cols_, row = i / cols_;
        float x = pad + col*(cardw+gutter);
        float y = top + row*rowstep - scroll_;
        // Prefetch thumbnails for a band around the viewport (~2 rows of slack), so we
        // only ever download/keep textures near what's on screen (LRU caps the rest).
        if (y + cardh > -2*rowstep && y < H + 2*rowstep) {
            const yt::SearchResult& pv = results_[i];
            if (!pv.thumbnail_url.empty()) thumbs_.request(pv.thumbnail_url);
            else if (pv.is_channel()) { std::string a = chan_meta_.avatar(pv.channel_id);
                                        if (!a.empty()) thumbs_.request(a); }
        }
        if (y + cardh < 0 || y > H) continue;       // cull offscreen (header scrolls away)
        bool sel = (i == sel_) && !tab_focus_;

        // Card background + selection ring. The meta/footer keeps its normal color
        // when selected — the accent border alone marks the highlight.
        gfx::Rect card{x, y, cardw, cardh};
        if (sel) rn.quad({x-4*s, y-4*s, cardw+8*s, cardh+8*s}, theme_.accent);
        rn.quad(card, theme_.card);

        const auto& v = results_[i];
        draw_thumb(rn, v, {x, y, cardw, thumbh}, s, 1.0f);
        draw_meta(rn, v, i, x + 12*s, y + thumbh + 8*s, cardw - 24*s, s, 1.0f);
    }

    // Footer hint bar.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "D-Pad: move    A: play/open    Select: options    Start: menu    Y: search    B: back",
            32*s, H - fh + 12*s, theme_.text_dim);

    // Transient status banner (e.g. "This live event will begin in 3 days.").
    draw_status_banner(rn, hbar + 22*s, s);
    rn.end();
}

// Transient status banner (resolve errors, "Added X to favorites", etc.). Width is
// clamped to the screen and the message ellipsized, so a long channel name never
// runs off both edges. Shared by every view's render.
void App::draw_status_banner(gfx::Renderer& rn, float top_y, float s) {
    if (status_msg_.empty() || SDL_GetTicks() >= status_until_) return;
    const int W = win_->width();
    std::string msg = font_body_->ellipsize(status_msg_, W - 96*s);
    float tw = font_body_->text_width(msg);
    float bw = tw + 48*s, bh = 52*s;
    float bx = (W - bw)/2;
    rn.quad({bx, top_y, bw, bh}, theme_.panel);
    rn.quad({bx, top_y, 5*s, bh}, theme_.accent);
    rn.text(*font_body_, msg, bx + 24*s, top_y + (bh-font_body_->line_height())/2 + 3*s,
            theme_.text);
}

// Shared empty/loading centre text for the non-grid views. Returns true if it drew
// something (i.e. there are no tiles to show).
bool App::browse_empty_overlay(gfx::Renderer& rn) {
    if (!results_.empty()) return false;
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    const char* l1 = "No videos yet";
    if (retry_pending_ || (refresh_running_ && retry_attempt_ > 0)) l1 = "Waiting for network...";
    else if (refresh_running_) l1 = "Loading...";
    else if (!query_.empty()) l1 = "No results";
    else if (view_label_ == "Watch Later") l1 = "Watch Later is empty";
    else if (view_label_ == "History") l1 = "No watch history yet";
    else if (view_label_ == "Favorite Channels") l1 = "No favorite channels yet";
    rn.text(*font_title_, l1, (W - font_title_->text_width(l1))/2, H*0.45f, theme_.text);
    return true;
}

void App::render_carousel(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);
    float hbar = 84 * s;
    render_browse_chrome(rn, 0);          // pinned header + tab strip

    int n = (int)results_.size();
    if (n == 0) { browse_empty_overlay(rn); rn.end(); return; }

    update_carousel_anim();

    float top = hbar + (channel_tabs_active() || home_tabs_active() ? 52*s : 0);
    float cx = W / 2.f, cy = top + (H - top) * 0.40f;
    float big_w = std::min(W * 0.46f, (H - top) * 0.60f * 16.f/9.f);
    float big_h = big_w * 9.f / 16.f;
    float spacing = big_w * 0.62f;

    for (int i = std::max(0, sel_-4); i < std::min(n, sel_+5); ++i)
        thumbs_.request(results_[i].thumbnail_url);

    auto& vis = vis_; vis.clear();       // reused scratch; compute range, don't scan all n
    { int lo = std::max(0, (int)std::floor(carousel_pos_ - 3.2f));
      int hi = std::min(n-1, (int)std::ceil(carousel_pos_ + 3.2f));
      for (int i = lo; i <= hi; ++i)
          if (std::abs(i - carousel_pos_) <= 3.2f) vis.push_back(i); }
    std::sort(vis.begin(), vis.end(), [&](int a, int b){
        return std::abs(a - carousel_pos_) > std::abs(b - carousel_pos_); });

    for (int i : vis) {
        float d = i - carousel_pos_, dist = std::abs(d);
        float scale = std::max(0.55f, 1.0f - dist * 0.16f);
        float w = big_w * scale, h = big_h * scale;
        float x = cx + d * spacing - w/2, y = cy - h/2;
        float a = std::max(0.30f, 1.0f - dist * 0.28f);
        if (i == sel_ && !tab_focus_) rn.quad({x-4*s, y-4*s, w+8*s, h+8*s}, theme_.accent);
        draw_thumb(rn, results_[i], {x, y, w, h}, s, a);
    }

    // Centered item metadata below the strip (full, kind-aware, centered-ish).
    const auto& v = results_[sel_];
    float mw = W * 0.7f, mx = (W - mw)/2, my = cy + big_h/2 + 24*s;
    draw_meta(rn, v, sel_, mx, my, mw, s, 1.0f);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, my + 82*s, theme_.text_dim);

    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "Left/Right: browse    A: open    L/R: switch tabs    V: view    B: back",
            32*s, H - fh + 12*s, theme_.text_dim);
    draw_status_banner(rn, hbar + 16*s, s);
    rn.end();
}

// 3D carousel: a spread horizontal flow, the centre tile flat and large, side
// tiles rotated inward (faux-perspective trapezoids) spaced out, receding + dimming.
void App::render_carousel3d(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);
    float hbar = 84 * s;
    render_browse_chrome(rn, 0);

    int n = (int)results_.size();
    if (n == 0) { browse_empty_overlay(rn); rn.end(); return; }

    update_carousel_anim();

    float top = hbar + (channel_tabs_active() || home_tabs_active() ? 52*s : 0);
    float cx = W / 2.f, cy = top + (H - top) * 0.42f;
    float cardW = std::min(W * 0.40f, (H - top) * 0.58f * 16.f/9.f);
    float cardH = cardW * 9.f / 16.f;
    float spacing = cardW * 0.56f;        // neighbours spread out beside the centre card

    for (int i = std::max(0, sel_-5); i < std::min(n, sel_+6); ++i)
        thumbs_.request(results_[i].thumbnail_url);

    auto& vis = vis_; vis.clear();       // reused scratch; compute range, don't scan all n
    { int lo = std::max(0, (int)std::floor(carousel_pos_ - 4.5f));
      int hi = std::min(n-1, (int)std::ceil(carousel_pos_ + 4.5f));
      for (int i = lo; i <= hi; ++i)
          if (std::abs(i - carousel_pos_) <= 4.5f) vis.push_back(i); }
    std::sort(vis.begin(), vis.end(), [&](int a, int b){
        return std::abs(a - carousel_pos_) > std::abs(b - carousel_pos_); });

    const float kMaxAng = 1.05f, kPersp = 0.32f;   // ~60deg, perspective strength
    for (int i : vis) {
        float d = i - carousel_pos_, dist = std::abs(d);
        bool center = std::abs(d) < 0.12f;
        float sc = std::max(0.62f, 1.0f - dist * 0.10f);
        float hw = cardW*0.5f*sc, hh = cardH*0.5f*sc;
        float x = cx + d * spacing;      // continuous in d -> no jump as it reaches centre
        float alpha = std::max(0.25f, 1.0f - dist * 0.24f);

        if (center) {                      // flat, full tile (image + pills + badges)
            gfx::Rect r{x - hw, cy - hh, hw*2, hh*2};
            if (!tab_focus_) rn.quad({r.x-4*s, r.y-4*s, r.w+8*s, r.h+8*s}, theme_.accent);
            draw_thumb(rn, results_[i], r, s, 1.0f);
            continue;
        }
        float ang = std::max(-kMaxAng, std::min(kMaxAng, d)) * 0.9f;
        float cosA = std::cos(ang), sinA = std::sin(ang);
        float lX = x - hw*cosA, rX = x + hw*cosA;
        float lY = hh * (1.0f + kPersp*sinA);   // left edge half-height
        float rY = hh * (1.0f - kPersp*sinA);   // right edge half-height
        const auto& v = results_[i];
        gfx::Texture* tex = v.thumbnail_url.empty() ? nullptr : thumbs_.get(v.thumbnail_url);
        if (tex)
            rn.textured_quad4(lX, cy-lY, rX, cy-rY, rX, cy+rY, lX, cy+lY, *tex,
                              gfx::Color{1,1,1,alpha});
        else
            rn.quad4(lX, cy-lY, rX, cy-rY, rX, cy+rY, lX, cy+lY,
                     theme_.card_sel.with_a(alpha));
    }

    // Centre metadata below the flow.
    float mw = W * 0.7f, mx = (W - mw)/2, my = cy + cardH*0.5f + 30*s;
    draw_meta(rn, results_[sel_], sel_, mx, my, mw, s, 1.0f);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, my + 82*s, theme_.text_dim);

    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "Left/Right: browse    A: open    L/R: switch tabs    V: view    B: back",
            32*s, H - fh + 12*s, theme_.text_dim);
    draw_status_banner(rn, hbar + 16*s, s);
    rn.end();
}

// Traditional coverflow: side cards parked at a FIXED inward tilt, stacked close
// together and receding; the transitioning card folds from tilted -> flat as it
// reaches centre, then folds to the other side (its outer edge swinging back).
void App::render_coverflow(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);
    float hbar = 84 * s;
    render_browse_chrome(rn, 0);

    int n = (int)results_.size();
    if (n == 0) { browse_empty_overlay(rn); rn.end(); return; }
    update_carousel_anim();

    float top = hbar + (channel_tabs_active() || home_tabs_active() ? 52*s : 0);
    float cx = W / 2.f, cy = top + (H - top) * 0.44f;
    float cardW = std::min(W * 0.42f, (H - top) * 0.60f * 16.f/9.f);
    float cardH = cardW * 9.f / 16.f;

    const float kTilt   = 1.15f;              // parked side tilt (~66 deg)
    float parkX  = cardW * 0.62f;             // how far a parked side card's centre sits
    float stackX = cardW * 0.28f;             // extra x per additional stacked card
    float hhC = cardH * 0.5f;                 // centre half-height

    for (int i = std::max(0, sel_-8); i < std::min(n, sel_+9); ++i)
        thumbs_.request(results_[i].thumbnail_url);

    auto& vis = vis_; vis.clear();       // reused scratch; compute range, don't scan all n
    { int lo = std::max(0, (int)std::floor(carousel_pos_ - 6.5f));
      int hi = std::min(n-1, (int)std::ceil(carousel_pos_ + 6.5f));
      for (int i = lo; i <= hi; ++i)
          if (std::abs(i - carousel_pos_) <= 6.5f) vis.push_back(i); }
    std::sort(vis.begin(), vis.end(), [&](int a, int b){
        return std::abs(a - carousel_pos_) > std::abs(b - carousel_pos_); });

    for (int i : vis) {
        float d = i - carousel_pos_, ad = std::abs(d);
        float sgn = d < 0 ? -1.f : 1.f;
        float slide = std::min(ad, 1.f);          // 0..1: folding/sliding-out phase
        float extra = std::max(0.f, ad - 1.f);    // beyond first side slot: stacked
        bool center = ad < 0.10f;

        float ang = kTilt * (d < 0 ? -slide : slide);   // 0 at centre -> ±kTilt parked
        float scale = 1.0f - 0.12f*slide - 0.03f*std::min(extra, 4.f);
        float hw = cardW*0.5f*scale, hh = cardH*0.5f*scale;
        float x = cx + sgn * (parkX*slide + stackX*extra);
        float alpha = std::max(0.28f, 1.0f - 0.10f*slide - 0.18f*std::min(extra,4.f));

        if (center) {
            gfx::Rect r{x - hw, cy - hh, hw*2, hh*2};
            if (!tab_focus_) rn.quad({r.x-4*s, r.y-4*s, r.w+8*s, r.h+8*s}, theme_.accent);
            draw_thumb(rn, results_[i], r, s, 1.0f);
            continue;
        }
        float cosA = std::cos(ang), sinA = std::sin(ang);
        float lX = x - hw*cosA, rX = x + hw*cosA;
        float persp = 0.35f;
        // Inner edge nearer -> taller. Right card (d>0, sinA>0): inner = LEFT edge tall.
        float lY = hh * (1.0f - persp*sinA);
        float rY = hh * (1.0f + persp*sinA);
        const auto& v = results_[i];
        gfx::Texture* tex = v.thumbnail_url.empty() ? nullptr : thumbs_.get(v.thumbnail_url);
        if (tex)
            rn.textured_quad4(lX, cy-lY, rX, cy-rY, rX, cy+rY, lX, cy+lY, *tex,
                              gfx::Color{1,1,1,alpha});
        else
            rn.quad4(lX, cy-lY, rX, cy-rY, rX, cy+rY, lX, cy+lY,
                     theme_.card_sel.with_a(alpha));
    }

    float mw = W * 0.7f, mx = (W - mw)/2, my = cy + hhC + 30*s;
    draw_meta(rn, results_[sel_], sel_, mx, my, mw, s, 1.0f);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, my + 82*s, theme_.text_dim);

    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "Left/Right: browse    A: open    L/R: switch tabs    V: view    B: back",
            32*s, H - fh + 12*s, theme_.text_dim);
    draw_status_banner(rn, hbar + 16*s, s);
    rn.end();
}

void App::adjust_volume(int delta) {
    int v = volume_ + delta;
    if (v < 0) v = 0;
    if (v > 150) v = 150;
    volume_ = v;
    player_.set_volume(volume_);
    it_.set_setting_int("volume", volume_);       // remember for future videos
    volume_overlay_until_ = SDL_GetTicks() + 1400;
}

void App::render_player(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    // mpv paints the video frame into the framebuffer first...
    static int fc = 0;
    if (kDbg && (fc++ % 20 == 0))
        std::fprintf(stderr, "[play] frame %d pos=%.1f dur=%.1f\n", fc, player_.position(), player_.duration());
    // Full-screen video (mpv paints the frame first)...
    player_.render(W, H);
    // ...then we overlay UI on top (mpv leaves GL state; begin() resets ours).
    rn.begin(W, H);

    double pos = player_.position(), dur = player_.duration();
    // While a debounced seek is pending, show the TARGET position (scrub preview).
    if (has_pending_seek_) {
        pos += pending_seek_;
        if (pos < 0) pos = 0;
        if (dur > 0 && pos > dur) pos = dur;
    }
    float frac = dur > 0 ? (float)(pos / dur) : 0.f;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    auto tstr = [](double t){ int m=(int)t/60, sec=(int)t%60; char b[16];
        std::snprintf(b,sizeof b,"%d:%02d",m,sec); return std::string(b); };

    // Controls / details overlay: fades out shortly after playback starts, and
    // reappears on pause / start / options-menu / seek (controls_until_ deadline).
    const unsigned kRevealMs = 2600, kFadeMs = 450;
    unsigned now = SDL_GetTicks();
    bool force = menu_open_ || player_.paused();
    if (force) controls_until_ = now + kRevealMs;   // stays up; lingers then fades after resume
    float alpha;
    long remain = (long)controls_until_ - (long)now;
    if (remain <= 0) alpha = 0.f;
    else if (remain >= (long)kFadeMs) alpha = 1.f;
    else alpha = (float)remain / (float)kFadeMs;    // fade tail

    if (alpha > 0.001f) {
        float bar_h = 96 * s;
        // Bottom panel for legibility.
        rn.quad({0, (float)H - bar_h, (float)W, bar_h}, theme_.bg.with_a(0.66f * alpha));
        gfx::Color ctext = theme_.text.with_a(alpha);
        gfx::Color cdim  = theme_.text_dim.with_a(alpha);

        // Title.
        rn.text(*font_body_, font_body_->ellipsize(now_playing_title_, W - 64*s),
                32*s, H - bar_h + 12*s, ctext);

        // Progress bar.
        float px = 32*s, pw = W - 64*s, py = H - 40*s, ph = 6*s;
        rn.quad({px, py, pw, ph}, theme_.card_sel.with_a(alpha));
        rn.quad({px, py, pw * frac, ph}, theme_.accent.with_a(alpha));
        rn.quad({px + pw*frac - 5*s, py - 5*s, 10*s, ph + 10*s}, ctext);

        // Time + control hints.
        std::string time = tstr(pos) + " / " + (dur>0?tstr(dur):"--:--");
        rn.text(*font_small_, time, 32*s, H - 30*s, cdim);
        std::string hint = std::string(player_.paused()?"[A] resume":"[A] pause")
                         + "   [<>] seek 10s   [Start] menu   [B] back";
        rn.text(*font_small_, hint, W - font_small_->text_width(hint) - 32*s,
                H - 30*s, cdim);

        // Centered pause glyph for a clear paused state.
        if (player_.paused()) {
            float cxp = W/2.f, cyp = H*0.46f, bw = 13*s, bh = 46*s, gap = 13*s;
            gfx::Color pc = theme_.text.with_a(0.85f * alpha);
            rn.quad({cxp - gap/2 - bw, cyp - bh/2, bw, bh}, pc);
            rn.quad({cxp + gap/2,      cyp - bh/2, bw, bh}, pc);
        }
    }

    // Stats for nerds — top-left overlay, always visible while enabled.
    if (stats_for_nerds_) {
        std::vector<std::string> lines = player_.stats_lines();
        if (!lines.empty()) {
            float lh = font_small_->line_height() + 6*s;
            float pad = 14*s, pw = 0;
            for (auto& l : lines) pw = std::max(pw, font_small_->text_width(l));
            float bw = pw + pad*2, bh = lines.size()*lh + pad*2;
            rn.quad({16*s, 16*s, bw, bh}, theme_.bg.with_a(0.66f));
            float ly = 16*s + pad;
            for (auto& l : lines) {
                rn.text(*font_small_, l, 16*s + pad, ly, theme_.text);
                ly += lh;
            }
        }
    }

    // Volume change indicator — pops up on Up/Down, then fades out.
    {
        long vremain = (long)volume_overlay_until_ - (long)now;
        if (vremain > 0) {
            float va = vremain >= (long)kFadeMs ? 1.f : (float)vremain / (float)kFadeMs;
            float bw = 360*s, bh = 66*s;
            float bx = (W - bw) / 2, by = H * 0.34f;
            rn.quad({bx, by, bw, bh}, theme_.bg.with_a(0.72f * va));
            gfx::Color vt = theme_.text.with_a(va);
            char lbl[32]; std::snprintf(lbl, sizeof lbl, "Volume  %d%%", volume_);
            rn.text(*font_small_, lbl, bx + 18*s, by + 8*s, vt);
            // Track + fill on a 0..150 scale, with a tick at the 100% reference.
            float tx = bx + 18*s, tw = bw - 36*s, ty = by + bh - 20*s, th = 8*s;
            rn.quad({tx, ty, tw, th}, theme_.card_sel.with_a(va));
            float f = volume_ / 150.f;
            rn.quad({tx, ty, tw * f, th}, theme_.accent.with_a(va));
            float mark = tx + tw * (100.f/150.f);
            rn.quad({mark - 1*s, ty - 4*s, 2*s, th + 8*s}, vt);
        }
    }

    // Transient status banner (e.g. resolve errors surfaced during playback).
    draw_status_banner(rn, 24*s, s);
    rn.end();
}

void App::open_search() {
    mode_ = Mode::Search;
    query_input_ = query_;      // seed with the current query for quick edits
    kb_row_ = 1; kb_col_ = 0;   // start on 'q'
    status_msg_.clear();
}
void App::input_text(const std::string& s) {
    for (char c : s) if (c >= 32 && c <= 126) query_input_ += c;   // ASCII-only atlas
}
void App::backspace() { if (!query_input_.empty()) query_input_.pop_back(); }
void App::kb_activate() {
    const Key& k = KB[kb_row_][kb_col_];
    switch (k.type) {
        case KCHAR:   query_input_ += k.ch; break;
        case KSPACE:  query_input_ += ' '; break;
        case KDEL:    backspace(); break;
        case KCLEAR:  query_input_.clear(); break;
        case KSUBMIT: submit_search(); break;
    }
}
void App::submit_search() {
    std::string q = query_input_;
    // trim trailing spaces
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) { mode_ = Mode::Grid; return; }
    mode_ = Mode::Grid;
    search(q);                 // synchronous (~1s); reuses the startup path
}

void App::draw_spinner(gfx::Renderer& rn, float cx, float cy, float r, gfx::Color c) {
    // 12 dots around a circle; a highlight chases around driven by wall time.
    const int N = 12;
    float phase = (SDL_GetTicks() / 80) % N;   // advance one dot every 80ms
    float dot = r * 0.16f;
    for (int i = 0; i < N; ++i) {
        float ang = (float)i / N * 6.2831853f - 1.5708f;
        float x = cx + std::cos(ang) * r, y = cy + std::sin(ang) * r;
        int d = (i - (int)phase + N) % N;        // 0 = head, brightest
        float a = 0.15f + 0.85f * (float)(N - d) / N;
        rn.quad({x - dot, y - dot, dot*2, dot*2}, c.with_a(a));
    }
}

void App::render_loading(gfx::Renderer& rn) {
    // Drawn ON TOP of the already-rendered (flushed) grid — begin() sets state
    // but does not clear, so the dimmed grid shows through.
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.72f));
    draw_spinner(rn, W/2.f, H/2.f - 30*s, 34*s, theme_.accent);
    std::string msg = "Loading";
    rn.text(*font_body_, msg, W/2.f - font_body_->text_width(msg)/2, H/2.f + 26*s, theme_.text);
    std::string t = font_small_->ellipsize(loading_title_, W * 0.6f);
    rn.text(*font_small_, t, W/2.f - font_small_->text_width(t)/2, H/2.f + 56*s, theme_.text_dim);
    rn.end();
}

void App::render_search(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);

    // Header.
    float hbar = 84 * s;
    rn.quad({0, 0, (float)W, hbar}, theme_.panel);
    rn.quad({0, hbar-3*s, (float)W, 3*s}, theme_.accent);
    rn.text(*font_title_, "Search", 32*s, 24*s, theme_.text);

    // Query input box + blinking caret.
    float bx = 32*s, by = hbar + 28*s, bw = W - 64*s, bh = 54*s;
    rn.quad({bx, by, bw, bh}, theme_.card);
    rn.quad({bx, by, 4*s, bh}, theme_.accent);
    // Show a right-anchored (tail) view when the query overflows, so the caret and the
    // most-recently-typed text stay visible instead of being ellipsized away.
    float avail = bw - 40*s;
    std::string shown = query_input_;
    while (font_body_->text_width(shown) > avail && !shown.empty()) {
        size_t adv = 1; uint8_t c = (uint8_t)shown[0];   // drop one leading codepoint
        if ((c>>5)==0x6) adv=2; else if ((c>>4)==0xE) adv=3; else if ((c>>3)==0x1E) adv=4;
        shown.erase(0, adv);
    }
    rn.text(*font_body_, shown, bx + 18*s, by + 14*s, theme_.text);
    float caret_x = bx + 18*s + font_body_->text_width(shown);
    if ((SDL_GetTicks() / 500) % 2 == 0)
        rn.quad({caret_x + 2*s, by + 12*s, 2*s, bh - 24*s}, theme_.text);
    if (query_input_.empty())
        rn.text(*font_body_, "type to search...", bx + 18*s, by + 14*s, theme_.text_dim);

    // Keyboard grid.
    float gx = 32*s, gy = by + bh + 34*s;
    float unit = 74*s;              // width of a span-1 key
    float keyh = 66*s, gap = 10*s;
    for (int r = 0; r < (int)KB.size(); ++r) {
        float x = gx;
        for (int c = 0; c < (int)KB[r].size(); ++c) {
            const Key& k = KB[r][c];
            float kw = unit * k.span + gap * (k.span - 1);
            bool sel = (r == kb_row_ && c == kb_col_);
            gfx::Rect kr{x, gy + r*(keyh+gap), kw, keyh};
            if (sel) rn.quad({kr.x-3*s, kr.y-3*s, kr.w+6*s, kr.h+6*s}, theme_.accent);
            rn.quad(kr, sel ? theme_.card_sel : theme_.card);
            const gfx::Font& f = (k.type == KCHAR || k.type == KSPACE) ? *font_body_ : *font_small_;
            float tw = f.text_width(k.label);
            gfx::Color tc = (k.type == KSUBMIT) ? theme_.accent : theme_.text;
            rn.text(f, k.label, kr.x + (kw - tw)/2, kr.y + (keyh - f.line_height())/2 + 4*s, tc);
            x += kw + gap;
        }
    }

    // Footer hints.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "D-Pad: move    A: type    Y: submit    B: cancel    (or just type)",
            32*s, H - fh + 12*s, theme_.text_dim);
    rn.end();
}

void App::request_playback() {
    const yt::SearchResult* v = selected();
    if (!v || v->video_id.empty()) return;   // only videos are playable
    now_playing_item_ = *v;                      // context for the player options menu
    now_playing_index_ = sel_;                   // remember list position (for autoplay)
    stats_for_nerds_ = false;                    // per-video: reset for each new video
    playback_speed_ = 1.0;                       // per-video: speed back to normal
    // (replay_current, used for quality changes, does NOT reset these -> speed persists
    //  across a re-resolve of the same video.)
    // Ask-to-resume: if a position was saved for this video, prompt before playing.
    if (ask_resume_) {
        double rp = it_.resume_pos(v->video_id);
        if (rp > 15) {                           // ignore trivially-early stops
            resume_prompt_open_ = true;
            resume_prompt_sel_ = 0;              // default: Resume
            resume_prompt_pos_ = rp;
            resume_prompt_item_ = *v;
            return;
        }
    }
    start_resolve(v->video_id, v->title, 0);
}

void App::test_end_playback(int idx) {
    if (idx >= 0 && idx < (int)results_.size()) {
        now_playing_item_ = results_[idx]; now_playing_index_ = idx;
    }
    handle_playback_ended();
}

// Launch a specific video (autoplay path) without the resume prompt.
void App::play_item(const yt::SearchResult& v, int index) {
    now_playing_item_ = v;
    now_playing_index_ = index;
    stats_for_nerds_ = false;
    playback_speed_ = 1.0;
    start_resolve(v.video_id, v.title, 0);
}

// Called when playback ends naturally (EOF). If autoplay is on, pick the next video
// (next in the current list, else a related video) and ARM the staged up-next sequence
// instead of playing immediately; otherwise return to the grid.
void App::handle_playback_ended() {
    if (!now_playing_item_.video_id.empty())
        it_.clear_resume_pos(now_playing_item_.video_id);   // finished -> forget resume
    player_.stop();
    has_pending_seek_ = false; pending_seek_ = 0;
    if (!autoplay_) { mode_ = Mode::Grid; return; }
    int idx = find_next_playable(now_playing_index_);
    if (idx >= 0) { arm_upnext(results_[idx], idx); return; }  // next in the current list
    start_related_autoplay(now_playing_item_.video_id);        // end of list -> related
}

// Index of the next playable video row after from_index in results_, or -1.
int App::find_next_playable(int from_index) const {
    int start = from_index >= 0 ? from_index + 1 : (int)results_.size();
    for (int i = start; i < (int)results_.size(); ++i) {
        const yt::SearchResult& v = results_[i];
        if (!v.video_id.empty() && !v.is_channel() && !v.is_playlist() && !v.is_post())
            return i;
    }
    return -1;
}

// Stage the up-next sequence: land on the grid with the next item selected; step_autoplay()
// then waits for any current popup to clear, shows "Up next: <title>", and starts it.
void App::arm_upnext(const yt::SearchResult& v, int index) {
    auto_next_item_ = v;
    auto_next_index_ = index;
    if (index >= 0 && index < (int)results_.size()) { sel_ = index; ensure_visible(); }
    mode_ = Mode::Grid;
    auto_state_ = AutoState::WaitClear;
}

// Runs each frame (pump_async): advance the staged autoplay.
void App::step_autoplay() {
    if (auto_state_ == AutoState::None || mode_ != Mode::Grid) return;
    unsigned now = SDL_GetTicks();
    if (auto_state_ == AutoState::WaitClear) {
        if (!status_msg_.empty() && now < status_until_) return;   // let the last popup finish
        status_msg_ = "Up next: " + auto_next_item_.title;
        status_until_ = now + 2600;
        auto_show_until_ = now + 2600;
        auto_state_ = AutoState::ShowUpNext;
    } else if (auto_state_ == AutoState::ShowUpNext) {
        if (now < auto_show_until_) return;                        // let "Up next" show
        auto_state_ = AutoState::None;
        play_item(auto_next_item_, auto_next_index_);              // now start the next video
    }
}
void App::cancel_autoplay() {
    if (auto_state_ != AutoState::None) { auto_state_ = AutoState::None; status_msg_.clear(); }
}

// End of list: fetch related videos off-thread, then arm the up-next sequence.
void App::start_related_autoplay(const std::string& video_id) {
    if (video_id.empty()) { mode_ = Mode::Grid; return; }
    if (rel_running_) { mode_ = Mode::Grid; return; }   // a related fetch is already running
    mode_ = Mode::Loading;
    status_msg_ = "Finding next video..."; status_until_ = SDL_GetTicks() + 6000;
    rel_autoplay_pending_ = true;
    if (rel_thread_.joinable()) rel_thread_.join();   // finished -> instant
    rel_running_ = true; rel_done_ = false;
    rel_thread_ = std::thread([this, video_id]() {
        std::vector<yt::SearchResult> r;
        try { r = it_.related_videos(video_id); } catch (...) {}
        { std::lock_guard<std::mutex> lk(rel_m_); rel_pending_ = std::move(r); }
        rel_running_ = false; rel_done_ = true;
    });
}
void App::poll_related_autoplay() {
    if (!rel_done_.exchange(false)) return;
    std::vector<yt::SearchResult> r;
    { std::lock_guard<std::mutex> lk(rel_m_); r = std::move(rel_pending_); rel_pending_.clear(); }
    // If the user navigated away (Back left Loading), or autoplay was cancelled, drop it.
    if (!rel_autoplay_pending_ || mode_ != Mode::Loading) { rel_autoplay_pending_ = false; return; }
    rel_autoplay_pending_ = false;
    if (r.empty()) { mode_ = Mode::Grid; status_msg_ = "No more videos";
                     status_until_ = SDL_GetTicks() + 2500; return; }
    // Replace the list with the related set so subsequent autoplay chains through it,
    // then arm the same staged up-next sequence.
    results_ = std::move(r);
    arm_upnext(results_[0], 0);
}

// Save (or clear) the current playback position for ask-to-resume. Called when
// leaving playback; near the start or the end we forget it instead.
void App::save_resume_position() {
    if (now_playing_item_.video_id.empty()) return;
    double pos = player_.position(), dur = player_.duration();
    if (pos > 15 && (dur <= 0 || pos < dur - 15))
        it_.set_resume_pos(now_playing_item_.video_id, pos);
    else
        it_.clear_resume_pos(now_playing_item_.video_id);
}

// Re-resolve the currently-playing video (e.g. after a quality change) and resume
// at at_seconds once the new stream loads.
void App::replay_current(double at_seconds) {
    if (now_playing_item_.video_id.empty()) return;
    // A restricted (paced) stream's FRESH url only serves ~the first 20MiB, so a deep
    // resume would 403 on load. Clamp into the fresh window (approx via bitrate).
    if (playing_paced_ && playing_vbitrate_ > 0) {
        double safe = (15.0 * 1024 * 1024 * 8) / (double)playing_vbitrate_;
        if (at_seconds > safe) {
            at_seconds = safe;
            status_msg_ = "Paced stream: resuming near the start";
            status_until_ = SDL_GetTicks() + 5000;
        }
    }
    start_resolve(now_playing_item_.video_id, now_playing_item_.title, at_seconds);
}

void App::start_resolve(const std::string& video_id, const std::string& title, double start_pos) {
    if (!Player::available()) {
        status_msg_ = "playback unavailable (no libmpv in this build)";
        return;
    }
    if (resolve_running_) return;               // one resolve at a time
    status_msg_.clear();
    loading_title_ = title;
    resume_pos_ = start_pos;                     // applied in poll_resolve after play()
    mode_ = Mode::Loading;

    std::string fallback_title = title;
    yt::VideoPrefs prefs = play_prefs_;
    resolve_running_ = true;
    resolve_done_ = false;
    if (resolve_thread_.joinable()) resolve_thread_.join();
    resolve_thread_ = std::thread([this, video_id, fallback_title, prefs]() {
        bool dbg = getenv("YTC_DEBUG");
        ResolveResult r;
        r.title = fallback_title;
        try {
        if (dbg) std::fprintf(stderr, "[play] resolving %s (cap=%d)...\n",
                              video_id.c_str(), prefs.max_height);
        yt::VideoInfo info = it_.resolve(video_id);   // background thread owns it_ here
        r.title = info.title.empty() ? fallback_title : info.title;
        r.description = info.description;
        if (!info.ok()) {
            r.ok = false;
            // Surface a human-readable reason (YouTube's own text is best).
            std::string reason = info.status_reason;
            if (info.is_upcoming)
                r.status = reason.empty() ? "Premiere hasn't started yet" : reason;
            else if (info.status == "LIVE_STREAM_OFFLINE")
                r.status = reason.empty() ? "Live stream is offline" : reason;
            else
                r.status = reason.empty() ? ("Can't play (" + info.status + ")") : reason;
            if (dbg) std::fprintf(stderr, "[play] not playable: %s / %s\n",
                                  info.status.c_str(), r.status.c_str());
        } else if (info.is_live && info.hls_manifest_url) {
            // Live: play the HLS manifest (mpv handles it live; audio is muxed).
            r.ok = true;
            r.video_url = *info.hls_manifest_url;
            r.audio_url = "";
            r.user_agent = info.user_agent;
            if (dbg) std::fprintf(stderr, "[play] resolved LIVE via HLS manifest\n");
        } else {
            const yt::Format* vf = info.best_video(prefs);
            const yt::Format* af = info.best_audio();
            if (!vf) { r.ok = false; r.status = "No playable video format"; }
            else {
                r.ok = true;
                r.video_url = vf->url;
                r.audio_url = af ? af->url : "";
                r.user_agent = info.user_agent;
                r.video_bitrate = vf->bitrate;
                // Detect restricted (paced) delivery: some videos (e.g. kids content)
                // 403 any range beyond a sliding window. The made-for-kids marker in
                // playabilityStatus tells us for FREE; for unmarked videos a deep
                // range probe is the fallback (skip small files — they fit anyway).
                if (info.made_for_kids) {
                    r.paced = true;
                } else if (vf->content_length > (24LL << 20)) {
                    try {
                        HttpClient probe;
                        char rng[64];
                        long long off = vf->content_length * 9 / 10;
                        std::snprintf(rng, sizeof rng, "Range: bytes=%lld-%lld", off, off + 1023);
                        auto pr = probe.get(vf->url, {"User-Agent: " + info.user_agent,
                                                      std::string(rng)});
                        r.paced = (pr.status == 403);
                    } catch (...) { r.paced = false; }
                    if (dbg && r.paced)
                        std::fprintf(stderr, "[play] RESTRICTED delivery detected (deep range 403)\n");
                }
                if (dbg) std::fprintf(stderr, "[play] resolved: video itag %d audio itag %d paced=%d\n",
                                      vf->itag, af ? af->itag : -1, (int)r.paced);
            }
        }
        } catch (const std::exception& e) {   // never let the resolve thread terminate the app
            r.ok = false; r.status = "Error resolving (see log)";
            std::fprintf(stderr, "[play] resolve thread exception: %s\n", e.what());
        }
        { std::lock_guard<std::mutex> lk(resolve_m_); resolve_result_ = std::move(r); }
        resolve_running_ = false;
        resolve_done_ = true;
    });
}

void App::poll_resolve() {
    if (!resolve_done_.exchange(false)) return;
    if (resolve_thread_.joinable()) resolve_thread_.join();
    ResolveResult r;
    { std::lock_guard<std::mutex> lk(resolve_m_); r = std::move(resolve_result_); }

    if (mode_ != Mode::Loading) return;         // user cancelled (Back) — discard
    if (!r.ok) {
        mode_ = Mode::Grid;
        status_msg_ = r.status;
        status_until_ = SDL_GetTicks() + 6000;  // show the banner for 6s
        return;
    }
    double start_at = resume_pos_; resume_pos_ = 0;   // consume the resume point
    if (!player_.play(r.video_url, r.audio_url, r.user_agent, start_at, r.paced)) {
        mode_ = Mode::Grid; status_msg_ = "player failed to start"; return;
    }
    player_.set_volume(volume_);                 // apply the app-local volume level
    player_.set_speed(playback_speed_);          // apply speed (persists across re-resolve)
    start_sponsorblock(now_playing_item_.video_id);   // fetch skip segments off-thread
    start_captions(now_playing_item_.video_id);        // fetch caption track list off-thread
    now_playing_title_ = r.title;
    now_playing_desc_ = r.description;          // free: came with the resolve
    playing_paced_ = r.paced;
    playing_vbitrate_ = r.video_bitrate;
    played_max_ = start_at;                     // seek window anchors here
    if (playing_paced_) {
        status_msg_ = "Limited seeking on this video (paced stream)";
        status_until_ = SDL_GetTicks() + 5000;
        // Remember the channel verdict (restriction is channel-wide in practice);
        // the "Hide Restricted" filter picks it up like any checked verdict.
        rcheck_.put(now_playing_item_.channel_id, true);
    }
    mode_ = Mode::Playing;
    controls_until_ = SDL_GetTicks() + 2600;   // show info briefly, then fade as it plays
    // Record in local watch history (most-recent first, deduped). Title prefers the
    // resolved title; fall back to the tile's title if resolve returned nothing.
    if (!now_playing_item_.video_id.empty())
        it_.add_history(now_playing_item_.video_id,
                        r.title.empty() ? now_playing_item_.title : r.title,
                        now_playing_item_.channel_id, now_playing_item_.author);
}

void App::ensure_visible() {
    const int H = win_->height();
    GridMetrics m = grid_metrics();       // same metrics render uses -> no clipping
    int row = sel_ / cols_;
    float y = m.top + row * m.rowstep - scroll_;   // selected row's on-screen top
    // Scroll up so the row clears the top (row 0 snaps to 0 -> header shows);
    // scroll down so the whole row + its selection ring clears the pinned footer,
    // with a pad gap so the card bottom isn't tucked under the footer.
    float bottom_limit = H - m.fh - m.pad;
    if (y < m.top) scroll_ -= (m.top - y);
    if (y + m.cardh > bottom_limit) scroll_ += (y + m.cardh) - bottom_limit;
    if (scroll_ < 0) scroll_ = 0;
}

} // namespace ui
