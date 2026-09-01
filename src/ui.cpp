#include "ui.h"
#include "platform.h"   // Windows timegm shim
#include "i18n.h"
#include "remux.h"
#include "hwdetect.h"
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
enum KType { KCHAR, KSPACE, KDEL, KSUBMIT, KSHIFT, KLEFT, KRIGHT };
struct Key { const char* label; KType type; char ch; int span; };
// Android-TV-style QWERTY, but keeping a dedicated number row on top. `span` widens a
// key (space/search) for layout + nav. Each row totals 10 units.
const std::vector<std::vector<Key>> KB = {
    {{"1",KCHAR,'1',1},{"2",KCHAR,'2',1},{"3",KCHAR,'3',1},{"4",KCHAR,'4',1},{"5",KCHAR,'5',1},
     {"6",KCHAR,'6',1},{"7",KCHAR,'7',1},{"8",KCHAR,'8',1},{"9",KCHAR,'9',1},{"0",KCHAR,'0',1}},
    {{"q",KCHAR,'q',1},{"w",KCHAR,'w',1},{"e",KCHAR,'e',1},{"r",KCHAR,'r',1},{"t",KCHAR,'t',1},
     {"y",KCHAR,'y',1},{"u",KCHAR,'u',1},{"i",KCHAR,'i',1},{"o",KCHAR,'o',1},{"p",KCHAR,'p',1}},
    {{"a",KCHAR,'a',1},{"s",KCHAR,'s',1},{"d",KCHAR,'d',1},{"f",KCHAR,'f',1},{"g",KCHAR,'g',1},
     {"h",KCHAR,'h',1},{"j",KCHAR,'j',1},{"k",KCHAR,'k',1},{"l",KCHAR,'l',1},{".",KCHAR,'.',1}},
    {{"\xE2\x87\xA7",KSHIFT,0,1},                                             // U+21E7 shift
     {"z",KCHAR,'z',1},{"x",KCHAR,'x',1},{"c",KCHAR,'c',1},{"v",KCHAR,'v',1},{"b",KCHAR,'b',1},
     {"n",KCHAR,'n',1},{"m",KCHAR,'m',1},{",",KCHAR,',',1},
     {"\xE2\x8C\xAB",KDEL,0,1}},                                             // U+232B backspace
    {{"\xE2\x97\x80",KLEFT,0,1},{"\xE2\x96\xB6",KRIGHT,0,1},                  // U+25C0 / U+25B6
     {"space",KSPACE,' ',4},{"-",KCHAR,'-',1},{"_",KCHAR,'_',1},
     {"Search",KSUBMIT,0,2}},
};
// Dedicated numeric keypad for device linking (TV codes are digits only). A squared
// 4-column dialpad; each row totals 4 units, keys are drawn square.
const std::vector<std::vector<Key>> KB_NUM = {
    {{"1",KCHAR,'1',1},{"2",KCHAR,'2',1},{"3",KCHAR,'3',1},{"4",KCHAR,'4',1}},
    {{"5",KCHAR,'5',1},{"6",KCHAR,'6',1},{"7",KCHAR,'7',1},{"8",KCHAR,'8',1}},
    {{"9",KCHAR,'9',1},{"0",KCHAR,'0',1},
     {"\xE2\x8C\xAB",KDEL,0,1},{"\xE2\x9C\x93",KSUBMIT,0,1}},                    // 9 0 ⌫ ✓
    {{"space",KSPACE,' ',4}},                                                   // full-width space bar
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
        Pending p{url, {}, false};
        if (url.find("://") == std::string::npos) {   // local file (offline download thumb)
            std::ifstream f(url, std::ios::binary);
            if (f) { p.bytes.assign(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>()); p.ok = !p.bytes.empty(); }
        } else {
            auto r = http.get(url);
            if (getenv("YTC_DEBUG"))
                std::fprintf(stderr, "[thumb] GET %.50s -> %ld %zub\n", url.c_str(), r.status, r.body.size());
            p.ok = r.ok();
            if (r.ok()) p.bytes.assign(r.body.begin(), r.body.end());
        }
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
std::string ChannelMetaCache::name(const std::string& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = name_.find(id);
    return it == name_.end() ? std::string() : it->second;
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
        name_[id]   = info.name;                        // "" if not found
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
        std::string base = platform::exe_dir();
        std::string cands[] = {
            env ? std::string(env) : std::string(),
            base + "/data/DejaVuSans.ttf",     // bundled next to the executable (desktop/port)
            "data/DejaVuSans.ttf",             // or relative to the working directory
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",  // Debian/Ubuntu system
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",           // Fedora/Arch system
            "/Library/Fonts/Arial Unicode.ttf",                 // macOS fallback
            "C:/Windows/Fonts/segoeui.ttf",                     // Windows fallback
        };
        for (const auto& c : cands) {
            if (c.empty()) continue;
            if (std::ifstream(c).good()) return c;
        }
        return base + "/data/DejaVuSans.ttf";
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
    m.tabs_h = (channel_tabs_active() || home_tabs_active() || search_tabs_active()) ? 52 * m.s : 0;
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

// Directory containing the config file (where cast.json etc. live).
static std::string dir_of(const std::string& path) {
    auto s = path.find_last_of('/');
    return s == std::string::npos ? std::string(".") : path.substr(0, s);
}
App::App(const std::string& config_path, gfx::Window* win)
    : win_(win), it_(config_path), cast_(dir_of(config_path)) {
    load_fonts();
    cols_ = compute_columns();
    view_mode_ = (ViewMode)it_.setting_int("view", 0);   // 0 grid / 1 carousel / 2 coverflow
    if (getenv("YTC_CAROUSEL")) view_mode_ = ViewMode::Carousel;
    if (const char* vm = getenv("YTC_VIEW")) view_mode_ = (ViewMode)atoi(vm);
    refresh_favorites();
    refresh_favorite_playlists();
    refresh_watch_later();
    refresh_downloads();
    hide_restricted_ = it_.setting_int("hide_restricted", 0) != 0;
    ask_resume_ = it_.setting_int("ask_resume", 1) != 0;   // default ON
    volume_ = it_.setting_int("volume", 100);              // app-local playback volume %
    if (volume_ < 0) volume_ = 0; if (volume_ > 150) volume_ = 150;
    // Hardware decode is real only when the device has a v4l2 decoder AND the
    // loaded libavcodec provides v4l2m2m (the optional hwdec bundle from
    // ytc_setup). Then the toggle appears in Settings > Video and "Hardware"
    // maps to v4l2m2m-copy; everywhere else the old auto-copy-safe default
    // stands (harmless: mpv falls back to software when nothing matches).
    hwdec_capable_ = ytn::hwdec_v4l2_available() && hwdetect::detect().has_decoder();
    hwdec_mode_ = it_.setting_int("hwdec", 0) ? 1 : 0;     // 0 hardware / 1 software
    player_.set_hwdec(hwdec_mode_str());
    aspect_mode_ = it_.setting_int("aspect", 0);           // 0 fit / 1 zoom / 2 stretch
    if (aspect_mode_ < 0 || aspect_mode_ > 2) aspect_mode_ = 0;
    player_.set_aspect(aspect_mode_);
    audio_lang_pref_ = it_.setting_str("audio_lang", "app");  // dub language default
    cc_lang_pref_ = it_.setting_str("cc_lang", "app");        // default caption language
    if (cc_lang_pref_ == "off") cc_lang_pref_ = "app";        // migrate the old value
    cc_size_ = it_.setting_int("cc_size", 1);                 // caption size (medium)
    if (cc_size_ < 0 || cc_size_ > 2) cc_size_ = 1;
    cc_style_ = it_.setting_int("cc_style", 0);               // caption style (white)
    if (cc_style_ < 0 || cc_style_ > 5) cc_style_ = 0;
    player_.set_caption_style(cc_size_, cc_style_);
    sponsorblock_ = it_.setting_int("sponsorblock", 0) != 0;   // default OFF
    autoplay_ = it_.setting_int("autoplay", 0) != 0;          // default OFF
    // Home sources bitmask (1 fav / 2 history / 4 custom). Default: Favorites
    // only — History and Custom are opt-in via Browsing > Home Feed.
    home_sources_ = it_.setting_int("home_sources", 1) & 7;
    if (!home_sources_) home_sources_ = 1;

    // UI language + Innertube locale (0 = English). Drives translated labels and
    // YouTube-localized titles/metadata. Set before any feed fetch is kicked off.
    lang_ = it_.setting_int("lang", 0);
    i18n::set_language(lang_);
    it_.set_locale(i18n::language_hl(lang_), i18n::language_gl(lang_));

    // Default quality cap: 1080p (not 4K). Persisted in settings.json (Settings menu);
    // YTC_MAXHEIGHT overrides for testing (0 = uncapped).
    play_prefs_.max_height = it_.setting_int("max_height", 1080);
    if (const char* mh = getenv("YTC_MAXHEIGHT")) play_prefs_.max_height = atoi(mh);
}

App::~App() {
    cast_events_run_ = false;
    if (cast_events_thread_.joinable()) cast_events_thread_.join();
    if (resolve_thread_.joinable()) resolve_thread_.join();
    if (chinfo_thread_.joinable()) chinfo_thread_.join();
    if (more_thread_.joinable()) more_thread_.join();
    if (home_more_thread_.joinable()) home_more_thread_.join();
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    if (cast_cmd_thread_.joinable()) cast_cmd_thread_.join();
    if (cast_link_thread_.joinable()) cast_link_thread_.join();
    if (refresh_thread_.joinable()) refresh_thread_.join();
    if (desc_thread_.joinable()) desc_thread_.join();
    if (comments_thread_.joinable()) comments_thread_.join();
    if (comments_reply_thread_.joinable()) comments_reply_thread_.join();
    dl_cancel_ = true;
    if (dl_thread_.joinable()) dl_thread_.join();
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
    for (const auto& v : results_) {
        if (v.is_channel()) chan_meta_.request(v.channel_id);   // async video count
        // Search shorts carry no uploader name; resolve it from their channel id.
        else if (v.is_short && v.author.empty() && !v.channel_id.empty())
            chan_meta_.request(v.channel_id);
    }
    queue_restricted_checks();   // judge unknown channels in the background
}
void App::search(const std::string& query) {
    query_ = query;
    view_label_.clear();
    in_channel_view_ = false;
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    filt_type_ = filt_duration_ = filt_date_ = filt_sort_ = 0;   // fresh search resets filters
    run_search();
}

// (Re-)issue the current query_ with the active server-side filters. Called by a
// fresh search() and again whenever the Search Filters modal changes something.
void App::run_search() {
    static const int kTypePb[] = {0, 1, 2, 3};        // Off/Videos/Channels/Playlists
    static const int kDurPb[]  = {0, 4, 5, 2};        // Off/Under3/3-20/Over20
    static const int kDatePb[] = {0, 2, 3, 4, 5};     // Off/Today/Week/Month/Year
    static const int kSortPb[] = {0, 0, 3};           // Off/Relevance/Popularity (view count)
    std::string params = yt::build_search_params(kTypePb[filt_type_], kDurPb[filt_duration_],
                                                 kDatePb[filt_date_], kSortPb[filt_sort_]);
    auto f = it_.search_feed(query_, params);   // never throws (guarded in Innertube)
    cont_token_ = f.continuation; cont_endpoint_ = f.endpoint; cont_channel_id_ = f.channel_id;
    set_results(std::move(f.items));
    search_tab_ = 0; search_sort_ = 0;  // client-side tab/sort reset to All / Relevance
    search_base_ = results_;            // canonical relevance order of all types (tab/sort source)
    if (results_.empty()) {
        status_msg_ = i18n::tr(i18n::Str::NoResultsConn);
        status_until_ = SDL_GetTicks() + 8000;
    }
}

// The "Search Filters" modal: four cyclable rows, each defaulting to Off. `snapshot`
// records the entry-state for change detection; false on in-place rebuilds (cycling),
// so it isn't overwritten between opening the modal and closing it.
void App::open_search_filters(bool snapshot) {
    using i18n::tr; using S = i18n::Str;
    auto row = [&](S label, const S* vals, int n, int cur, MenuAction act) {
        (void)n;
        menu_items_.push_back({tr(label) + std::string(":  ") + tr(vals[cur]), act});
    };
    static const S kType[] = {S::Off, S::TabVideos, S::FilterChannels, S::TabPlaylists};
    static const S kDur[]  = {S::Off, S::DurShort, S::DurMedium, S::DurLong};
    static const S kDate[] = {S::Off, S::DateToday, S::DateWeek, S::DateMonth, S::DateYear};
    static const S kSort[] = {S::Off, S::FilterRelevance, S::FilterPopularity};
    menu_kind_ = MenuKind::SearchFilters;
    menu_items_.clear();
    row(S::FilterType,       kType, 4, filt_type_,     MenuAction::CycleFilterType);
    row(S::FilterDuration,   kDur,  4, filt_duration_, MenuAction::CycleFilterDuration);
    row(S::FilterUploadDate, kDate, 5, filt_date_,     MenuAction::CycleFilterDate);
    row(S::FilterPrioritize, kSort, 3, filt_sort_,     MenuAction::CycleFilterSort);
    menu_sel_ = 0;
    if (snapshot) {
        filt_snapshot_[0] = filt_type_; filt_snapshot_[1] = filt_duration_;
        filt_snapshot_[2] = filt_date_; filt_snapshot_[3] = filt_sort_;
    }
    menu_open_ = true;
}

// Parse a view-count string ("1,605,755 views" or "1.6M views") into a number for
// the Popular sort. Empty/unknown (playlists, channels) sort last (returns -1).
static long long parse_view_count(const std::string& s) {
    if (s.empty()) return -1;
    double n = 0; size_t i = 0; bool any = false;
    while (i < s.size() && !isdigit((unsigned char)s[i])) ++i;
    for (; i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == ',' || s[i] == '.'); ++i) {
        if (s[i] == ',') continue;
        if (s[i] == '.') { double frac = 0, scale = 0.1;
            for (++i; i < s.size() && isdigit((unsigned char)s[i]); ++i) { frac += (s[i]-'0')*scale; scale *= 0.1; }
            n += frac; break; }
        n = n * 10 + (s[i]-'0'); any = true;
    }
    if (!any) return -1;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size()) { char u = toupper((unsigned char)s[i]);
        if (u == 'K') n *= 1e3; else if (u == 'M') n *= 1e6; else if (u == 'B') n *= 1e9; }
    return (long long)(n + 0.5);
}

// Cycle the search-results sort (Relevance -> Newest -> Popular) and pop a toast.
void App::toggle_search_sort() {
    if (query_.empty() || search_base_.empty()) return;
    search_sort_ = (search_sort_ + 1) % 3;
    apply_search_view(/*keep_selection=*/false);
    i18n::Str toast = search_sort_ == 1 ? i18n::Str::SortedByDate
                    : search_sort_ == 2 ? i18n::Str::SortedByViews
                                        : i18n::Str::SortedByRelevance;
    status_msg_ = i18n::tr(toast);
    status_until_ = SDL_GetTicks() + 2500;
}

// Switch the active search tab (All/Videos/Shorts/Playlists) and rebuild the view.
void App::load_search_tab(int tab) {
    if (tab < 0 || tab > 4 || tab == search_tab_) return;
    search_tab_ = tab;
    apply_search_view(/*keep_selection=*/false);
}

// Rebuild results_ from the canonical list: filter by the active tab, then sort.
// keep_selection preserves the highlighted item by id (used after pagination).
void App::apply_search_view(bool keep_selection) {
    std::string cur_id;
    if (keep_selection && sel_ >= 0 && sel_ < (int)results_.size())
        cur_id = results_[sel_].video_id;
    results_.clear();
    for (const auto& r : search_base_) {
        switch (search_tab_) {
            case 1: if (r.is_short || !(r.kind == yt::SearchResult::Kind::Video)) continue; break;  // Videos
            case 2: if (!r.is_short) continue; break;                                                // Shorts
            case 3: if (!r.is_channel()) continue; break;                                            // Channels
            case 4: if (!r.is_playlist()) continue; break;                                           // Playlists
            default: break;                                                                          // All
        }
        results_.push_back(r);
    }
    if (search_sort_ == 1)
        std::stable_sort(results_.begin(), results_.end(),
            [](const yt::SearchResult& a, const yt::SearchResult& b) {
                return yt::approx_age_secs(a.published_text) <
                       yt::approx_age_secs(b.published_text);   // newest (smallest age) first
            });
    else if (search_sort_ == 2)
        std::stable_sort(results_.begin(), results_.end(),
            [](const yt::SearchResult& a, const yt::SearchResult& b) {
                return parse_view_count(a.view_count_text) >
                       parse_view_count(b.view_count_text);     // most views first
            });
    build_tile_lines();
    sel_ = 0;
    if (keep_selection && !cur_id.empty())
        for (int i = 0; i < (int)results_.size(); ++i)
            if (results_[i].video_id == cur_id) { sel_ = i; break; }
    if (sel_ == 0) { scroll_ = 0; carousel_pos_ = 0; }
    ensure_visible();
}
void App::load_home() {
    query_.clear();                       // empty query + empty view_label_ => "Home"
    view_label_.clear();
    in_channel_view_ = false;
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    cont_token_.clear();                  // merged home feed isn't paginated
    home_tab_ = 0;
    home_playlists_.clear(); home_playlists_loaded_ = false;
    home_posts_.clear(); home_posts_loaded_ = false;
    home_items_.clear();
    home_cursor_ = {}; home_pl_cursor_ = {}; home_posts_cursor_ = {};   // drop prior continuation
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

void App::load_downloads() {
    query_.clear(); view_label_ = "Downloads";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    set_results(it_.downloads());   // tiles built from downloads/*.info (local thumbs)
}

void App::load_history() {
    query_.clear(); view_label_ = "History";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& [id, title, channel] : it_.history()) {
        yt::SearchResult r;
        r.video_id = id; r.title = title; r.author = channel;
        r.thumbnail_url = thumb_for(id);
        items.push_back(std::move(r));
    }
    set_results(std::move(items));
}
void App::refresh_favorite_playlists() {
    fav_pl_ids_.clear();
    for (auto& p : it_.favorite_playlists()) fav_pl_ids_.insert(p.id);
}

// The Favorite Playlists view: one tile per saved playlist, everything needed
// for the tile stored at favoriting time (thumbnails aren't derivable from ids).
void App::load_favorite_playlists() {
    query_.clear(); view_label_ = "Favorite Playlists";
    in_channel_view_ = false; cont_token_.clear();
    subview_playlist_.clear(); tab_focus_ = false; view_stack_.clear();
    std::vector<yt::SearchResult> items;
    for (auto& p : it_.favorite_playlists()) {
        yt::SearchResult r;
        r.kind = yt::SearchResult::Kind::Playlist;
        r.playlist_id = p.id;
        r.title = p.title;
        r.author = p.author;
        r.thumbnail_url = p.thumb;
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
void App::refresh_downloads() {
    dl_ids_.clear();
    for (auto& id : it_.download_ids()) dl_ids_.insert(id);
}

// Download the highlighted/playing video as a progressive .mp4 (offline), matching the
// current playback quality where a progressive stream allows.
void App::start_download(const yt::SearchResult& t) {
    if (t.video_id.empty()) return;
    if (dl_running_) {
        status_msg_ = i18n::tr(i18n::Str::DownloadBusy);
        status_until_ = SDL_GetTicks() + 2500; return;
    }
    if (it_.is_downloaded(t.video_id)) {
        status_msg_ = i18n::tr(i18n::Str::DownloadDone);
        status_until_ = SDL_GetTicks() + 2500; return;
    }
    dl_running_ = true; dl_done_ = false; dl_ok_ = false; dl_cancel_ = false;
    dl_progress_ = 0; dl_id_ = t.video_id; dl_title_ = t.title;
    int max_h = play_prefs_.max_height;   // match the quality that would have played
    yt::AudioPrefs dl_aprefs;             // ...and the audio language that would have
    dl_aprefs.lang = audio_lang_pref_ == "app"  ? i18n::language_hl(lang_)
                   : audio_lang_pref_ == "orig" ? "" : audio_lang_pref_;
    yt::SearchResult item = t;
    if (dl_thread_.joinable()) dl_thread_.join();
    dl_thread_ = std::thread([this, item, max_h, dl_aprefs]() {
        bool ok = false;
        try {
            // Use the same resolve that plays the video — it works for every video we
            // show, and its adaptive streams download via the chunked (ranged) fetch.
            yt::VideoInfo info = it_.resolve(item.video_id);
            if (getenv("YTC_DEBUG")) {
                const yt::Format* dv = nullptr; { yt::VideoPrefs vp; vp.max_height=max_h; dv=info.best_video(vp); }
                const yt::Format* da = info.best_audio(dl_aprefs);
                std::fprintf(stderr, "[dl] status=%s formats=%zu vf=%s(%d) af=%s remux=%d\n",
                    info.status.c_str(), info.formats.size(),
                    dv?(dv->url.empty()?"nourl":"ok"):"null", dv?dv->height:0,
                    da?(da->url.empty()?"nourl":"ok"):"null", (int)ytn::remux_available());
            }
            if (info.ok()) {
                const std::string id = item.video_id;
                std::string mp4 = it_.download_path(id);
                std::string finalpart = mp4 + ".part";
                HttpClient http;
                auto ua_hdrs = [](const std::string& ua) {
                    std::vector<std::string> h;
                    if (!ua.empty()) h.push_back("User-Agent: " + ua);
                    return h;
                };
                auto prog = [this](int base, int span) {
                    return [this, base, span](long long dn, long long tot) {
                        if (tot > 0) dl_progress_ = base + (int)(dn * span / tot);
                        return !dl_cancel_.load();
                    };
                };
                // Fetch to dest, verifying the byte count matches content_length; retry a
                // couple times so a dropped connection doesn't silently downgrade quality.
                auto fetch = [&](const std::string& url, const std::string& dest,
                                 long long expect, int base, int span,
                                 const std::vector<std::string>& hdrs) -> bool {
                    for (int attempt = 0; attempt < 3 && !dl_cancel_; ++attempt) {
                        if (!http.download(url, dest, hdrs, prog(base, span))) continue;
                        if (expect <= 0) return true;
                        std::ifstream f(dest, std::ios::binary | std::ios::ate);
                        if (f && (long long)f.tellg() == expect) return true;
                    }
                    return false;
                };
                // Mux adaptive video (up to the cap) + audio into one .mp4, matching the
                // quality that would have played.
                yt::VideoPrefs vp; vp.max_height = max_h;
                const yt::Format* vf = info.best_video(vp);
                const yt::Format* af = info.best_audio(dl_aprefs);
                if (ytn::remux_available() && vf && af && !vf->url.empty() && !af->url.empty()) {
                    auto h = ua_hdrs(info.user_agent);
                    std::string vtmp = mp4 + ".v", atmp = mp4 + ".a";
                    bool dv = fetch(vf->url, vtmp, (long long)vf->content_length, 0, 65, h);
                    bool da = dv && !dl_cancel_ && fetch(af->url, atmp, (long long)af->content_length, 65, 30, h);
                    if (dv && da) { dl_progress_ = 96; ok = ytn::remux_to_mp4(vtmp, atmp, finalpart); }
                    if (!getenv("YTC_DEBUG")) { std::remove(vtmp.c_str()); std::remove(atmp.c_str()); }
                }
                if (!ok && !dl_cancel_) {   // no muxer (dev build): 360p single-file
                    // The playback resolve has no progressive stream; only ANDROID_VR
                    // serves the combined itag 18, so resolve once more just for it.
                    yt::VideoInfo pinfo = it_.resolve_for_download(item.video_id);
                    const yt::Format* pf = pinfo.ok() ? pinfo.best_progressive(max_h) : nullptr;
                    if (pf && !pf->url.empty())
                        ok = fetch(pf->url, finalpart, (long long)pf->content_length, 0, 98,
                                   ua_hdrs(pinfo.user_agent));
                }
                if (ok) {
                    dl_progress_ = 100;
                    std::rename(finalpart.c_str(), mp4.c_str());
                    std::string th = !item.thumbnail_url.empty() ? item.thumbnail_url
                                   : ("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg");
                    auto tr = http.get(th);
                    if (tr.ok() && !tr.body.empty()) {
                        std::ofstream tf(it_.download_thumb_path(id), std::ios::binary);
                        tf.write(tr.body.data(), tr.body.size());
                    }
                    it_.write_download_info(id, item.title, item.author,
                                            item.channel_id, info.length_seconds, th,
                                            info.description);
                } else { std::remove(finalpart.c_str()); }
            }
        } catch (...) {}
        if (getenv("YTC_DEBUG")) std::fprintf(stderr, "[dl] finished ok=%d cancel=%d\n",
                                              (int)ok, (int)dl_cancel_.load());
        dl_ok_ = ok; dl_running_ = false; dl_done_ = true;
    });
    status_msg_ = std::string(i18n::tr(i18n::Str::Downloading)) + ": "
                + font_body_->ellipsize(dl_title_, win_->width() * 0.5f);
    status_until_ = SDL_GetTicks() + 60000;
}

void App::poll_download() {
    // Live progress line while downloading.
    if (dl_running_) {
        status_msg_ = std::string(i18n::tr(i18n::Str::Downloading)) + " "
                    + std::to_string(dl_progress_.load()) + "%: " + dl_title_;
        status_until_ = SDL_GetTicks() + 2000;
    }
    if (!dl_done_.exchange(false)) return;
    if (dl_thread_.joinable()) dl_thread_.join();
    refresh_downloads();
    status_msg_ = std::string(i18n::tr(dl_ok_ ? i18n::Str::DownloadDone
                                              : i18n::Str::DownloadFailed)) + ": " + dl_title_;
    status_until_ = SDL_GetTicks() + 3000;
    if (dl_ok_ && view_label_ == "Downloads") load_downloads();   // show the new tile
}
// Drop items per the hide settings: restricted channels (checked or learned) and/or
// Shorts. FAVORITE channels are never hidden by the restricted rule — favoriting is
// explicit user intent (their videos still play, with limited seeking) — but the
// Shorts rule applies everywhere.
void App::filter_hidden(std::vector<yt::SearchResult>& items) {
    // Live broadcasts are always hidden for now (playback is unreliable behind YouTube's
    // bot wall); is_live detection stays so this can be re-enabled as an opt-in later.
    items.erase(std::remove_if(items.begin(), items.end(),
        [&](const yt::SearchResult& r) {
            if (r.is_live) return true;
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
    if (query_.empty()) return home_tab_ == 3 ? "home:posts"
                             : home_tab_ == 4 ? "home:pl" : "home";
    return "q:" + query_;
}

// What a background refresh fetched (stored in refresh_kind_ for poll_refresh).
enum { RK_CHANNEL, RK_PLAYLIST, RK_HOME, RK_HOME_PLAYLISTS, RK_HOME_POSTS, RK_SEARCH };

// Re-fetch whatever list is currently showing (hide-filter toggles, tab switches).
// Network views fetch on a BACKGROUND thread — the UI updates instantly and
// poll_refresh applies the new list when it arrives.
void App::refresh_current_view(bool is_retry) {
    if (!is_retry) { retry_pending_ = false; retry_attempt_ = 0; }  // fresh user action
    // Local (file-backed) views are instant — no thread needed. (Guarded on NOT
    // being in a subview: a lingering label must never hijack a channel refresh.)
    if (!in_channel_view_) {
        if (view_label_ == "Favorite Channels")  { load_favorites(); return; }
        if (view_label_ == "Favorite Playlists") { load_favorite_playlists(); return; }
        if (view_label_ == "Watch Later")        { load_watch_later(); return; }
        if (view_label_ == "Downloads")         { load_downloads(); return; }
        if (view_label_ == "History")           { load_history(); return; }
    }
    if (refresh_running_) return;              // one refresh at a time
    int kind = in_channel_view_
                  ? (!subview_playlist_.empty() ? RK_PLAYLIST : RK_CHANNEL)
                  : query_.empty() ? (home_tab_ == 3 ? RK_HOME_POSTS
                                       : home_tab_ == 4 ? RK_HOME_PLAYLISTS : RK_HOME)
                                   : RK_SEARCH;
    std::string id = kind == RK_PLAYLIST ? subview_playlist_
                   : !channel_info_.channel_id.empty() ? channel_info_.channel_id
                                                       : cont_channel_id_;
    if ((kind == RK_CHANNEL || kind == RK_PLAYLIST) && id.empty()) return;
    std::string query = query_;
    int tab = chan_tab_;
    refresh_sig_ = view_sig();
    refresh_kind_ = kind;
    // Pin the home source for this fetch: if the user flips Home Feed while it's
    // in flight, poll_refresh sees the mismatch and refetches instead of applying
    // a feed built from the old source.
    int hsrc = home_sources_;
    refresh_home_source_ = hsrc;
    refresh_running_ = true; refresh_done_ = false;
    if (refresh_thread_.joinable()) refresh_thread_.join();
    refresh_thread_ = std::thread([this, kind, id, query, tab, hsrc]() {
        yt::Innertube::Feed f;
        yt::Innertube::HomeCursor hc;
        try {   // all paths use thread-local HttpClients inside Innertube
            if (kind == RK_CHANNEL) {
                if (tab == 0)      f = it_.channel_all_feed(id);
                else if (tab == 2) f = it_.channel_shorts_feed(id);
                else if (tab == 3) f = it_.channel_posts_feed(id);
                else if (tab == 4) f = it_.channel_playlists_feed(id);
                else               f = it_.channel_feed(id);
            }
            else if (kind == RK_PLAYLIST)        f = it_.playlist_feed(id);
            else if (kind == RK_HOME) {
                // Blend the enabled sources (bitmask: 1 fav / 2 history / 4 custom):
                // channel uploads via home_all (age-sorted), then the custom feed's
                // saved-search results interleaved in round-robin, deduped by id.
                bool fav = hsrc & 1, hist = (hsrc & 2) != 0, cust = (hsrc & 4) != 0;
                if (fav) f.items = it_.home_all({}, hist, &hc);
                else if (hist) {
                    std::vector<std::string> ids;
                    for (auto& [cid, nm] : it_.history_channels()) ids.push_back(cid);
                    if (!ids.empty()) f.items = it_.home_all(ids, false, &hc);
                }
                if (cust) {
                    std::vector<yt::SearchResult> cf = it_.custom_feed();
                    std::vector<yt::SearchResult> merged;
                    std::unordered_map<std::string, bool> seen;
                    auto key_of = [](const yt::SearchResult& r) {
                        return !r.video_id.empty() ? r.video_id
                             : !r.playlist_id.empty() ? r.playlist_id
                                                      : r.channel_id + r.title;
                    };
                    size_t n = std::max(f.items.size(), cf.size());
                    for (size_t i = 0; i < n; ++i) {
                        if (i < f.items.size() && seen.emplace(key_of(f.items[i]), true).second)
                            merged.push_back(std::move(f.items[i]));
                        if (i < cf.size() && seen.emplace(key_of(cf[i]), true).second)
                            merged.push_back(std::move(cf[i]));
                    }
                    f.items = std::move(merged);
                }
                // Favorite playlists ride with the Favorites source: woven into the
                // All feed near the top at a wide spacing (they carry no upload date,
                // so an age-sort would bury them at the very end).
                if (fav) {
                    size_t pos = 4;
                    for (const auto& p : it_.favorite_playlists()) {
                        yt::SearchResult r;
                        r.kind = yt::SearchResult::Kind::Playlist;
                        r.playlist_id = p.id; r.title = p.title;
                        r.author = p.author; r.thumbnail_url = p.thumb;
                        if (pos >= f.items.size()) f.items.push_back(std::move(r));
                        else f.items.insert(f.items.begin() + pos, std::move(r));
                        pos += 9;
                    }
                }
                // ok unless a source SHOULD have produced content but the network
                // was unreachable (drives the auto-retry banner).
                bool expect = (fav && !it_.favorite_channel_ids().empty()) ||
                              (hist && !it_.history_channels().empty()) ||
                              (cust && !it_.custom_feed_sources().empty());
                f.ok = !expect || it_.has_visitor_data(); }
            else if (kind == RK_HOME_PLAYLISTS){ f.items = it_.home_playlists({}, &hc);
                // Favorited playlists lead the Playlists tab (deduped against the
                // favorites-channels' own playlists fetched above).
                std::vector<yt::SearchResult> lead;
                for (const auto& p : it_.favorite_playlists()) {
                    yt::SearchResult r;
                    r.kind = yt::SearchResult::Kind::Playlist;
                    r.playlist_id = p.id; r.title = p.title;
                    r.author = p.author; r.thumbnail_url = p.thumb;
                    lead.push_back(std::move(r));
                }
                if (!lead.empty()) {
                    f.items.erase(std::remove_if(f.items.begin(), f.items.end(),
                        [&](const yt::SearchResult& r) {
                            for (const auto& l : lead) if (l.playlist_id == r.playlist_id) return true;
                            return false;
                        }), f.items.end());
                    f.items.insert(f.items.begin(), std::make_move_iterator(lead.begin()),
                                   std::make_move_iterator(lead.end()));
                }
                f.ok = (it_.favorite_channel_ids().empty() && lead.empty()) || it_.has_visitor_data(); }
            else if (kind == RK_HOME_POSTS){ f.items = it_.home_posts({}, &hc);
                f.ok = it_.favorite_channel_ids().empty() || it_.has_visitor_data(); }
            else                                 f = it_.search_feed(query);
        } catch (...) {}   // never let an exception escape the thread
        { std::lock_guard<std::mutex> lk(refresh_m_);
          refresh_pending_ = std::move(f); refresh_home_cursor_ = std::move(hc); }
        refresh_running_ = false;
        refresh_done_ = true;
    });
}

void App::poll_refresh() {
    if (!refresh_done_.exchange(false)) return;
    if (refresh_thread_.joinable()) refresh_thread_.join();
    yt::Innertube::Feed f;
    yt::Innertube::HomeCursor hc;
    { std::lock_guard<std::mutex> lk(refresh_m_);
      f = std::move(refresh_pending_); hc = std::move(refresh_home_cursor_); }
    bool fetch_ok = f.ok;
    if (refresh_kind_ == RK_HOME && refresh_home_source_ != home_sources_) {
        // Home Feed source changed while this fetch was in flight (its result is
        // for the old source): discard it and fetch the current source.
        refresh_current_view();
        return;
    }
    if (view_sig() != refresh_sig_) {
        // Stale (user switched tab/view while fetching). If the current view is
        // sitting empty waiting for content, kick off the right fetch now.
        if (results_.empty()) refresh_current_view();
        return;
    }
    switch (refresh_kind_) {
        case RK_HOME:                        // refresh of the Home master feed
            home_items_ = std::move(f.items);
            home_cursor_ = std::move(hc);    // per-channel tokens for "load more"
            cont_token_.clear();
            apply_home_tab();                // re-apply the active All/Videos/Shorts filter
            break;
        case RK_HOME_PLAYLISTS:
            home_playlists_ = f.items;       // cache (pre-filter copy)
            home_playlists_loaded_ = true;
            home_pl_cursor_ = std::move(hc); // per-channel Playlists-tab tokens
            cont_token_.clear();
            set_results(std::move(f.items));
            break;
        case RK_HOME_POSTS:
            home_posts_ = f.items;           // cache
            home_posts_loaded_ = true;
            home_posts_cursor_ = std::move(hc);
            cont_token_.clear();
            set_results(std::move(f.items));
            break;
        default:
            cont_token_ = f.continuation; cont_endpoint_ = f.endpoint;
            cont_channel_id_ = f.channel_id;
            // Channel tabs: rows omit the uploader; the view title (query_) is the channel
            // name. Fill it in BEFORE set_results so the composed tiles show it.
            if (refresh_kind_ == RK_CHANNEL)
                for (auto& r : f.items)
                    if (r.author.empty()) r.author = query_;
            set_results(std::move(f.items));
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
    // Don't open behind another modal overlay — Start is wired straight to this from the
    // event loop (bypassing input()), so guard here for every caller.
    if (cast_picker_open_ || cast_confirm_remove_ || cast_manage_open_ ||
        resume_prompt_open_ || desc_open_ || comments_open_ || casting_ ||
        mode_ == Mode::Search) return;
    menu_kind_ = MenuKind::Main;
    menu_items_.clear();
    using i18n::tr; using S = i18n::Str;
    menu_items_.push_back({tr(S::Home),             MenuAction::GoHome});
    menu_items_.push_back({tr(S::FavoriteChannels), MenuAction::GoFavorites});
    menu_items_.push_back({tr(S::FavoritePlaylists), MenuAction::GoFavoritePlaylists});
    menu_items_.push_back({tr(S::WatchLater),       MenuAction::GoWatchLater});
    menu_items_.push_back({tr(S::Downloads),        MenuAction::GoDownloads});
    menu_items_.push_back({tr(S::History),          MenuAction::GoHistory});
    menu_items_.push_back({tr(S::Settings),         MenuAction::GoSettings});
    menu_items_.push_back({tr(S::Exit),             MenuAction::Quit});
    menu_sel_ = 0;
    // Opening a menu over the player pauses it; resumed when the menu closes without
    // navigating away. Don't clear menu_paused_ if we're already paused (e.g. returning
    // from the Settings submenu), or we'd forget to resume on close.
    if (mode_ == Mode::Playing && !player_.paused()) { player_.set_pause(true); menu_paused_ = true; }
    menu_open_ = true;
}

// Human label for a max-height cap (0 = uncapped).
static std::string quality_label(int h) {
    if (h <= 0) return i18n::tr(i18n::Str::AutoHighest);
    return std::to_string(h) + "p";
}

// Languages offered by the Audio/Caption Language settings — the ones YouTube
// commonly auto-dubs or captions. English names on purpose: the bundled font
// covers Latin/Cyrillic/Greek only, so endonyms like العربية wouldn't render.
static const struct { const char* code; const char* name; } kLangChoices[] = {
    {"en","English"}, {"es","Spanish"}, {"pt","Portuguese"}, {"fr","French"},
    {"de","German"}, {"it","Italian"}, {"ru","Russian"}, {"uk","Ukrainian"},
    {"el","Greek"}, {"pl","Polish"}, {"tr","Turkish"}, {"ar","Arabic"},
    {"hi","Hindi"}, {"bn","Bengali"}, {"id","Indonesian"}, {"vi","Vietnamese"},
    {"th","Thai"}, {"ja","Japanese"}, {"ko","Korean"}, {"zh","Chinese"},
};
static const int kLangChoiceCount = (int)(sizeof(kLangChoices)/sizeof(kLangChoices[0]));

// Display label for an audio/cc language preference value ("app"/"orig"/"off"/code).
static std::string lang_pref_label(const std::string& v) {
    using S = i18n::Str;
    if (v == "app")  return i18n::tr(S::AppLanguage);
    if (v == "orig") return i18n::tr(S::OriginalTrack);
    if (v == "off")  return i18n::tr(S::Off);
    for (const auto& l : kLangChoices) if (v == l.code) return l.name;
    return v;   // unknown code from a hand-edited settings.json: show it raw
}

// Cycle a language preference through [specials..., kLangChoices...] by dir.
static std::string cycle_lang_pref(const std::string& cur, int dir,
                                   const std::vector<std::string>& specials) {
    std::vector<std::string> vals = specials;
    for (const auto& l : kLangChoices) vals.push_back(l.code);
    int n = (int)vals.size(), idx = 0;
    for (int i = 0; i < n; ++i) if (vals[i] == cur) { idx = i; break; }
    return vals[((idx + dir) % n + n) % n];
}

// ---------- Settings menu + its Audio / Video / Captions submenus ----------
void App::open_settings() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::Settings;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetAudioMenu),    MenuAction::GoSettingsAudio});
    menu_items_.push_back({tr(S::SetVideoMenu),    MenuAction::GoSettingsVideo});
    menu_items_.push_back({tr(S::SetCaptionsMenu), MenuAction::GoSettingsCaptions});
    menu_items_.push_back({tr(S::SetPlaybackMenu), MenuAction::GoSettingsPlayback});
    menu_items_.push_back({tr(S::SetBrowsingMenu), MenuAction::GoSettingsBrowsing});
    // Language stays top-level on purpose: a user stuck in the wrong language
    // must be able to find it without reading submenu labels.
    menu_items_.push_back({tr(S::SetLanguage) + std::string(":  ") + i18n::language_name(lang_),
                           MenuAction::CycleLanguage});
    menu_items_.push_back({tr(S::SetLinkedDevices), MenuAction::GoLinkedDevices});
    menu_sel_ = 0;
    // (Do not re-pause here; if opened from the player the main menu already did.)
    menu_open_ = true;
}

void App::open_settings_playback() {
    using i18n::tr; using S = i18n::Str;
    auto onoff = [&](bool b){ return std::string(tr(b ? S::On : S::Off)); };
    menu_kind_ = MenuKind::SettingsPlayback;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetAskResume) + std::string(":  ") + onoff(ask_resume_),
                           MenuAction::ToggleAskResume});
    menu_items_.push_back({tr(S::SetAutoplay) + std::string(":  ") + onoff(autoplay_),
                           MenuAction::ToggleAutoplay});
    menu_items_.push_back({tr(S::SetSponsorBlock) + std::string(":  ") + onoff(sponsorblock_),
                           MenuAction::ToggleSponsorBlock});
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_browsing() {
    using i18n::tr; using S = i18n::Str;
    auto onoff = [&](bool b){ return std::string(tr(b ? S::On : S::Off)); };
    menu_kind_ = MenuKind::SettingsBrowsing;
    menu_items_.clear();
    const S vname[] = {S::ViewGrid, S::ViewCarousel, S::View3DCarousel, S::ViewCoverflow};
    menu_items_.push_back({tr(S::SetView) + std::string(":  ") + tr(vname[(int)view_mode_]),
                           MenuAction::CycleView});
    menu_items_.push_back({tr(S::SetHomeFeed), MenuAction::GoHomeFeedMenu});
    menu_items_.push_back({tr(S::SetCustomFeed), MenuAction::GoCustomFeed});
    menu_items_.push_back({tr(S::SetHidePaced) + std::string(":  ") + onoff(hide_restricted_),
                           MenuAction::ToggleHideRestricted});
    menu_sel_ = 0;
    menu_open_ = true;
}

// Browsing > Home Feed: what flows into the Home grid — any mix of Favorites
// (channel uploads), History (channels you've watched), and the Custom feed's
// saved searches. At least one stays on (the toggle refuses to clear the last).
void App::open_settings_homefeed() {
    using i18n::tr; using S = i18n::Str;
    auto onoff = [&](int bit){ return std::string(tr(home_sources_ & bit ? S::On : S::Off)); };
    menu_kind_ = MenuKind::SettingsHomeFeed;
    menu_items_.clear();
    menu_items_.push_back({tr(S::Favorites) + std::string(":  ") + onoff(1),
                           MenuAction::ToggleFeedFavorites});
    menu_items_.push_back({tr(S::History) + std::string(":  ") + onoff(2),
                           MenuAction::ToggleFeedHistory});
    menu_items_.push_back({tr(S::HomeCustom) + std::string(":  ") + onoff(4),
                           MenuAction::ToggleFeedCustom});
    menu_sel_ = 0;
    menu_open_ = true;
}

// Browsing > Custom Feed: one row per saved search ("query · filters"); A removes
// (with a Yes/No confirm page), B returns to Browsing. Searches are added from
// the search-results options menu ("Add Search to Custom Feed").
void App::open_feed_manage() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::FeedManage;
    menu_items_.clear();
    static const S kType[] = {S::Off, S::TabVideos, S::FilterChannels, S::TabPlaylists};
    static const S kDur[]  = {S::Off, S::DurShort, S::DurMedium, S::DurLong};
    static const S kDate[] = {S::Off, S::DateToday, S::DateWeek, S::DateMonth, S::DateYear};
    static const S kSort[] = {S::Off, S::FilterRelevance, S::FilterPopularity};
    for (const auto& s : it_.custom_feed_sources()) {
        std::string label = "\"" + s.query + "\"";
        if (s.type)        { label += " \xC2\xB7 "; label += tr(kType[s.type & 3]); }
        if (s.duration)    { label += " \xC2\xB7 "; label += tr(kDur[s.duration & 3]); }
        if (s.upload_date) { label += " \xC2\xB7 "; label += tr(kDate[s.upload_date % 5]); }
        if (s.sort)        { label += " \xC2\xB7 "; label += tr(kSort[s.sort % 3]); }
        menu_items_.push_back({label, MenuAction::FeedSourceRow});
    }
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_audio() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::SettingsAudio;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetVolume) + std::string(":  ") + std::to_string(volume_) + "%",
                           MenuAction::CycleVolume});
    menu_items_.push_back({tr(S::SetAudioLang) + std::string(":  ") + lang_pref_label(audio_lang_pref_),
                           MenuAction::CycleAudioLang});
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_video() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::SettingsVideo;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetMaxQuality) + std::string(":  ") + quality_label(play_prefs_.max_height),
                           MenuAction::CycleMaxQuality});
    // Video Decode: shown only when BOTH sides are real — the device has a v4l2
    // decoder (hwdetect) AND the loaded libavcodec has the v4l2m2m decoder (the
    // optional hwdec lib bundle, installed by ytc_setup). Software-only builds
    // keep the row hidden, so the toggle can never be a no-op.
    if (hwdec_capable_)
        menu_items_.push_back({tr(S::SetVideoDecode) + std::string(":  ")
                               + tr(hwdec_mode_ ? S::Software : S::Hardware),
                               MenuAction::CycleHwdec});
    const S aname[] = {S::AspectFit, S::AspectZoom, S::AspectStretch};
    menu_items_.push_back({tr(S::SetAspect) + std::string(":  ") + tr(aname[aspect_mode_]),
                           MenuAction::CycleAspect});
    menu_sel_ = 0;
    menu_open_ = true;
}

void App::open_settings_captions() {
    using i18n::tr; using S = i18n::Str;
    menu_kind_ = MenuKind::SettingsCaptions;
    menu_items_.clear();
    menu_items_.push_back({tr(S::SetCaptionLang) + std::string(":  ") + lang_pref_label(cc_lang_pref_),
                           MenuAction::CycleCaptionLang});
    const S sname[] = {S::SizeSmall, S::SizeMedium, S::SizeLarge};
    menu_items_.push_back({tr(S::SetCaptionSize) + std::string(":  ") + tr(sname[cc_size_]),
                           MenuAction::CycleCaptionSize});
    const S styname[] = {S::StyleWhite, S::StyleYellow, S::StyleWhiteOnBlack,
                         S::StyleBlackOnWhite, S::StyleYellowOnBlack, S::StyleYellowOnBlue};
    menu_items_.push_back({tr(S::SetCaptionStyle) + std::string(":  ") + tr(styname[cc_style_]),
                           MenuAction::CycleCaptionStyle});
    menu_sel_ = 0;
    menu_open_ = true;
}

// Rebuild whichever settings page (or player menu) is current, for in-place
// label refreshes after a Left/Right value change.
void App::reopen_settings_menu() {
    switch (menu_kind_) {
        case MenuKind::SettingsAudio:    open_settings_audio(); break;
        case MenuKind::SettingsVideo:    open_settings_video(); break;
        case MenuKind::SettingsCaptions: open_settings_captions(); break;
        case MenuKind::SettingsPlayback: open_settings_playback(); break;
        case MenuKind::SettingsBrowsing: open_settings_browsing(); break;
        case MenuKind::SettingsHomeFeed: open_settings_homefeed(); break;
        case MenuKind::Settings:         open_settings(); break;
        default:                         open_menu(); break;
    }
}

// Change a setting's value by dir (-1 = Left, +1 = Right) and persist it.
void App::adjust_setting(MenuAction a, int dir) {
    // Search-filter rows cycle Off..N-1 (wrap) and rebuild the modal in place.
    auto cycle = [&](int& v, int n) { v = (v + dir % n + n) % n;
        int keep = menu_sel_; open_search_filters(/*snapshot=*/false); menu_sel_ = keep; };
    if (a == MenuAction::CycleFilterType)     { cycle(filt_type_, 4);     return; }
    if (a == MenuAction::CycleFilterDuration) { cycle(filt_duration_, 4); return; }
    if (a == MenuAction::CycleFilterDate)     { cycle(filt_date_, 5);     return; }
    if (a == MenuAction::CycleFilterSort)     { cycle(filt_sort_, 3);     return; }
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
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleAskResume) {
        ask_resume_ = !ask_resume_;
        it_.set_setting_int("ask_resume", ask_resume_ ? 1 : 0);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleVolume) {
        int v = volume_ + dir * 5;
        if (v < 0) v = 0;
        if (v > 150) v = 150;
        volume_ = v;
        it_.set_setting_int("volume", volume_);
        if (mode_ == Mode::Playing) player_.set_volume(volume_);   // live if playing
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleAutoplay) {
        autoplay_ = !autoplay_;
        it_.set_setting_int("autoplay", autoplay_ ? 1 : 0);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::ToggleFeedFavorites || a == MenuAction::ToggleFeedHistory ||
        a == MenuAction::ToggleFeedCustom) {
        int bit = a == MenuAction::ToggleFeedFavorites ? 1
                : a == MenuAction::ToggleFeedHistory   ? 2 : 4;
        int next = home_sources_ ^ bit;
        if (!next) return;                       // never all-off: keep the last source
        home_sources_ = next;
        it_.set_setting_int("home_sources", home_sources_);
        // If we're viewing Home, re-fetch with the new source.
        if (query_.empty() && view_label_.empty() && !in_channel_view_)
            refresh_current_view();
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleLanguage) {
        int n = i18n::language_count();
        lang_ = ((lang_ + dir) % n + n) % n;        // Left/Right wraps
        it_.set_setting_int("lang", lang_);
        i18n::set_language(lang_);
        it_.set_locale(i18n::language_hl(lang_), i18n::language_gl(lang_));
        // Re-fetch the current view so titles/metadata come back in the new language.
        // (No worker is running here — this is a synchronous menu action on the UI thread.)
        if (!in_channel_view_ && view_label_.empty()) { load_home(); }
        else refresh_current_view();
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;   // relabels the menu
        return;
    }
    if (a == MenuAction::ToggleSponsorBlock) {
        sponsorblock_ = !sponsorblock_;
        it_.set_setting_int("sponsorblock", sponsorblock_ ? 1 : 0);
        if (sponsorblock_ && mode_ == Mode::Playing)      // fetch now for the current video
            start_sponsorblock(now_playing_item_.video_id);
        else if (!sponsorblock_) { std::lock_guard<std::mutex> lk(sb_m_);
            sb_segments_.clear(); sb_skipped_.clear(); }
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
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
        player_.set_hwdec(hwdec_mode_str());
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        // hwdec is fixed at mpv init, so a live change needs a fresh stream: re-resolve
        // the current video at its position (applies the new decoder immediately).
        if (mode_ == Mode::Playing) { menu_open_ = false; menu_paused_ = false;
                                      replay_current(player_.position()); }
        return;
    }
    if (a == MenuAction::CycleAudioLang) {
        audio_lang_pref_ = cycle_lang_pref(audio_lang_pref_, dir, {"app", "orig"});
        it_.set_setting_str("audio_lang", audio_lang_pref_);
        // Default for the NEXT videos; the playing one keeps its (possibly
        // overridden) track — the player-menu Audio Track row changes it live.
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptionLang) {
        cc_lang_pref_ = cycle_lang_pref(cc_lang_pref_, dir, {"app"});
        it_.set_setting_str("cc_lang", cc_lang_pref_);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptionSize) {
        cc_size_ = ((cc_size_ + dir) % 3 + 3) % 3;
        it_.set_setting_int("cc_size", cc_size_);
        player_.set_caption_style(cc_size_, cc_style_);   // live if captions showing
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleCaptionStyle) {
        cc_style_ = ((cc_style_ + dir) % 6 + 6) % 6;
        it_.set_setting_int("cc_style", cc_style_);
        player_.set_caption_style(cc_size_, cc_style_);
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleAudioTrack) {
        // Player menu only: pick a dub for THIS video (session-local, never saved).
        int n = (int)playing_audio_tracks_.size();
        if (n > 1) {
            std::string cur = audio_override_lang_.empty() ? playing_audio_lang_
                                                           : audio_override_lang_;
            int idx = 0;
            for (int i = 0; i < n; ++i)
                if (playing_audio_tracks_[i].lang == cur) { idx = i; break; }
            idx = ((idx + dir) % n + n) % n;
            audio_override_lang_ = playing_audio_tracks_[idx].lang;
            audio_dirty_ = true;             // re-resolve with the new track on menu close
        }
        int keep = menu_sel_; open_menu(); menu_sel_ = keep;   // rebuild label
        return;
    }
    if (a == MenuAction::CycleAspect) {
        aspect_mode_ = ((aspect_mode_ + dir) % 3 + 3) % 3;     // Fit -> Zoom -> Stretch, wraps
        it_.set_setting_int("aspect", aspect_mode_);
        player_.set_aspect(aspect_mode_);        // mpv property change: applies live
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
        return;
    }
    if (a == MenuAction::CycleView) {
        int nv = ((int)view_mode_ + (dir < 0 ? 3 : 1)) % 4;
        view_mode_ = (ViewMode)nv;
        it_.set_setting_int("view", nv);
        carousel_pos_ = sel_;
        int keep = menu_sel_; reopen_settings_menu(); menu_sel_ = keep;
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
        reopen_settings_menu();
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
    char tb[16]; std::snprintf(tb, sizeof tb, "%d:%02d", m, sec);
    std::string opts[2] = { std::string(i18n::tr(i18n::Str::ResumeFrom)) + " " + tb,
                            i18n::tr(i18n::Str::StartOver) };
    float iy = py + 60*s, ih = 56*s, gap = 10*s;
    for (int i = 0; i < 2; ++i) {
        bool sel = (i == resume_prompt_sel_);
        rn.quad({px, iy, pw, ih}, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({px, iy, 4*s, ih}, theme_.accent);
        rn.text(*font_body_, opts[i], px + 18*s, iy + (ih-font_body_->line_height())/2 + 3*s,
                sel ? theme_.text : theme_.text_dim);
        iy += ih + gap;
    }
    rn.text(*font_small_, i18n::tr(i18n::Str::FooterResume),
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
    desc_is_post_ = false; desc_post_id_.clear();
    desc_open_ = true;
    // Playing video: the description arrived with the resolve — no fetch needed.
    if (mode_ == Mode::Playing && !v.is_playlist() &&
        v.video_id == now_playing_item_.video_id) {
        desc_text_ = now_playing_desc_.empty() ? "(no description)" : now_playing_desc_;
        desc_loading_ = false;
        return;
    }
    // Downloaded video in the Downloads view: read the description saved in its .info
    // (offline, instant). From other views a downloaded video is treated as normal and
    // its description is fetched live below.
    if (!v.is_playlist() && !v.video_id.empty() && it_.is_downloaded(v.video_id) &&
        view_label_ == "Downloads") {
        std::string d = it_.download_description(v.video_id);
        desc_text_ = d.empty() ? "(no description)" : d;
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
    { std::lock_guard<std::mutex> lk(cc_m_); cc_tracks_.clear(); cc_pending_.clear(); }
    cc_sel_ = 0; cc_paths_.clear();
    cc_dl_want_url_.clear(); cc_dl_want_key_.clear(); cc_dl_want_tlang_.clear();  // drop the
                                                       // old video's queued fetch
    cc_fail_pending_ = false;                          // stale failure: not this video's
    ++cc_sig_;                 // invalidate any in-flight fetch (its publish is dropped)
    cc_list_want_ = video_id;  // queue THIS video's list fetch; if the worker is still
                               // on the old video, poll_captions() starts it on finish —
                               // the old early-return here silently skipped the fetch,
                               // leaving the new video showing the old (often empty) list
    if (video_id.empty()) { cc_list_want_.clear(); return; }
    maybe_start_cc_list();
}
// Start the queued track-list fetch if the worker is free. Never blocks.
void App::maybe_start_cc_list() {
    if (cc_running_ || cc_list_want_.empty()) return;
    if (cc_thread_.joinable()) cc_thread_.join();   // worker idle -> instant
    std::string vid = std::move(cc_list_want_);
    cc_list_want_.clear();
    int sig = cc_sig_;
    cc_running_ = true; cc_done_ = false;
    cc_thread_ = std::thread([this, vid, sig]() {
        std::vector<yt::CaptionTrack> t;
        try { t = it_.caption_tracks(vid); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cc_m_); if (sig == cc_sig_) cc_pending_ = std::move(t); }
        cc_running_ = false; cc_done_ = true;
    });
}
void App::poll_captions() {
    if (!cc_done_.exchange(false)) return;
    { std::lock_guard<std::mutex> lk(cc_m_);
      cc_tracks_ = std::move(cc_pending_);
      cc_pending_.clear(); }
    // Caption Language setting: order the preferred language first (manual track
    // before auto-generated) so turning captions ON lands on it. It never turns
    // captions on by itself — cc_sel_ stays 0 (Off) here.
    std::string want = cc_lang_pref_ == "app" ? i18n::language_hl(lang_) : cc_lang_pref_;
    auto primary = [](const std::string& s) { return s.substr(0, s.find('-')); };
    auto rank = [&](const yt::CaptionTrack& t) {
        if (primary(t.language_code) != primary(want)) return 2;
        return t.auto_generated ? 1 : 0;
    };
    std::stable_sort(cc_tracks_.begin(), cc_tracks_.end(),
                     [&](const yt::CaptionTrack& a, const yt::CaptionTrack& b) {
                         return rank(a) < rank(b); });
    // Preferred language not offered at all? YouTube can machine-translate any
    // translatable track server-side (&tlang=, at fetch time) — synthesize a
    // "<Language> (translated)" entry as the FIRST option in the cycle. Prefer a
    // human-made source track over ASR (translation quality compounds).
    bool want_failed = std::find(cc_failed_langs_.begin(), cc_failed_langs_.end(), want)
                       != cc_failed_langs_.end();   // failed earlier this video: don't re-offer
    if (!want_failed && !cc_tracks_.empty() && rank(cc_tracks_.front()) == 2) {
        const yt::CaptionTrack* src = nullptr;
        for (const auto& t : cc_tracks_)
            if (t.translatable && (!src || (src->auto_generated && !t.auto_generated)))
                src = &t;
        if (src) {
            yt::CaptionTrack synth;
            synth.language_code = want;
            synth.name = lang_pref_label(want) + " (" + i18n::tr(i18n::Str::CcTranslated) + ")";
            synth.base_url = src->base_url;
            synth.auto_generated = true;
            synth.tlang = want;
            cc_tracks_.insert(cc_tracks_.begin(), std::move(synth));
        }
    }
    // Replay of the same video (quality/audio-track change): put the caption
    // selection back where it was. A new video always starts with captions Off.
    if (!cc_restore_key_.empty()) {
        bool failed = std::find(cc_failed_langs_.begin(), cc_failed_langs_.end(),
                                cc_restore_key_) != cc_failed_langs_.end();
        if (!failed)
            for (int i = 0; i < (int)cc_tracks_.size(); ++i)
                if (cc_tracks_[i].language_code == cc_restore_key_) {
                    cc_sel_ = i + 1; apply_caption_selection(); break;
                }
        cc_restore_key_.clear();
    }
    // The worker is free now — start the queued fetch, if a new video arrived
    // while it was still busy with the old one.
    maybe_start_cc_list();
    // The player options menu may be open showing "Captions: Loading..." — the
    // list just landed, so rebuild it in place with the real state.
    if (menu_open_ && menu_kind_ == MenuKind::Context && mode_ == Mode::Playing) {
        int keep = menu_sel_; open_menu(); menu_sel_ = keep;
    }
}
// Apply the current caption selection WITHOUT blocking: Off hides; an already-cached
// track is added instantly; an un-cached track is downloaded on a worker thread and
// installed later by poll_caption_download().
// The cache key for the current CC selection ("" = Off/invalid) — a track's language code.
std::string App::cc_current_key() const {
    if (cc_sel_ <= 0 || cc_sel_ > (int)cc_tracks_.size()) return "";
    return cc_tracks_[cc_sel_ - 1].language_code;
}
void App::apply_caption_selection() {
    if (cc_sel_ <= 0 || cc_sel_ > (int)cc_tracks_.size()) { player_.subtitles_off(); return; }
    std::string url = cc_tracks_[cc_sel_ - 1].base_url;
    std::string tlang = cc_tracks_[cc_sel_ - 1].tlang;
    std::string key = cc_current_key();
    auto it = cc_paths_.find(key);
    if (it != cc_paths_.end()) { player_.add_subtitle(it->second); return; }  // cached
    // Not cached: fetch off-thread. Hide until it arrives; show a brief status.
    // Queue the request instead of joining a busy worker — a join here would stall
    // the UI thread for a full network round-trip when cycling tracks quickly.
    player_.subtitles_off();
    status_msg_ = i18n::tr(i18n::Str::LoadingCaptions); status_until_ = SDL_GetTicks() + 4000;
    cc_dl_want_url_ = url; cc_dl_want_key_ = key;   // latest selection wins
    cc_dl_want_tlang_ = tlang;
    maybe_start_cc_download();
}
// Start the queued caption fetch if the worker is free. NEVER blocks: while a
// download runs this is a no-op — poll_caption_download() calls back here when
// it finishes, picking up whatever request is queued by then (rapid cycling
// overwrites the slot, so only the newest selection gets fetched next).
void App::maybe_start_cc_download() {
    if (cc_dl_running_) return;
    if (cc_dl_thread_.joinable()) cc_dl_thread_.join();   // worker idle -> instant
    if (cc_dl_want_key_.empty()) return;
    std::string url = std::move(cc_dl_want_url_), key = std::move(cc_dl_want_key_);
    std::string tlang = std::move(cc_dl_want_tlang_);
    cc_dl_want_url_.clear(); cc_dl_want_key_.clear(); cc_dl_want_tlang_.clear();
    cc_dl_running_ = true; cc_dl_done_ = false;
    int sig = cc_sig_;   // which video this fetch belongs to
    cc_dl_thread_ = std::thread([this, url, key, tlang, sig]() {
        std::string vtt;
        try { vtt = it_.caption_vtt(url, tlang); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cc_m_);
          cc_dl_vtt_ = std::move(vtt); cc_dl_lang_ = key; cc_dl_res_sig_ = sig; }
        cc_dl_running_ = false; cc_dl_done_ = true;
    });
}
// Install a finished VTT — but only if that selection is still active — then
// kick off whatever request queued up while the worker was busy.
void App::poll_caption_download() {
    if (!cc_dl_done_.exchange(false)) return;
    std::string vtt, lang; int res_sig;
    { std::lock_guard<std::mutex> lk(cc_m_); vtt = std::move(cc_dl_vtt_); lang = cc_dl_lang_;
      res_sig = cc_dl_res_sig_; cc_dl_vtt_.clear(); }
    if (res_sig != cc_sig_) { maybe_start_cc_download(); return; }  // a previous video's
                                                                    // fetch: discard it
    bool still_selected = !lang.empty() && lang == cc_current_key();
    if (vtt.empty()) {
        if (still_selected && cc_dl_want_key_.empty()) {
            cc_sel_ = 0; player_.subtitles_off();
            // Remember the failure for this video, and drop any synthetic
            // (translated) entry for that language — it will never play, so it
            // must not sit in the cycle inviting the same failure again. Real
            // tracks stay listed (a transient network error shouldn't hide them).
            cc_failed_langs_.push_back(lang);
            cc_tracks_.erase(std::remove_if(cc_tracks_.begin(), cc_tracks_.end(),
                                 [&](const yt::CaptionTrack& t) {
                                     return !t.tlang.empty() && t.language_code == lang;
                                 }),
                             cc_tracks_.end());
            // Don't flash the toast now: with the options menu up the user may not
            // see it before it expires, leaving a mysterious "Captions: Off". The
            // frame loop shows it once the menu is closed.
            cc_fail_pending_ = true;
            if (menu_open_ && menu_kind_ == MenuKind::Context) {   // relabel the row: Off
                int keep = menu_sel_; open_menu(); menu_sel_ = keep;
            }
        }
    } else {
        std::string path = "/tmp/ytc_cc_" + lang + ".vtt";
        { std::ofstream o(path, std::ios::binary); o << vtt; }
        cc_paths_[lang] = path;
        // A queued request for what just landed (selecting a track whose prefetch
        // was in flight) is satisfied — drop it instead of fetching it again.
        if (cc_dl_want_key_ == lang) {
            cc_dl_want_url_.clear(); cc_dl_want_key_.clear(); cc_dl_want_tlang_.clear();
        }
        if (kDbg) std::fprintf(stderr, "[cc] %s -> %zu bytes (%s)\n", lang.c_str(), vtt.size(),
                               still_selected ? "applied" : "cached");
        if (still_selected) player_.add_subtitle(path);   // user may have moved on -> just cache
    }
    maybe_start_cc_download();
}

// Channel description in the same overlay (used from playlist rows, where each
// video names a different uploader).
void App::open_channel_description(const std::string& channel_id, const std::string& name) {
    desc_title_ = name;
    desc_text_.clear();
    desc_lines_.clear();
    desc_scroll_ = 0; desc_wrap_w_ = 0;
    post_has_video_ = false;   // plain description, not the post-with-video layout
    desc_is_post_ = false; desc_post_id_.clear();
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
    desc_is_post_ = true; desc_post_id_ = p.post_id; desc_post_channel_ = p.channel_id;
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
    float wrap_w = pw - pad*2;
    // Title: word-wrap (left-aligned) instead of a single ellipsized line, so a long
    // title breaks at a word boundary and continues below. Capped so it can't eat the
    // whole panel; the last shown line is ellipsized if still more remains.
    float title_lh = font_body_->line_height() + 3*s;
    std::vector<std::string> tlines = wrap_text(*font_body_, desc_title_, wrap_w);
    const int kMaxTitleLines = 3;
    float ty = my + 16*s;
    int nt = std::min((int)tlines.size(), kMaxTitleLines);
    for (int i = 0; i < nt; ++i) {
        std::string line = tlines[i];
        if (i == kMaxTitleLines - 1 && (int)tlines.size() > kMaxTitleLines)
            line = font_body_->ellipsize(line + " \xE2\x80\xA6", wrap_w);   // … more remains
        rn.text(*font_body_, line, tx, ty, theme_.text);
        ty += title_lh;
    }
    float body_top = ty + 20*s, body_bot = my + ph - 46*s;
    // Post with an attached video: a selectable thumbnail on top (A plays it).
    bool has_comments_btn = desc_is_post_ && !desc_post_id_.empty();
    int vid_idx = post_has_video_ ? 0 : -1;
    int com_idx = has_comments_btn ? (post_has_video_ ? 1 : 0) : -1;
    if (post_has_video_) {
        float tw = std::min(pw - pad*2, 360*s);
        float th = tw * 9.f/16.f;
        gfx::Rect vr{tx, body_top, tw, th};
        if (post_focus_ == vid_idx)   // selection border
            rn.quad({vr.x-4*s, vr.y-4*s, vr.w+8*s, vr.h+8*s}, theme_.accent);
        rn.quad(vr, theme_.thumb_bg);
        gfx::Texture* vt = post_thumb_url_.empty() ? nullptr : thumbs_.get(post_thumb_url_);
        if (vt) rn.textured_cover(vr, *vt, {1,1,1,1});
        // Play glyph in the corner.
        rn.text(*font_small_, post_focus_ == vid_idx ? i18n::tr(i18n::Str::PressAToPlay) : "",
                vr.x, vr.y + vr.h + 6*s, theme_.text_dim);
        body_top = vr.y + vr.h + 56*s;   // text starts well below the hint
    }
    // Selectable "Show Comments" button (A opens the comments overlay).
    if (has_comments_btn) {
        bool sel = (post_focus_ == com_idx);
        std::string lbl = std::string("\xF0\x9F\x92\xAC  ") + i18n::tr(i18n::Str::MenuShowComments); // 💬
        float bw = std::min(pw - pad*2, 300*s), bh = 44*s;
        gfx::Rect br{tx, body_top, bw, bh};
        rn.quad(br, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({br.x, br.y, 4*s, bh}, theme_.accent);
        rn.text(*font_small_, lbl, br.x + 14*s, br.y + (bh - font_small_->line_height())/2 + 2*s,
                sel ? theme_.text : theme_.text_dim);
        body_top = br.y + bh + 22*s;
    }
    if (desc_loading_) {
        rn.text(*font_small_, i18n::tr(i18n::Str::LoadingDescription), tx, body_top, theme_.text_dim);
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
    rn.text(*font_small_, post_has_video_ ? i18n::tr(i18n::Str::FooterPostOverlay)
                                          : i18n::tr(i18n::Str::FooterScrollOverlay),
            tx, my + ph - 34*s, theme_.text_dim);
    rn.end();
}

// ---------- Comments overlay ----------
void App::open_comments(const yt::SearchResult& t) {
    comments_is_post_ = t.is_post();
    comments_target_id_ = comments_is_post_ ? t.post_id : t.video_id;
    comments_channel_id_ = t.channel_id;
    comments_title_ = i18n::tr(i18n::Str::CommentsTitle);
    comments_.clear(); comment_lines_.clear(); comment_units_.clear(); comments_wrap_w_ = 0;
    comments_scroll_ = 0; comments_cont_.clear(); comments_total_.clear();
    comments_sel_ = 0; comments_dirty_ = true; comments_reply_idx_ = -1;
    comments_sort_ = 0; comments_sort_top_.clear(); comments_sort_newest_.clear();
    comments_next_sort_token_.clear();
    comments_gen_++;                     // invalidate any in-flight fetch from a prior open
    comments_open_ = true;
    comments_loading_ = true;
    if (comments_target_id_.empty()) { comments_loading_ = false; return; }
    comments_want_fetch_ = true;         // queue the first page; started when the thread is free
    maybe_start_comment_fetch();
}

// Start the queued first-page fetch once no page fetch is in flight. The running one
// (from a previous video) finishes into a stale generation and is discarded.
void App::maybe_start_comment_fetch() {
    if (comments_running_ || !comments_want_fetch_) return;
    comments_want_fetch_ = false;
    comments_running_ = true; comments_done_ = false;
    if (comments_thread_.joinable()) comments_thread_.join();
    unsigned gen = comments_gen_.load();
    std::string id = comments_target_id_, cid = comments_channel_id_; bool is_post = comments_is_post_;
    // A sort switch reloads page 1 with an explicit sort continuation; a fresh open
    // resolves the section token (and the count + sort tokens) from scratch.
    std::string sort_tok = comments_next_sort_token_; comments_next_sort_token_.clear();
    comments_thread_ = std::thread([this, id, cid, is_post, gen, sort_tok]() {
        yt::CommentPage pg;
        try {
            pg = is_post ? it_.post_comments(id, cid, sort_tok) : it_.video_comments(id, sort_tok);
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(comments_m_);
          comments_pending_ = std::move(pg); comments_pending_reset_ = true; comments_pending_gen_ = gen; }
        comments_running_ = false; comments_done_ = true;
    });
}

void App::load_more_comments() {
    if (comments_running_ || comments_cont_.empty() || comments_target_id_.empty()) return;
    comments_running_ = true; comments_done_ = false;
    if (comments_thread_.joinable()) comments_thread_.join();
    unsigned gen = comments_gen_.load();
    std::string id = comments_target_id_, cid = comments_channel_id_, cont = comments_cont_;
    bool is_post = comments_is_post_;
    comments_thread_ = std::thread([this, id, cid, cont, is_post, gen]() {
        yt::CommentPage pg;
        try { pg = is_post ? it_.post_comments(id, cid, cont) : it_.video_comments(id, cont); }
        catch (...) {}
        { std::lock_guard<std::mutex> lk(comments_m_);
          comments_pending_ = std::move(pg); comments_pending_reset_ = false; comments_pending_gen_ = gen; }
        comments_running_ = false; comments_done_ = true;
    });
}

void App::poll_comments_page() {
    if (comments_done_.exchange(false)) {
        if (comments_thread_.joinable()) comments_thread_.join();
        yt::CommentPage pg; bool reset; unsigned gen;
        { std::lock_guard<std::mutex> lk(comments_m_);
          pg = std::move(comments_pending_); reset = comments_pending_reset_;
          gen = comments_pending_gen_; }
        // Apply only if it belongs to the current open (else it's a stale prior video).
        if (gen == comments_gen_.load() && comments_open_) {
            if (reset) {
                comments_ = std::move(pg.items);
                if (!pg.total.empty()) comments_total_ = pg.total;   // keep count across sort
                if (!pg.sort_top.empty())    comments_sort_top_    = pg.sort_top;
                if (!pg.sort_newest.empty()) comments_sort_newest_ = pg.sort_newest;
            } else comments_.insert(comments_.end(),
                                  std::make_move_iterator(pg.items.begin()),
                                  std::make_move_iterator(pg.items.end()));
            comments_cont_ = pg.continuation;
            comments_loading_ = false;
            comments_dirty_ = true;   // re-wrap / rebuild units
        }
    }
    maybe_start_comment_fetch();   // start a queued fetch once the thread is free
}

// Load replies for one comment at a time in the background: user-expanded threads
// first, then threads where the creator replied (shown inline as a preview).
void App::pump_reply_loads() {
    if (!comments_open_ || comments_reply_running_) return;
    int idx = -1;
    for (int i = 0; i < (int)comments_.size(); ++i) {
        yt::Comment& c = comments_[i];
        if (c.replies_loaded || c.replies_loading || c.reply_token.empty()) continue;
        if (c.expanded) { idx = i; break; }            // priority: user expanded
        if (c.has_creator_reply && idx < 0) idx = i;   // otherwise: creator-reply preview
    }
    if (idx < 0) return;
    comments_[idx].replies_loading = true;
    comments_reply_running_ = true; comments_reply_done_ = false;
    comments_reply_idx_ = idx;
    if (comments_reply_thread_.joinable()) comments_reply_thread_.join();
    unsigned gen = comments_gen_.load();
    std::string tok = comments_[idx].reply_token; bool is_post = comments_is_post_;
    comments_reply_thread_ = std::thread([this, tok, is_post, gen]() {
        yt::CommentPage pg;
        try { pg = it_.comment_replies(tok, is_post); } catch (...) {}
        { std::lock_guard<std::mutex> lk(comments_m_);
          comments_reply_pending_ = std::move(pg); comments_reply_pending_gen_ = gen; }
        comments_reply_running_ = false; comments_reply_done_ = true;
    });
}

// Expand or collapse the replies under the selected top-level comment.
void App::toggle_comment_replies() {
    if (comments_sel_ < 0 || comments_sel_ >= (int)comment_units_.size()) return;
    const CommentUnit& u = comment_units_[comments_sel_];
    if (!u.is_top || u.top_idx < 0 || u.top_idx >= (int)comments_.size()) return;
    yt::Comment& c = comments_[u.top_idx];
    if (c.reply_token.empty()) return;                 // no replies to show
    c.expanded = !c.expanded;                          // pump_reply_loads() fetches them
    comments_dirty_ = true;
}

// Switch the comment sort (Top <-> Newest) and reload page 1 in that order.
void App::toggle_comment_sort() {
    if (comments_sort_top_.empty() || comments_sort_newest_.empty()) return;   // no sort here
    comments_sort_ ^= 1;
    std::string tok = comments_sort_ ? comments_sort_newest_ : comments_sort_top_;
    comments_.clear(); comment_lines_.clear(); comment_units_.clear();
    comments_scroll_ = 0; comments_sel_ = 0; comments_cont_.clear();
    comments_dirty_ = true; comments_reply_idx_ = -1;
    comments_gen_++;                          // discard in-flight results from the old sort
    comments_loading_ = true;
    comments_next_sort_token_ = tok;          // page 1 in the chosen order
    comments_want_fetch_ = true;
    maybe_start_comment_fetch();
}

void App::poll_comments() {
    if (comments_reply_done_.exchange(false)) {
        if (comments_reply_thread_.joinable()) comments_reply_thread_.join();
        yt::CommentPage pg; unsigned gen;
        { std::lock_guard<std::mutex> lk(comments_m_);
          pg = std::move(comments_reply_pending_); gen = comments_reply_pending_gen_; }
        if (gen == comments_gen_.load() && comments_open_ && comments_reply_idx_ >= 0 &&
            comments_reply_idx_ < (int)comments_.size()) {   // ignore replies from a prior open
            comments_[comments_reply_idx_].replies = std::move(pg.items);
            comments_[comments_reply_idx_].replies_loaded = true;
            comments_[comments_reply_idx_].replies_loading = false;
            comments_dirty_ = true;
        }
        comments_reply_idx_ = -1;
    }
    poll_comments_page();
    pump_reply_loads();
}

void App::close_comments() {
    comments_open_ = false;
    if (comments_paused_ && mode_ == Mode::Playing) player_.set_pause(false);
    comments_paused_ = false;
}

void App::render_comments(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg);
    float mx = 70*s, my = 46*s;
    float pw = W - mx*2, ph = H - my*2;
    rn.quad({mx, my, pw, ph}, theme_.panel);
    rn.quad({mx, my, pw, 4*s}, theme_.accent);
    float pad = 26*s;
    float tx = mx + pad;
    std::string head = comments_title_;
    if (!comments_total_.empty()) head += "  (" + comments_total_ + ")";      // real total
    else if (!comments_.empty()) head += "  (" + std::to_string(comments_.size())
                                       + (comments_cont_.empty() ? "" : "+") + ")";
    rn.text(*font_body_, font_body_->ellipsize(head, pw - pad*2), tx, my + 16*s, theme_.text);
    // Current sort, to the right of the count (only when sorting is available).
    if (!comments_sort_top_.empty() && !comments_sort_newest_.empty()) {
        std::string sort = i18n::tr(comments_sort_ ? i18n::Str::CommentSortNewest
                                                   : i18n::Str::CommentSortTop);
        float hx = tx + font_body_->text_width(font_body_->ellipsize(head, pw - pad*2)) + 20*s;
        rn.text(*font_small_, sort, hx, my + 20*s, theme_.accent);
    }
    float body_top = my + 62*s, body_bot = my + ph - 46*s;
    float wrap_w = pw - pad*2;

    if (comments_loading_ && comments_.empty()) {
        rn.text(*font_small_, i18n::tr(i18n::Str::CommentsLoading), tx, body_top, theme_.text_dim);
        rn.end(); return;
    }
    if (!comments_loading_ && comments_.empty()) {
        rn.text(*font_small_, i18n::tr(i18n::Str::CommentsEmpty), tx, body_top, theme_.text_dim);
        rn.text(*font_small_, i18n::tr(i18n::Str::FooterComments), tx, my + ph - 34*s, theme_.text_dim);
        rn.end(); return;
    }

    float ind = 34*s;   // reply indent
    // Rebuild the wrapped line list + selectable units when the data or width changes.
    if (comments_dirty_ || comments_wrap_w_ != wrap_w) {
        comment_lines_.clear(); comment_units_.clear();
        auto emit = [&](const yt::Comment& c, int indent) {
            std::string name = c.author.empty() ? "\xE2\x80\x94" : c.author;
            if (c.pinned) name = std::string("\xF0\x9F\x93\x8C ") + name;   // 📌
            std::string sub;
            if (!c.time.empty()) sub += c.time;
            if (!c.likes.empty()) sub += (sub.empty()?"":"   ") + std::string("\xE2\x96\xB2 ") + c.likes;
            if (indent == 0 && c.reply_count > 0)
                sub += (sub.empty()?"":"   ") + std::to_string(c.reply_count) + " \xE2\x86\xB3";
            comment_lines_.push_back({sub, 0, indent, name, c.is_creator});   // meta: name + rest
            for (auto& l : wrap_text(*font_small_, c.text, wrap_w - indent*ind))
                comment_lines_.push_back({l, 1, indent, "", false});
        };
        for (int i = 0; i < (int)comments_.size(); ++i) {
            const yt::Comment& c = comments_[i];
            CommentUnit u{(int)comment_lines_.size(), 0, true, i};
            emit(c, 0);
            // Collapsed creator reply preview (shown inline like the Android app).
            if (!c.expanded && c.has_creator_reply && c.replies_loaded)
                for (const auto& r : c.replies) if (r.is_creator) emit(r, 1);
            if (!c.reply_token.empty()) {
                std::string tog = !c.expanded
                    ? std::string("\xE2\x96\xB8 ") + std::to_string(c.reply_count)
                      + " " + i18n::tr(i18n::Str::CommentReplies)   // ▸ N replies
                    : (c.replies_loaded ? std::string("\xE2\x96\xBE ") + i18n::tr(i18n::Str::CommentHideReplies)
                                        : std::string("\xE2\x96\xBE ") + i18n::tr(i18n::Str::CommentsLoading));
                comment_lines_.push_back({tog, 3, 0});
            }
            u.line_count = (int)comment_lines_.size() - u.line_start;
            comment_units_.push_back(u);
            comment_lines_.push_back({"", 2, 0});
            if (c.expanded)
                for (const auto& r : c.replies) {
                    CommentUnit ru{(int)comment_lines_.size(), 0, false, i};
                    emit(r, 1);
                    ru.line_count = (int)comment_lines_.size() - ru.line_start;
                    comment_units_.push_back(ru);
                    comment_lines_.push_back({"", 2, 1});
                }
        }
        comments_wrap_w_ = wrap_w; comments_dirty_ = false;
    }
    if (comments_sel_ >= (int)comment_units_.size()) comments_sel_ = (int)comment_units_.size() - 1;
    if (comments_sel_ < 0) comments_sel_ = 0;

    float lh = font_small_->line_height() + 4*s;
    float total = comment_lines_.size() * lh;
    float view = body_bot - body_top;
    float max_scroll = total > view ? total - view : 0;
    // Keep the selected unit in view.
    if (!comment_units_.empty()) {
        const CommentUnit& su = comment_units_[comments_sel_];
        float s0 = su.line_start * lh, s1 = (su.line_start + su.line_count) * lh;
        if (s0 < comments_scroll_) comments_scroll_ = s0;
        if (s1 > comments_scroll_ + view) comments_scroll_ = s1 - view;
    }
    if (comments_scroll_ < 0) comments_scroll_ = 0;
    if (comments_scroll_ > max_scroll) comments_scroll_ = max_scroll;
    // Auto-page more top-level comments when the selection nears the end.
    if (comments_sel_ >= (int)comment_units_.size() - 3 && !comments_cont_.empty() && !comments_running_)
        load_more_comments();

    // Selection highlight (behind the selected unit's lines).
    if (!comment_units_.empty()) {
        const CommentUnit& su = comment_units_[comments_sel_];
        float hy = body_top + su.line_start * lh - comments_scroll_ - 3*s;
        float hh = su.line_count * lh + 4*s;
        if (hy < body_bot && hy + hh > body_top) {
            float cy = std::max(hy, body_top), cb = std::min(hy + hh, body_bot);
            rn.quad({mx + 10*s, cy, pw - 20*s, cb - cy}, theme_.card_sel);
            rn.quad({mx + 10*s, cy, 4*s, cb - cy}, theme_.accent);
        }
    }
    float y = body_top - comments_scroll_;
    for (const auto& cl : comment_lines_) {
        if (y >= body_top - 0.5f && y <= body_bot - lh * 0.4f) {
            float x = tx + cl.indent*ind;
            if (cl.kind == 0) {   // meta: colored author name, then dimmed time/likes
                gfx::Color acol = cl.creator ? theme_.accent : theme_.text;
                rn.text(*font_small_, cl.author, x, y, acol);
                if (!cl.text.empty())
                    rn.text(*font_small_, cl.text,
                            x + font_small_->text_width(cl.author) + 12*s, y, theme_.text_dim);
            } else {
                gfx::Color col = cl.kind == 3 ? theme_.accent : theme_.text_dim;
                rn.text(*font_small_, cl.text, x, y, col);
            }
        }
        y += lh;
    }
    if (max_scroll > 0) {   // scrollbar
        float track_h = view, knob_h = view * (view / total);
        if (knob_h < 24*s) knob_h = 24*s;
        float ky = body_top + (track_h - knob_h) * (comments_scroll_ / max_scroll);
        rn.quad({mx + pw - 8*s, body_top, 4*s, track_h}, theme_.card_sel);
        rn.quad({mx + pw - 8*s, ky, 4*s, knob_h}, theme_.accent);
    }
    rn.text(*font_small_, i18n::tr(i18n::Str::FooterCommentsSel), tx, my + ph - 34*s, theme_.text_dim);
    rn.end();
}

// ---------- Context options menu ----------
void App::open_menu() {
    menu_kind_ = MenuKind::Context;
    // Target: the playing video (in player) or the highlighted grid item. On the
    // search screen with no results we still open the menu (Search Filters only) so a
    // filter that returns nothing doesn't strand the user with no way back to filters.
    bool have_item = true;
    if (mode_ == Mode::Playing) menu_target_ = now_playing_item_;
    else { const yt::SearchResult* v = selected();
           if (v) menu_target_ = *v;
           else if (search_tabs_active()) { menu_target_ = yt::SearchResult{}; have_item = false; }
           else return; }

    menu_items_.clear();
    using i18n::tr; using S = i18n::Str;
    const yt::SearchResult& t = menu_target_;
    // On the search-results screen, the options menu leads with Search Filters,
    // plus saving the current query (+ its filters) into the Custom home feed.
    if (search_tabs_active() && mode_ != Mode::Playing) {
        menu_items_.push_back({tr(S::MenuSearchFilters), MenuAction::OpenSearchFilters});
        if (!query_.empty())
            menu_items_.push_back({tr(S::MenuAddSearchFeed), MenuAction::AddSearchToFeed});
    }
    if (have_item) {
    if (t.is_post()) {
        menu_items_.push_back({tr(S::ReadPost), MenuAction::ShowDescription});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::PlayAttachedVideo), MenuAction::PlayPostVideo});
        if (!t.post_id.empty())
            menu_items_.push_back({tr(S::MenuShowComments), MenuAction::ShowComments});
    } else if (t.is_playlist()) {
        bool pfav = fav_pl_ids_.count(t.playlist_id) > 0;
        menu_items_.push_back({tr(pfav ? S::RemovePlaylistFav : S::AddPlaylistFav),
                               MenuAction::FavoritePlaylistToggle});
        bool wl = wl_ids_.count(t.playlist_id) > 0;
        menu_items_.push_back({tr(wl ? S::RemoveWatchLater : S::AddWatchLater),
                               MenuAction::WatchLaterToggle});
        menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowDescription});
        if (!t.channel_id.empty())
            menu_items_.push_back({tr(S::ShowChannelDescription),
                                   MenuAction::ShowChannelDescription});
    } else if (t.is_channel()) {
        bool fav = fav_ids_.count(t.channel_id) > 0;
        menu_items_.push_back({tr(fav ? S::RemoveFavorite : S::AddFavorite),
                               MenuAction::FavoriteToggle});
        menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowChannelDescription});
    } else if (!t.video_id.empty() && dl_ids_.count(t.video_id) && view_label_ == "Downloads") {
        // Offline mode — but only in the Downloads view. Here the video plays from the
        // local file, so only options that work without the network make sense (no
        // comments, channel info, casting, quality, or captions). From other pages a
        // downloaded video streams, so it falls through to the full menu below.
        menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowDescription});
        if (mode_ == Mode::Playing) {   // playback speed is a local mpv setting
            char sb[24]; std::snprintf(sb, sizeof sb, ":  %gx", playback_speed_);
            menu_items_.push_back({tr(S::MenuSpeed) + std::string(sb), MenuAction::CycleSpeed});
        }
        menu_items_.push_back({tr(S::MenuRemoveDownload), MenuAction::RemoveDownload});
    } else {
        if (!t.channel_id.empty()) {
            bool fav = fav_ids_.count(t.channel_id) > 0;
            menu_items_.push_back({tr(fav ? S::RemoveFavorite : S::AddFavorite),
                                   MenuAction::FavoriteToggle});
        }
        bool wl = wl_ids_.count(t.video_id) > 0;
        menu_items_.push_back({tr(wl ? S::RemoveWatchLater : S::AddWatchLater),
                               MenuAction::WatchLaterToggle});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::MenuCastToDevice), MenuAction::CastToDevice});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::ShowDescription), MenuAction::ShowDescription});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(S::MenuShowComments), MenuAction::ShowComments});
        if (!t.video_id.empty())
            menu_items_.push_back({tr(dl_ids_.count(t.video_id) ? S::MenuRemoveDownload
                                                                : S::MenuDownload),
                                   dl_ids_.count(t.video_id) ? MenuAction::RemoveDownload
                                                             : MenuAction::DownloadVideo});
        // Channel description on every video tile with a known uploader.
        if (!t.channel_id.empty())
            menu_items_.push_back({tr(S::ShowChannelDescription),
                                   MenuAction::ShowChannelDescription});
        // The browsed playlist's description (playlist screens only).
        if (!subview_playlist_.empty() && mode_ != Mode::Playing)
            menu_items_.push_back({tr(S::ShowPlaylistDescription),
                                   MenuAction::ShowPlaylistDescription});
        // Quality + Speed + Stats (Left/Right) — only for the playing video.
        if (mode_ == Mode::Playing) {
            menu_items_.push_back({tr(S::MenuQuality) + std::string(":  ")
                                   + quality_label(play_prefs_.max_height),
                                   MenuAction::CycleMaxQuality});
            char sb[24]; std::snprintf(sb, sizeof sb, ":  %gx", playback_speed_);
            menu_items_.push_back({tr(S::MenuSpeed) + std::string(sb), MenuAction::CycleSpeed});
            std::string cc = std::string(tr(S::MenuCaptions)) + ":  ";
            if (cc_tracks_.empty()) cc += cc_running_ ? tr(S::Loading) : tr(S::CcNone);
            else if (cc_sel_ <= 0) cc += tr(S::Off);
            else cc += cc_tracks_[cc_sel_ - 1].name;   // track title, localized by YouTube (hl)
            menu_items_.push_back({cc, MenuAction::CycleCaptions});
            // Audio (dub) track — only when the video actually has alternatives.
            if (playing_audio_tracks_.size() > 1) {
                std::string cur = audio_override_lang_.empty() ? playing_audio_lang_
                                                               : audio_override_lang_;
                std::string tname;
                for (const auto& at : playing_audio_tracks_)
                    if (at.lang == cur) { tname = at.name; break; }
                if (tname.empty()) tname = cur;
                menu_items_.push_back({tr(S::MenuAudioTrack) + std::string(":  ") + tname,
                                       MenuAction::CycleAudioTrack});
            }
            menu_items_.push_back({tr(S::MenuStats) + std::string(":  ")
                                   + tr(stats_for_nerds_ ? S::Enabled : S::Disabled),
                                   MenuAction::ToggleStats});
        }
        if (mode_ != Mode::Playing && !t.channel_id.empty())
            menu_items_.push_back({tr(S::GoToChannel), MenuAction::OpenChannel});
    }
    }  // end have_item
    // Clear the whole watch history — only from a tile in the History view.
    if (mode_ != Mode::Playing && view_label_ == "History")
        menu_items_.push_back({tr(S::ClearHistoryItem), MenuAction::ClearHistory});
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
        case MenuAction::OpenSearchFilters:
            menu_open_ = false; open_search_filters(); return;   // swap context menu -> filters modal
        case MenuAction::FavoriteToggle: {
            std::string name = t.is_channel() ? t.title : t.author;
            if (fav_ids_.count(t.channel_id)) { it_.remove_favorite(t.channel_id);
                status_msg_ = i18n::tr(i18n::Str::RemovedFav) + std::string(": ") + name; }
            else { it_.add_favorite(t.channel_id, name);
                status_msg_ = i18n::tr(i18n::Str::AddedFav) + std::string(": ") + name; }
            status_until_ = SDL_GetTicks() + 4000; refresh_favorites();
            // Un-favoriting from the Favorite Channels view drops the tile now.
            if (view_label_ == "Favorite Channels" && mode_ != Mode::Playing) {
                menu_open_ = false; menu_paused_ = false;
                load_favorites();
                return;
            }
            // Un-favoriting a restricted channel makes it hideable again.
            if (hide_restricted_ && mode_ != Mode::Playing) {
                filter_hidden(results_);
                if (sel_ >= (int)results_.size())
                    sel_ = results_.empty() ? 0 : (int)results_.size() - 1;
            }
            break;
        }
        case MenuAction::FavoritePlaylistToggle: {
            if (fav_pl_ids_.count(t.playlist_id)) {
                it_.remove_favorite_playlist(t.playlist_id);
                status_msg_ = i18n::tr(i18n::Str::RemovedFav) + std::string(": ") + t.title;
            } else {
                it_.add_favorite_playlist({t.playlist_id, t.title, t.author, t.thumbnail_url});
                status_msg_ = i18n::tr(i18n::Str::AddedFav) + std::string(": ") + t.title;
            }
            status_until_ = SDL_GetTicks() + 4000;
            refresh_favorite_playlists();
            // Un-favoriting from the Favorite Playlists view drops the tile now.
            if (view_label_ == "Favorite Playlists" && mode_ != Mode::Playing) {
                menu_open_ = false; menu_paused_ = false;
                load_favorite_playlists();
                return;
            }
            break;
        }
        case MenuAction::WatchLaterToggle: {
            std::string wid = t.is_playlist() ? t.playlist_id : t.video_id;
            if (wl_ids_.count(wid)) { it_.remove_watch_later(wid);
                status_msg_ = i18n::tr(i18n::Str::RemovedWatchLater); }
            else { it_.add_watch_later(wid, t.title, t.is_playlist(),
                                       t.thumbnail_url, t.author, t.view_count_text);
                status_msg_ = i18n::tr(i18n::Str::AddedWatchLater); }
            status_until_ = SDL_GetTicks() + 4000; refresh_watch_later();
            break;
        }
        case MenuAction::DownloadVideo:
            menu_open_ = false; menu_paused_ = false;
            start_download(t);
            return;
        case MenuAction::RemoveDownload: {
            it_.remove_download(t.video_id); refresh_downloads();
            status_msg_ = i18n::tr(i18n::Str::DownloadRemoved);
            status_until_ = SDL_GetTicks() + 2500;
            if (view_label_ == "Downloads") load_downloads();
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
        case MenuAction::ShowComments: {
            menu_open_ = false;
            // Inherit the menu's pause (resumed when the comments overlay closes).
            comments_paused_ = menu_paused_;
            menu_paused_ = false;
            open_comments(t);
            return;
        }
        case MenuAction::PlayPostVideo:
            menu_open_ = false;
            menu_paused_ = false;
            request_playback();          // selected() is the post; its video_id plays
            return;
        case MenuAction::CastToDevice: {
            // Capture the target video (playing video, or the highlighted tile), then
            // open the device picker. If casting from the player, hand off the position.
            cast_target_id_ = t.video_id;
            cast_target_title_ = t.title;
            cast_target_is_short_ = t.is_short;
            cast_target_pos_ = (mode_ == Mode::Playing) ? (int)player_.position() : 0;
            menu_open_ = false; menu_paused_ = false;
            open_cast_picker();
            return;
        }
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
        case MenuAction::GoFavoritePlaylists:
        case MenuAction::GoWatchLater:
        case MenuAction::GoDownloads:
        case MenuAction::GoHistory: {
            MenuAction act = menu_items_[menu_sel_].action;
            menu_open_ = false;
            menu_paused_ = false;             // navigating away; no player to resume
            if (mode_ == Mode::Playing) { save_resume_position(); player_.stop(); }
            in_channel_view_ = false;
            mode_ = Mode::Grid;
            if      (act == MenuAction::GoHome)       load_home();
            else if (act == MenuAction::GoFavorites)  load_favorites();
            else if (act == MenuAction::GoFavoritePlaylists) load_favorite_playlists();
            else if (act == MenuAction::GoWatchLater) load_watch_later();
            else if (act == MenuAction::GoDownloads)  load_downloads();
            else                                      load_history();
            return;
        }
        case MenuAction::GoSettings:
            open_settings();              // switch this overlay to the settings submenu
            return;
        case MenuAction::GoSettingsAudio:    open_settings_audio();    return;
        case MenuAction::GoSettingsVideo:    open_settings_video();    return;
        case MenuAction::GoSettingsCaptions: open_settings_captions(); return;
        case MenuAction::GoSettingsPlayback: open_settings_playback(); return;
        case MenuAction::GoSettingsBrowsing: open_settings_browsing(); return;
        case MenuAction::GoHomeFeedMenu:     open_settings_homefeed(); return;
        case MenuAction::AddSearchToFeed: {
            yt::FeedSource s;
            s.query = query_;
            s.type = filt_type_; s.duration = filt_duration_;
            s.upload_date = filt_date_; s.sort = filt_sort_;
            bool added = it_.add_custom_feed_source(s);
            status_msg_ = i18n::tr(added ? i18n::Str::AddedToFeed : i18n::Str::AlreadyInFeed);
            status_until_ = SDL_GetTicks() + 3000;
            menu_open_ = false; menu_paused_ = false;
            return;
        }
        case MenuAction::GoCustomFeed:
            if (it_.custom_feed_sources().empty()) {
                status_msg_ = i18n::tr(i18n::Str::FeedEmpty);
                status_until_ = SDL_GetTicks() + 3000;
                menu_open_ = false; menu_paused_ = false;
            } else open_feed_manage();
            return;
        case MenuAction::FeedSourceRow:
            // A on a saved search: confirm before removing it.
            feed_remove_idx_ = menu_sel_;
            menu_kind_ = MenuKind::FeedRemoveConfirm;
            menu_items_.clear();
            menu_items_.push_back({i18n::tr(i18n::Str::Yes), MenuAction::FeedRemoveYes});
            menu_items_.push_back({i18n::tr(i18n::Str::No),  MenuAction::FeedRemoveNo});
            menu_sel_ = 1;   // default No
            return;
        case MenuAction::FeedRemoveYes:
            if (feed_remove_idx_ >= 0) it_.remove_custom_feed_source((size_t)feed_remove_idx_);
            feed_remove_idx_ = -1;
            if (it_.custom_feed_sources().empty()) {
                // Feed emptied: fall back off Custom, and land back on Browsing.
                if (home_sources_ & 4) {
                    home_sources_ &= ~4;
                    if (!home_sources_) home_sources_ = 1;
                    it_.set_setting_int("home_sources", home_sources_);
                }
                open_settings_browsing();
            } else open_feed_manage();
            // Viewing Home on the Custom feed? Refresh with the source gone.
            if (query_.empty() && view_label_.empty() && !in_channel_view_ && mode_ != Mode::Playing)
                refresh_current_view();
            return;
        case MenuAction::FeedRemoveNo:
            feed_remove_idx_ = -1;
            open_feed_manage();
            return;
        case MenuAction::GoLinkedDevices:
            menu_open_ = false; menu_paused_ = false;
            open_manage_devices();
            return;
        case MenuAction::ClearHistory:
            menu_open_ = false; menu_paused_ = false;
            it_.clear_history();
            load_history();               // reload the (now empty) view
            status_msg_ = i18n::tr(i18n::Str::HistoryCleared);
            status_until_ = SDL_GetTicks() + 3000;
            return;
        case MenuAction::CycleMaxQuality:
        case MenuAction::ToggleStats:
        case MenuAction::ToggleHideRestricted:
        case MenuAction::ToggleAskResume:
        case MenuAction::CycleView:
        case MenuAction::CycleVolume:
        case MenuAction::CycleHwdec:
        case MenuAction::CycleAspect:
        case MenuAction::CycleAudioLang:
        case MenuAction::CycleCaptionLang:
        case MenuAction::CycleAudioTrack:
        case MenuAction::CycleCaptionSize:
        case MenuAction::CycleCaptionStyle:
        case MenuAction::CycleSpeed:
        case MenuAction::ToggleSponsorBlock:
        case MenuAction::CycleCaptions:
        case MenuAction::ToggleAutoplay:
        case MenuAction::ToggleFeedFavorites:
        case MenuAction::ToggleFeedHistory:
        case MenuAction::ToggleFeedCustom:
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
    for (auto& r : f.items)                              // channel rows omit the uploader; fill
        if (r.author.empty()) r.author = name;           // it in BEFORE composing the tiles
    set_results(std::move(f.items));
    if (results_.empty()) {
        status_msg_ = i18n::tr(i18n::Str::NoRecentUploads);
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
// home feed; Playlists and Posts fetch (async) on first visit and are cached after.
void App::load_home_tab(int tab) {
    if (tab < 0 || tab > 4) return;
    home_tab_ = tab;
    tab_focus_ = false;                                 // first video selected on load
    cont_token_.clear();
    if (tab == 3 || tab == 4) {
        bool loaded = tab == 3 ? home_posts_loaded_ : home_playlists_loaded_;
        if (loaded) {
            auto copy = tab == 3 ? home_posts_ : home_playlists_;
            set_results(std::move(copy));
        } else {
            results_.clear(); sel_ = 0; scroll_ = 0;
            refresh_current_view();          // async home_playlists / home_posts fetch
        }
    } else {
        apply_home_tab();
    }
}

// Rebuild results_ from the Home master list for the active All/Videos/Shorts tab.
void App::apply_home_tab() {
    std::vector<yt::SearchResult> items;
    for (const auto& r : home_items_) {
        if (home_tab_ == 1 && (r.is_short || r.is_post() || r.is_playlist())) continue;  // Videos only
        if (home_tab_ == 2 && !r.is_short) continue;                  // Shorts only
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
        status_msg_ = i18n::tr(i18n::Str::PlaylistUnavailable);
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
        else if (v.is_short && v.author.empty() && !v.channel_id.empty())
            chan_meta_.request(v.channel_id);   // resolve the short's uploader name
    }
    size_t added = next.items.size();
    bool search_ctx = !query_.empty() && !in_channel_view_ && !search_base_.empty();
    if (search_ctx) {
        // Search results: new page joins the canonical relevance list, then we rebuild
        // results_ in the active sort order (keeping the current selection by id).
        search_base_.insert(search_base_.end(), std::make_move_iterator(next.items.begin()),
                            std::make_move_iterator(next.items.end()));
        apply_search_view(/*keep_selection=*/true);
    } else {
        results_.insert(results_.end(), std::make_move_iterator(next.items.begin()),
                        std::make_move_iterator(next.items.end()));
        if (channel_tabs_active())         // channel-tab rows omit the uploader
            for (auto& r : results_)
                if (r.author.empty()) r.author = query_;
        build_tile_lines();                // refresh metadata-line cache for the grown list
    }
    cont_token_ = next.continuation;   // new token, or "" at the end of the feed
    queue_restricted_checks();
    if (getenv("YTC_DEBUG"))
        std::fprintf(stderr, "[more] +%zu -> total %zu (%s)\n", added, results_.size(),
                     cont_token_.empty() ? "END" : "more available");
}

// Home feed pages differently from search/channel: no single continuation token,
// but a per-channel cursor. When the selection nears the bottom, continue every
// channel one page on a worker, then merge the batch into home_items_ (append-only,
// so the selection doesn't jump) and re-apply the active All/Videos/Shorts filter.
// The active Home sub-tab's cursor: Playlists (3) and Posts (4) are separate
// aggregated feeds with their own per-channel cursors; All/Videos/Shorts share one.
yt::Innertube::HomeCursor& App::home_cursor_for(int tab) {
    return tab == 3 ? home_posts_cursor_ : tab == 4 ? home_pl_cursor_ : home_cursor_;
}
void App::maybe_load_more_home() {
    if (home_more_running_) return;
    if (!(query_.empty() && view_label_.empty() && !in_channel_view_)) return;  // home master only
    int tab = home_tab_;
    if (!home_cursor_for(tab).has_more()) return;
    int n = (int)results_.size();
    if (n == 0 || sel_ < n - cols_ * 2) return; // only when near the last rows
    home_more_running_ = true; home_more_done_ = false; home_more_tab_ = tab;
    if (home_more_thread_.joinable()) home_more_thread_.join();
    yt::Innertube::HomeCursor cursor = home_cursor_for(tab);   // worker owns a copy
    home_more_thread_ = std::thread([this, cursor, tab]() mutable {
        std::vector<yt::SearchResult> batch;
        try {
            batch = tab == 3 ? it_.home_posts_more(cursor)
                  : tab == 4 ? it_.home_playlists_more(cursor)
                             : it_.home_feed_more(cursor);
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(home_more_m_);
          home_more_pending_ = std::move(batch); home_more_cursor_pending_ = std::move(cursor); }
        home_more_running_ = false; home_more_done_ = true;
    });
}
void App::poll_more_home() {
    if (!home_more_done_.exchange(false)) return;
    if (home_more_thread_.joinable()) home_more_thread_.join();
    std::vector<yt::SearchResult> batch;
    yt::Innertube::HomeCursor cur;
    { std::lock_guard<std::mutex> lk(home_more_m_);
      batch = std::move(home_more_pending_); cur = std::move(home_more_cursor_pending_); }
    int tab = home_more_tab_;
    home_cursor_for(tab) = std::move(cur);   // advance the right cursor
    // Still on the home master view AND the same sub-tab the page was fetched for?
    if (!(query_.empty() && view_label_.empty() && !in_channel_view_)) return;
    if (home_tab_ != tab) return;
    if (batch.empty()) return;
    int base = (int)results_.size();
    if (tab == 3 || tab == 4) {
        filter_hidden(batch);                 // restricted only here (homogeneous feed)
        for (auto& v : batch) thumbs_.request(v.thumbnail_url);
        // Playlists / Posts: append into the tab's cache (so a round-trip keeps them)
        // and the visible list. No is_short filter — the feed is homogeneous.
        auto& cache = (tab == 3) ? home_posts_ : home_playlists_;
        for (auto& v : batch) { cache.push_back(v); results_.push_back(std::move(v)); }
    } else {
        // Append the raw page to the master (keeps Shorts so the Shorts tab still has
        // them), then add to the visible list respecting the tab + hide-shorts rule.
        size_t added = batch.size();
        home_items_.insert(home_items_.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
        std::vector<yt::SearchResult> add;
        for (size_t i = home_items_.size() - added; i < home_items_.size(); ++i) {
            const auto& r = home_items_[i];
            if (home_tab_ == 1 && (r.is_short || r.is_post())) continue;  // Videos only
            if (home_tab_ == 2 && !r.is_short) continue;                  // Shorts only
            add.push_back(r);
        }
        filter_hidden(add);                   // restricted + Shorts (only on the All tab)
        for (auto& v : add) { thumbs_.request(v.thumbnail_url); results_.push_back(std::move(v)); }
    }
    if ((int)results_.size() > base) { build_tile_lines(); queue_restricted_checks(); }
    if (getenv("YTC_DEBUG")) {
        const char* tag = tab == 3 ? ":posts" : tab == 4 ? ":pl" : "";
        std::fprintf(stderr, "[home-more%s] +%d shown -> %zu (%s)\n", tag,
                     (int)results_.size() - base, results_.size(),
                     home_cursor_for(tab).has_more() ? "more available" : "END");
    }
}

// ===================== Casting (Option B) =====================
void App::open_cast_picker() {
    cast_picker_mode_ = PickerMode::Cast;
    cast_picker_open_ = true; cast_sel_ = 0; cast_devices_.clear();
    if (cast_disc_running_) return;
    cast_disc_running_ = true; cast_disc_done_ = false;
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    cast_disc_thread_ = std::thread([this]() {
        std::vector<yt::Cast::Device> devs;
        try { devs = cast_.discover(3000); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_disc_m_); cast_disc_pending_ = std::move(devs); }
        cast_disc_running_ = false; cast_disc_done_ = true;
    });
}
void App::open_link_picker() {           // Linked Devices -> "Add a device": discover linkable TVs
    cast_picker_mode_ = PickerMode::Link;
    cast_picker_open_ = true; cast_sel_ = 0; cast_devices_.clear();
    if (cast_disc_running_) return;
    cast_disc_running_ = true; cast_disc_done_ = false;
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    cast_disc_thread_ = std::thread([this]() {
        std::vector<yt::Cast::Device> devs;
        try { devs = cast_.discover(3000); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_disc_m_); cast_disc_pending_ = std::move(devs); }
        cast_disc_running_ = false; cast_disc_done_ = true;
    });
}
void App::rebuild_picker_rows() {
    cast_devices_.clear();
    if (cast_picker_mode_ == PickerMode::Link) {
        // Only devices that can be code-linked: unpaired Cast/Android-TV devices.
        for (auto& d : cast_all_)
            if (d.kind == yt::Cast::Kind::CastDevice && d.screen_id.empty() && !d.ip.empty())
                cast_devices_.push_back(d);
        return;
    }
    for (auto& d : cast_all_) {
        if (d.kind == yt::Cast::Kind::DialYouTube) {
            cast_devices_.push_back(d);                 // smart TV: native, no code
        } else if (d.kind == yt::Cast::Kind::CastDevice) {
            // The native TV app plays a Short in its Shorts UI, which ignores the lounge
            // remote — so hide the linked (paired) row for Shorts. The web-receiver row
            // below plays it as a normal, controllable video.
            if (!d.screen_id.empty() && !cast_target_is_short_)
                cast_devices_.push_back(d);              // paired native row
            if (!d.ip.empty()) {                         // code-free "· Chromecast" row (web receiver)
                yt::Cast::Device cc = d; cc.screen_id.clear();
                cast_devices_.push_back(cc);
            }
        }
    }
}
void App::poll_cast_discovery() {
    if (!cast_disc_done_.exchange(false)) return;
    if (cast_disc_thread_.joinable()) cast_disc_thread_.join();
    { std::lock_guard<std::mutex> lk(cast_disc_m_); cast_all_ = std::move(cast_disc_pending_); }
    rebuild_picker_rows();
    int maxsel = (int)cast_devices_.size() + (cast_picker_mode_ == PickerMode::Cast ? 1 : 0) - 1;
    if (cast_sel_ > maxsel) cast_sel_ = std::max(0, maxsel);
}
void App::cast_activate() {
    int n = (int)cast_devices_.size();
    if (cast_picker_mode_ == PickerMode::Link) {   // pick a device to link -> code entry for it
        if (cast_sel_ < 0 || cast_sel_ >= n) return;
        cast_link_name_ = cast_devices_[cast_sel_].name;
        open_search();
        query_input_.clear(); kb_caret_ = 0; kb_row_ = 0; kb_col_ = 0;
        kb_mode_ = KbMode::LinkDevice;
        kb_title_ = i18n::tr(i18n::Str::CastCodeTitle);
        kb_placeholder_.clear();
        return;
    }
    (void)n; return cast_activate_cast();
}
void App::cast_activate_cast() {
    int n = (int)cast_devices_.size();
    if (cast_sel_ == n) {   // "Add a device" -> on-screen keyboard in code mode
        open_search();
        query_input_.clear(); kb_caret_ = 0; kb_row_ = 0; kb_col_ = 0;   // number row
        kb_mode_ = KbMode::CastCode;
        kb_title_ = i18n::tr(i18n::Str::CastCodeTitle);   // "Enter TV Code"
        kb_placeholder_.clear();                          // no hint in the box
        return;
    }
    if (cast_sel_ >= 0 && cast_sel_ < n) start_cast(cast_devices_[cast_sel_]);
}
double App::cast_est_pos() const {
    if (cast_ev_valid_) {   // real TV position + elapsed since it was reported (if playing)
        double e = cast_ev_pos_.load();
        if (cast_ev_state_.load() == 1) e += (SDL_GetTicks() - cast_ev_ts_.load()) / 1000.0;
        double dur = cast_ev_dur_.load();
        if (dur > 0 && e > dur) e = dur;
        return e < 0 ? 0 : e;
    }
    double e = cast_base_pos_;   // fallback before the first event arrives
    if (!cast_paused_) e += (SDL_GetTicks() - cast_started_ms_) / 1000.0;
    return e < 0 ? 0 : e;
}
void App::start_cast(const yt::Cast::Device& d) {
    if (cast_play_running_) return;
    cast_play_name_ = d.name.empty() ? "TV" : d.name;
    cast_play_running_ = true; cast_play_done_ = false;
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    yt::Cast::Device dev = d;
    std::string vid = cast_target_id_; int pos = cast_target_pos_;
    cast_play_thread_ = std::thread([this, dev, vid, pos]() {
        yt::Cast::Session s;
        try { s = cast_.play(dev, vid, pos); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_play_m_); cast_play_pending_ = s; }
        cast_play_running_ = false; cast_play_done_ = true;
    });
    status_msg_ = i18n::tr(i18n::Str::CastConnecting); status_until_ = SDL_GetTicks() + 9000;
}
void App::submit_cast_code(const std::string& code) {
    if (cast_play_running_ || code.empty()) return;
    // Name the pairing after the discovered device: if exactly one unpaired Cast
    // device is on the network, the code is almost certainly for it (e.g. "SHIELD").
    std::string name; int unpaired = 0;
    for (auto& d : cast_all_) if (d.needs_code()) { name = d.name; unpaired++; }
    if (unpaired != 1) name.clear();     // ambiguous -> fall back to the generic name
    cast_play_name_ = name.empty() ? "TV" : name;
    cast_play_running_ = true; cast_play_done_ = false;
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    std::string vid = cast_target_id_; int pos = cast_target_pos_; std::string nm = cast_play_name_;
    cast_play_thread_ = std::thread([this, code, vid, pos, nm]() {
        yt::Cast::Session s;
        try {
            std::string sid = cast_.pair_with_code(code, nm);
            if (!sid.empty()) {
                yt::Cast::Device d; d.screen_id = sid; d.name = nm; d.kind = yt::Cast::Kind::CastDevice;
                s = cast_.play(d, vid, pos);
            }
        } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_play_m_); cast_play_pending_ = s; }
        cast_play_running_ = false; cast_play_done_ = true;
    });
    status_msg_ = i18n::tr(i18n::Str::CastConnecting); status_until_ = SDL_GetTicks() + 9000;
}
void App::poll_cast_play() {
    if (!cast_play_done_.exchange(false)) return;
    if (cast_play_thread_.joinable()) cast_play_thread_.join();
    yt::Cast::Session s;
    { std::lock_guard<std::mutex> lk(cast_play_m_); s = cast_play_pending_; }
    if (s.ok) {
        cast_session_ = s; cast_name_ = cast_play_name_; casting_ = true;
        cast_paused_ = false; cast_vol_ = 100;
        cast_base_pos_ = cast_target_pos_; cast_started_ms_ = SDL_GetTicks();
        cast_picker_open_ = false;
        // Start the event-reader: long-poll the lounge backchannel for the TV's real
        // position (used for accurate seek). Runs on a copy of the session; RID=rpc so
        // it never touches the command rid counter.
        cast_ev_valid_ = false; cast_nowplaying_at_ = 0;
        if (cast_events_thread_.joinable()) cast_events_thread_.join();
        cast_events_run_ = true;
        yt::Cast::Session es = s;
        cast_events_thread_ = std::thread([this, es]() {
            int aid = -1;
            while (cast_events_run_) {
                yt::Cast::NowPlaying np;
                try { np = cast_.read_events(es, aid, 4); } catch (...) {}
                aid = np.aid;
                if (np.valid) {
                    cast_ev_pos_ = np.current_time; cast_ev_dur_ = np.duration;
                    cast_ev_state_ = np.state; cast_ev_ts_ = SDL_GetTicks();
                    cast_ev_valid_ = true;
                }
            }
        });
        // Option B: local playback stops; the handheld becomes the remote.
        if (mode_ == Mode::Playing) { save_resume_position(); player_.stop(); mode_ = Mode::Grid; }
        status_msg_.clear();
    } else {
        status_msg_ = i18n::tr(i18n::Str::CastFailed); status_until_ = SDL_GetTicks() + 4000;
    }
}
void App::cast_command(const std::string& type, double arg) {
    if (!casting_ || cast_cmd_running_) return;   // drop rapid extra presses (never block the UI)
    cast_cmd_running_ = true;
    if (cast_cmd_thread_.joinable()) cast_cmd_thread_.join();   // prior one already finished
    std::string ty = type; double ar = arg;
    cast_cmd_thread_ = std::thread([this, ty, ar]() {
        try { cast_.command(cast_session_, ty, ar); } catch (...) {}  // only the worker touches rid
        cast_cmd_running_ = false;
    });
}
void App::stop_casting() {
    casting_ = false;
    cast_events_run_ = false;
    if (cast_events_thread_.joinable()) cast_events_thread_.join();
    if (cast_cmd_thread_.joinable()) cast_cmd_thread_.join();   // don't free the session under it
    cast_session_ = {};
    cast_ev_valid_ = false;
}

// ---- Settings -> Linked Devices ----
void App::open_manage_devices() {
    cast_manage_open_ = true; cast_manage_sel_ = 0;
    cast_paired_ = cast_.paired();
}
void App::manage_activate() {
    int n = (int)cast_paired_.size();
    if (cast_manage_sel_ == n) {   // "+ Add a device" -> discovery picker of linkable TVs
        open_link_picker();
        return;
    }
    if (cast_manage_sel_ >= 0 && cast_manage_sel_ < n) {   // confirm before removing
        cast_confirm_remove_ = true; cast_confirm_sel_ = 0;   // default to "No"
    }
}
void App::confirm_remove_device() {
    int n = (int)cast_paired_.size();
    if (cast_manage_sel_ < 0 || cast_manage_sel_ >= n) return;
    std::string name = cast_paired_[cast_manage_sel_].name;
    cast_.forget(cast_paired_[cast_manage_sel_].screen_id);
    cast_paired_ = cast_.paired();
    if (cast_manage_sel_ >= (int)cast_paired_.size()) cast_manage_sel_ = std::max(0, (int)cast_paired_.size());
    status_msg_ = std::string(i18n::tr(i18n::Str::CastRemoved)) + ": " + name;
    status_until_ = SDL_GetTicks() + 2500;
}
void App::link_device(const std::string& code) {
    if (cast_link_running_ || code.empty()) return;
    cast_picker_open_ = false;              // done with the link picker -> back to the manage list
    cast_link_running_ = true; cast_link_done_ = false;
    if (cast_link_thread_.joinable()) cast_link_thread_.join();
    std::string nm = cast_link_name_;       // the device we chose to link
    cast_link_thread_ = std::thread([this, code, nm]() {
        std::string sid;
        try { sid = cast_.pair_with_code(code, nm); } catch (...) {}
        { std::lock_guard<std::mutex> lk(cast_link_m_); cast_link_result_ = sid; }
        cast_link_running_ = false; cast_link_done_ = true;
    });
    status_msg_ = i18n::tr(i18n::Str::CastConnecting); status_until_ = SDL_GetTicks() + 8000;
}
void App::poll_cast_link() {
    if (!cast_link_done_.exchange(false)) return;
    if (cast_link_thread_.joinable()) cast_link_thread_.join();
    std::string sid;
    { std::lock_guard<std::mutex> lk(cast_link_m_); sid = cast_link_result_; }
    if (sid.empty()) { status_msg_ = i18n::tr(i18n::Str::CastPairFailed); status_until_ = SDL_GetTicks() + 4000; }
    else status_msg_.clear();
    cast_paired_ = cast_.paired();   // refresh the manage list either way
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
    // Remote mode: the handheld controls the TV (playback continues there).
    if (casting_) {
        switch (a) {
            case Action::Select: {   // A: play/pause
                double at = cast_est_pos();
                cast_paused_ = !cast_paused_;
                cast_command(cast_paused_ ? "pause" : "play");
                // Optimistic local update; the next event corrects it.
                cast_ev_pos_ = at; cast_ev_ts_ = SDL_GetTicks();
                cast_ev_state_ = cast_paused_ ? 2 : 1; cast_ev_valid_ = true;
                cast_base_pos_ = at; cast_started_ms_ = SDL_GetTicks();
                break;
            }
            case Action::Left: case Action::Right: {   // seek +/- 10s (real position)
                double t = cast_est_pos() + (a == Action::Right ? 10 : -10); if (t < 0) t = 0;
                cast_command("seekTo", t);
                cast_ev_pos_ = t; cast_ev_ts_ = SDL_GetTicks(); cast_ev_valid_ = true;
                cast_base_pos_ = t; cast_started_ms_ = SDL_GetTicks();
                break;
            }
            case Action::Up: case Action::Down:   // volume
                cast_vol_ += (a == Action::Up ? 10 : -10);
                if (cast_vol_ > 100) cast_vol_ = 100; if (cast_vol_ < 0) cast_vol_ = 0;
                cast_command("setVolume", cast_vol_); break;
            case Action::Back:
                cast_command("stopVideo");   // stop playback on the TV (esp. the web receiver)
                stop_casting();              // joins the command worker, then leaves the remote
                break;
            default: break;
        }
        return;
    }
    // Device picker overlay (but not while the code keyboard is up over it).
    if (cast_picker_open_ && mode_ != Mode::Search) {
        // Cast mode has a trailing "Add a device" row; Link mode is devices only.
        int rows = (int)cast_devices_.size() + (cast_picker_mode_ == PickerMode::Cast ? 1 : 0);
        switch (a) {
            case Action::Up:     if (cast_sel_ > 0) cast_sel_--; break;
            case Action::Down:   if (cast_sel_ < rows - 1) cast_sel_++; break;
            case Action::Select: cast_activate(); break;
            case Action::Back:   cast_picker_open_ = false; break;   // Link mode: back to manage
            default: break;
        }
        return;
    }
    // "Remove Device?" confirmation prompt (topmost over the manage list).
    if (cast_confirm_remove_ && mode_ != Mode::Search) {
        switch (a) {
            case Action::Left: case Action::Right: cast_confirm_sel_ ^= 1; break;
            case Action::Select:
                if (cast_confirm_sel_ == 1) confirm_remove_device();
                cast_confirm_remove_ = false;
                break;
            case Action::Back:   cast_confirm_remove_ = false; break;  // cancel = No
            default: break;
        }
        return;
    }
    // Linked-devices management overlay (Settings -> Linked Devices).
    if (cast_manage_open_ && mode_ != Mode::Search) {
        int rows = (int)cast_paired_.size() + 1;    // + "Link a device"
        switch (a) {
            case Action::Up:     if (cast_manage_sel_ > 0) cast_manage_sel_--; break;
            case Action::Down:   if (cast_manage_sel_ < rows - 1) cast_manage_sel_++; break;
            case Action::Select: manage_activate(); break;
            case Action::Back:   cast_manage_open_ = false; open_settings(); break;  // back to Settings
            default: break;
        }
        return;
    }
    // Comments overlay consumes input while open (topmost). Up/Down move the selected
    // comment; A expands/collapses its replies; Left/Right jump; B closes.
    if (comments_open_) {
        int n = (int)comment_units_.size();
        switch (a) {
            case Action::Up:    if (comments_sel_ > 0) comments_sel_--; break;
            case Action::Down:  if (comments_sel_ + 1 < n) comments_sel_++; break;
            case Action::Left:  comments_sel_ = std::max(0, comments_sel_ - 6); break;
            case Action::Right: comments_sel_ = std::min(n - 1, comments_sel_ + 6); break;
            case Action::Select: toggle_comment_replies(); break;
            case Action::Sort:   toggle_comment_sort(); break;
            case Action::Back:
            case Action::Menu:  close_comments(); break;
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
        // Post overlay: selectable top buttons (video, comments) then the scrolling text.
        // Focus 0..n_top-1 = buttons in order [video, comments]; n_top = text scroll.
        bool has_comments_btn = desc_is_post_ && !desc_post_id_.empty();
        int n_top = (post_has_video_ ? 1 : 0) + (has_comments_btn ? 1 : 0);
        if (n_top > 0) {
            int vid_idx = post_has_video_ ? 0 : -1;
            int com_idx = has_comments_btn ? (post_has_video_ ? 1 : 0) : -1;
            int text_focus = n_top;
            switch (a) {
                case Action::Up:
                    if (post_focus_ == text_focus) {
                        if (desc_scroll_ > 0) desc_scroll_ -= step; else post_focus_ = n_top - 1;
                    } else if (post_focus_ > 0) post_focus_--;
                    break;
                case Action::Down:
                    if (post_focus_ < text_focus) post_focus_++; else desc_scroll_ += step;
                    break;
                case Action::Select:
                    if (post_focus_ == vid_idx) {          // play the attached video
                        close_desc();
                        request_playback();
                    } else if (post_focus_ == com_idx) {   // open this post's comments
                        yt::SearchResult tmp;
                        tmp.kind = yt::SearchResult::Kind::Post; tmp.post_id = desc_post_id_;
                        tmp.channel_id = desc_post_channel_;
                        comments_paused_ = false;          // the post overlay keeps the pause
                        open_comments(tmp);
                    }
                    break;
                case Action::Back:
                    // From the text: first B returns to the buttons; from a button: close.
                    if (post_focus_ == text_focus) { post_focus_ = n_top - 1; desc_scroll_ = 0; }
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
                    act == MenuAction::ToggleHideRestricted ||
                    act == MenuAction::ToggleAskResume || act == MenuAction::CycleView ||
                    act == MenuAction::CycleVolume || act == MenuAction::CycleHwdec ||
                    act == MenuAction::CycleAspect || act == MenuAction::CycleAudioLang ||
                    act == MenuAction::CycleCaptionLang || act == MenuAction::CycleAudioTrack ||
                    act == MenuAction::CycleCaptionSize || act == MenuAction::CycleCaptionStyle ||
                    act == MenuAction::CycleSpeed || act == MenuAction::ToggleSponsorBlock ||
                    act == MenuAction::CycleCaptions || act == MenuAction::ToggleAutoplay ||
                    act == MenuAction::ToggleFeedFavorites || act == MenuAction::ToggleFeedHistory ||
                    act == MenuAction::ToggleFeedCustom || act == MenuAction::CycleLanguage ||
                    act == MenuAction::CycleFilterType || act == MenuAction::CycleFilterDuration ||
                    act == MenuAction::CycleFilterDate || act == MenuAction::CycleFilterSort)
                    adjust_setting(act, a == Action::Right ? +1 : -1);
                break;
            }
            case Action::Select:
                // The Search Filters rows are cycled with Left/Right; A does nothing
                // (without this it would fall through and close the modal).
                if (menu_kind_ != MenuKind::SearchFilters) menu_activate();
                break;
            case Action::Back:
            case Action::Menu:
                if (menu_kind_ == MenuKind::FeedRemoveConfirm) {  // cancel -> the list
                    feed_remove_idx_ = -1; open_feed_manage();
                    break;
                }
                if (menu_kind_ == MenuKind::FeedManage) {         // list -> Browsing
                    open_settings_browsing();
                    break;
                }
                if (menu_kind_ == MenuKind::SettingsHomeFeed) {   // nested under Browsing
                    open_settings_browsing();
                    break;
                }
                if (settings_kind(menu_kind_) && menu_kind_ != MenuKind::Settings) {
                    open_settings();                              // submenu -> Settings
                    break;
                }
                if (menu_kind_ == MenuKind::Settings) {   // Settings -> back to main menu
                    menu_open_ = false; open_main_menu();
                    break;
                }
                if (menu_kind_ == MenuKind::SearchFilters) {   // close filters; re-search if changed
                    menu_open_ = false;
                    bool changed = filt_type_ != filt_snapshot_[0] || filt_duration_ != filt_snapshot_[1]
                                || filt_date_ != filt_snapshot_[2] || filt_sort_ != filt_snapshot_[3];
                    if (changed) run_search();
                    break;
                }
                // Close the options menu. If the quality or audio track changed while
                // playing, re-resolve the current video and resume the position.
                menu_open_ = false;
                if ((quality_dirty_ || audio_dirty_) && mode_ == Mode::Playing) {
                    quality_dirty_ = false; audio_dirty_ = false; menu_paused_ = false;
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
        if (a == Action::Back) { mode_ = Mode::Grid; status_msg_ = i18n::tr(i18n::Str::Cancelled); }
        return;
    }
    if (mode_ == Mode::Search) {
        const auto& kb = kb_numeric() ? KB_NUM : KB;
        // Rows have different key counts/spans, so vertical moves are SPATIAL: keep the
        // horizontal center and land on whichever key in the target row spans it (a key
        // above the space bar lands on space, above Enter lands on Enter, etc.).
        auto xcenter = [&](int row, int col) {
            float x = 0; for (int j = 0; j < col; ++j) x += kb[row][j].span;
            return x + kb[row][col].span / 2.f;
        };
        auto col_at = [&](int row, float x) {
            float acc = 0;
            for (int j = 0; j < (int)kb[row].size(); ++j) {
                acc += kb[row][j].span;
                if (x < acc) return j;
            }
            return (int)kb[row].size() - 1;
        };
        switch (a) {
            case Action::Left:  kb_col_ = (kb_col_ > 0) ? kb_col_ - 1 : (int)kb[kb_row_].size()-1; break;
            case Action::Right: kb_col_ = (kb_col_ < (int)kb[kb_row_].size()-1) ? kb_col_ + 1 : 0; break;
            case Action::Up:    if (kb_row_ > 0) { float c = xcenter(kb_row_, kb_col_);
                                                   kb_col_ = col_at(--kb_row_, c); } break;
            case Action::Down:  if (kb_row_ < (int)kb.size()-1) { float c = xcenter(kb_row_, kb_col_);
                                                   kb_col_ = col_at(++kb_row_, c); } break;
            case Action::Select: kb_activate(); break;
            case Action::Sort:   query_input_.clear(); kb_caret_ = 0; break;   // X clears
            case Action::Search: submit_search(); break;      // Y submits
            case Action::Back:   mode_ = Mode::Grid;           // cancel; code entry -> picker
                                 kb_mode_ = KbMode::Search; break;
            default: break;
        }
        if (kb_col_ >= (int)kb[kb_row_].size()) kb_col_ = kb[kb_row_].size()-1;
        return;
    }
    // Grid / carousel browse. Any user input here cancels a pending autoplay countdown.
    cancel_autoplay();
    if (a == Action::Search) { open_search(); return; }
    if (a == Action::Sort && !query_.empty()) { toggle_search_sort(); return; }  // X: sort search
    if (a == Action::Menu) { open_menu(); return; }   // options for the highlighted item
    if (a == Action::Back) {
        // Stage 1: B jumps to the top of the list (first tile selected).
        if (!results_.empty() && sel_ != 0) {
            sel_ = 0; scroll_ = 0; carousel_pos_ = 0; ensure_visible();
            return;
        }
        // Stage 2: at the top of a tabbed view, a non-All tab returns to the All tab
        // (Home and channel pages both). The All tab then does the view's normal back.
        if (home_tabs_active() && home_tab_ != 0) { load_home_tab(0); return; }
        if (channel_tabs_active() && chan_tab_ != 0) { load_channel_tab(0); return; }
        if (search_tabs_active() && search_tab_ != 0) { load_search_tab(0); return; }
        // Stage 3: normal back — unwind a subview (channel/playlist -> where it was
        // opened from), else a top-level view that isn't Home falls back to Home.
        // On Home itself, Back does nothing (never quits).
        if (pop_view()) return;
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
        case Action::Down:
            if (sel_ + cols_ < n) sel_ += cols_;                    // tile directly below
            else if (sel_ / cols_ < (n - 1) / cols_) sel_ = n - 1;  // partial last row: fall to the last tile
            break;
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
    poll_more_home();
    poll_cast_discovery();
    poll_cast_play();
    poll_cast_link();
    // Periodically ask the TV for its real position; the reply lands on the event
    // backchannel and refreshes cast_ev_pos_ for accurate seek.
    if (casting_ && SDL_GetTicks() - cast_nowplaying_at_ > 3000) {
        cast_command("getNowPlaying");
        cast_nowplaying_at_ = SDL_GetTicks();
    }
    poll_refresh();
    poll_description();
    poll_comments();
    poll_download();
    poll_sponsorblock();
    poll_captions();
    poll_caption_download();
    // A caption fetch failed earlier: surface it now that the menu is down.
    if (cc_fail_pending_ && !menu_open_) {
        cc_fail_pending_ = false;
        status_msg_ = i18n::tr(i18n::Str::CcUnavailable);
        status_until_ = SDL_GetTicks() + 4000;
    }
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
                status_msg_ = std::string(i18n::tr(i18n::Str::SkippedPrefix)) + " " + sb_category_label(sg.category);
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
    maybe_load_more_home();
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
                        status_msg_ = i18n::tr(i18n::Str::SeekLimited);
                        status_until_ = SDL_GetTicks() + 4000;
                    }
                } else if (delta < 0) {
                    double floor_t = p - 60.0;                     // ~back-buffer span
                    if (floor_t < 0) floor_t = 0;
                    if (target < floor_t) {
                        delta = floor_t - p;
                        status_msg_ = i18n::tr(i18n::Str::SeekLimited);
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
        int t = home_tab_ + dir; if (t < 0 || t > 4) return;
        load_home_tab(t);
    } else if (search_tabs_active()) {
        int t = search_tab_ + dir; if (t < 0 || t > 4) return;   // All/Videos/Shorts/Channels/Playlists
        load_search_tab(t);
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
    if (comments_open_) render_comments(rn);  // comments float above everything
    if (resume_prompt_open_) render_resume_prompt(rn);
    if (casting_) render_remote(rn);
    else if (cast_picker_open_ && mode_ != Mode::Search) render_cast_picker(rn);
    else if (cast_manage_open_ && mode_ != Mode::Search) render_manage_devices(rn);
}

void App::render_menu(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.72f));
    int n = (int)menu_items_.size();
    // Settings rows carry a "Label:  Value" so they need more room than the
    // context menu; widen that panel to keep values from crowding the edge.
    float iw = std::min((settings_kind(menu_kind_) ||
                         menu_kind_ == MenuKind::FeedManage ||
                         menu_kind_ == MenuKind::SearchFilters ? 620.f : 560.f)*s, W*0.86f);
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
                        : (menu_kind_ == MenuKind::SettingsAudio)
                          ? std::string("Settings \xE2\x80\xA3 ") + i18n::tr(i18n::Str::SetAudioMenu)
                        : (menu_kind_ == MenuKind::SettingsVideo)
                          ? std::string("Settings \xE2\x80\xA3 ") + i18n::tr(i18n::Str::SetVideoMenu)
                        : (menu_kind_ == MenuKind::SettingsCaptions)
                          ? std::string("Settings \xE2\x80\xA3 ") + i18n::tr(i18n::Str::SetCaptionsMenu)
                        : (menu_kind_ == MenuKind::SettingsPlayback)
                          ? std::string("Settings \xE2\x80\xA3 ") + i18n::tr(i18n::Str::SetPlaybackMenu)
                        : (menu_kind_ == MenuKind::SettingsBrowsing)
                          ? std::string("Settings \xE2\x80\xA3 ") + i18n::tr(i18n::Str::SetBrowsingMenu)
                        : (menu_kind_ == MenuKind::SettingsHomeFeed)
                          ? std::string("Settings \xE2\x80\xA3 ") + i18n::tr(i18n::Str::SetHomeFeed)
                        : (menu_kind_ == MenuKind::FeedManage)
                          ? std::string(i18n::tr(i18n::Str::SetCustomFeed))
                        : (menu_kind_ == MenuKind::FeedRemoveConfirm)
                          ? std::string(i18n::tr(i18n::Str::FeedRemoveConfirm))
                        : (menu_kind_ == MenuKind::SearchFilters) ? i18n::tr(i18n::Str::MenuSearchFilters)
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
            it.action == MenuAction::ToggleHideRestricted ||
            it.action == MenuAction::ToggleAskResume || it.action == MenuAction::CycleView ||
            it.action == MenuAction::CycleVolume || it.action == MenuAction::CycleHwdec ||
            it.action == MenuAction::CycleAspect || it.action == MenuAction::CycleAudioLang ||
            it.action == MenuAction::CycleCaptionLang || it.action == MenuAction::CycleAudioTrack)
            has_value = true;
    const char* foot = (menu_kind_ == MenuKind::FeedManage) ? i18n::tr(i18n::Str::FooterManage)
                     : (menu_kind_ == MenuKind::FeedRemoveConfirm) ? i18n::tr(i18n::Str::FooterMenuPlain)
                     : (settings_kind(menu_kind_) || menu_kind_ == MenuKind::SearchFilters)
                     ? i18n::tr(i18n::Str::FooterDesc)
                     : has_value ? i18n::tr(i18n::Str::FooterMenuValue)
                     : i18n::tr(i18n::Str::FooterMenuPlain);
    rn.text(*font_small_, foot, px, iy + 10*s, theme_.text_dim);
    rn.end();
}

void App::render_cast_picker(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    bool link = (cast_picker_mode_ == PickerMode::Link);
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.72f));
    int addrow = link ? 0 : 1;               // Cast mode has a trailing "Add a device"
    int n = (int)cast_devices_.size() + addrow;
    float iw = std::min(600.f*s, W*0.86f), ih = 58*s, gap = 8*s, title_h = 56*s, foot_h = 34*s;
    float ph = title_h + std::max(1,n)*(ih+gap) + foot_h;
    float px = (W-iw)/2, py = (H-ph)/2;
    rn.quad({px-24*s, py-24*s, iw+48*s, ph+48*s}, theme_.panel);
    rn.quad({px-24*s, py-24*s, iw+48*s, 4*s}, theme_.accent);
    rn.text(*font_body_, i18n::tr(link ? i18n::Str::CastAddDevice : i18n::Str::MenuCastToDevice),
            px, py, theme_.text_dim);
    float iy = py + title_h;
    for (int i = 0; i < n; ++i) {
        bool sel = (i == cast_sel_);
        rn.quad({px, iy, iw, ih}, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({px, iy, 4*s, ih}, theme_.accent);
        std::string label;
        if (!link && i == (int)cast_devices_.size())
            label = std::string("+  ") + i18n::tr(i18n::Str::CastAddDevice);
        else {
            const auto& d = cast_devices_[i];
            label = d.name;
            if (!link && d.kind == yt::Cast::Kind::CastDevice && d.screen_id.empty())
                label += "  \xC2\xB7  Chromecast";   // code-free web-receiver row
        }
        rn.text(*font_body_, font_body_->ellipsize(label, iw - 36*s),
                px + 18*s, iy + (ih-font_body_->line_height())/2 + 3*s,
                sel ? theme_.text : theme_.text_dim);
        iy += ih + gap;
    }
    // Status line: searching / none / connecting.
    const char* note = cast_disc_running_ ? i18n::tr(i18n::Str::CastSearching)
                     : cast_play_running_ ? i18n::tr(i18n::Str::CastConnecting)
                     : cast_devices_.empty() ? i18n::tr(i18n::Str::CastNoDevices) : "";
    if (note[0]) rn.text(*font_small_, note, px, py + title_h - 26*s, theme_.text_dim);
    rn.text(*font_small_, i18n::tr(i18n::Str::FooterCastPicker), px, iy + 10*s, theme_.text_dim);
    rn.end();
}

void App::render_manage_devices(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.72f));
    int n = (int)cast_paired_.size() + 1;   // + "Link a device"
    float iw = std::min(600.f*s, W*0.86f), ih = 58*s, gap = 8*s, title_h = 56*s, foot_h = 34*s;
    float ph = title_h + n*(ih+gap) + foot_h;
    float px = (W-iw)/2, py = (H-ph)/2;
    rn.quad({px-24*s, py-24*s, iw+48*s, ph+48*s}, theme_.panel);
    rn.quad({px-24*s, py-24*s, iw+48*s, 4*s}, theme_.accent);
    rn.text(*font_body_, i18n::tr(i18n::Str::SetLinkedDevices), px, py, theme_.text_dim);
    float iy = py + title_h;
    for (int i = 0; i < n; ++i) {
        bool sel = (i == cast_manage_sel_);
        rn.quad({px, iy, iw, ih}, sel ? theme_.card_sel : theme_.card);
        if (sel) rn.quad({px, iy, 4*s, ih}, theme_.accent);
        std::string label = (i == (int)cast_paired_.size())
            ? std::string("+  ") + i18n::tr(i18n::Str::CastAddDevice)
            : cast_paired_[i].name;
        rn.text(*font_body_, font_body_->ellipsize(label, iw - 36*s),
                px + 18*s, iy + (ih-font_body_->line_height())/2 + 3*s,
                sel ? theme_.text : theme_.text_dim);
        iy += ih + gap;
    }
    if (cast_paired_.empty())
        rn.text(*font_small_, i18n::tr(i18n::Str::CastNoLinked), px, py + title_h - 26*s, theme_.text_dim);
    // Footer: the "Link a device" row can't be removed, so show the right hint.
    const char* foot = (cast_manage_sel_ == (int)cast_paired_.size())
                     ? i18n::tr(i18n::Str::FooterCastPicker)   // A: select   B: cancel
                     : i18n::tr(i18n::Str::FooterManage);      // A: remove   B: back
    rn.text(*font_small_, foot, px, iy + 10*s, theme_.text_dim);

    // "Remove Device?" yes/no dialog on top of the list.
    if (cast_confirm_remove_ &&
        cast_manage_sel_ >= 0 && cast_manage_sel_ < (int)cast_paired_.size()) {
        rn.quad({0, 0, (float)W, (float)H}, theme_.bg.with_a(0.6f));
        float dw = std::min(460.f*s, W*0.82f), dh = 210*s;
        float dx = (W-dw)/2, dy = (H-dh)/2;
        rn.quad({dx-20*s, dy-20*s, dw+40*s, dh+40*s}, theme_.panel);
        rn.quad({dx-20*s, dy-20*s, dw+40*s, 4*s}, theme_.accent);
        rn.text(*font_body_, i18n::tr(i18n::Str::CastRemoveConfirm), dx, dy, theme_.text);
        std::string dname = font_body_->ellipsize(cast_paired_[cast_manage_sel_].name, dw);
        rn.text(*font_small_, dname, dx, dy + 40*s, theme_.text_dim);
        // No / Yes buttons.
        float bw = (dw - 16*s)/2, bh = 52*s, by = dy + dh - bh - 34*s;
        const i18n::Str labels[2] = { i18n::Str::No, i18n::Str::Yes };
        for (int i = 0; i < 2; ++i) {
            bool sel = (i == cast_confirm_sel_);
            float bx = dx + i*(bw + 16*s);
            rn.quad({bx, by, bw, bh}, sel ? theme_.card_sel : theme_.card);
            if (sel) rn.quad({bx, by, bw, 3*s}, theme_.accent);
            const char* lbl = i18n::tr(labels[i]);
            float tw = font_body_->text_width(lbl);
            rn.text(*font_body_, lbl, bx + (bw-tw)/2, by + (bh-font_body_->line_height())/2 + 3*s,
                    sel ? theme_.text : theme_.text_dim);
        }
        rn.text(*font_small_, i18n::tr(i18n::Str::FooterConfirm),
                dx, by + bh + 12*s, theme_.text_dim);
    }
    rn.end();
}

void App::render_remote(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);
    // "Casting to <TV>"
    std::string to = std::string(i18n::tr(i18n::Str::CastingTo)) + "  " + cast_name_;
    rn.text(*font_body_, to, (W - font_body_->text_width(to))/2, H*0.24f, theme_.text_dim);
    // Title of what's playing.
    std::string t = font_title_->ellipsize(cast_target_title_, W*0.86f);
    rn.text(*font_title_, t, (W - font_title_->text_width(t))/2, H*0.32f, theme_.text);
    // Big play/pause glyph.
    float cx = W/2.f, cy = H*0.52f;
    gfx::Color ac = theme_.accent;
    if (cast_paused_) {   // paused -> show a play triangle (degenerate quad)
        float r = 34*s;
        rn.quad4(cx - r*0.5f, cy - r,  cx + r, cy,  cx - r*0.5f, cy + r,  cx - r*0.5f, cy - r, ac);
    } else {              // playing -> pause bars
        float bw = 14*s, bh = 54*s, gp = 16*s;
        rn.quad({cx - gp/2 - bw, cy - bh/2, bw, bh}, ac);
        rn.quad({cx + gp/2,      cy - bh/2, bw, bh}, ac);
    }
    // Position (estimated) + volume.
    auto tstr = [](double v){ int m=(int)v/60, sec=(int)v%60; char b[16]; std::snprintf(b,sizeof b,"%d:%02d",m,sec); return std::string(b); };
    std::string pos = tstr(cast_est_pos());
    rn.text(*font_body_, pos, (W - font_body_->text_width(pos))/2, H*0.62f, theme_.text_dim);
    char vol[24]; std::snprintf(vol, sizeof vol, "Vol %d%%", cast_vol_);
    rn.text(*font_small_, vol, (W - font_small_->text_width(vol))/2, H*0.68f, theme_.text_dim);
    // Footer hints.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    const char* foot = i18n::tr(i18n::Str::FooterRemote);
    rn.text(*font_small_, foot, (W - font_small_->text_width(foot))/2, H - fh + 12*s, theme_.text_dim);
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
        else rn.text(*font_small_, i18n::tr(i18n::Str::Loading), r.x + 12*s, r.y + r.h/2 - 9*s, A(theme_.text_dim));
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
        t.l2 = v.author;                             // channel name
        std::string type = v.video_id.empty() ? i18n::tr(i18n::Str::TilePost)
             : std::string(i18n::tr(i18n::Str::TilePost)) + "   -   " + i18n::tr(i18n::Str::TileVideo);
        std::string meta = v.view_count_text;        // "N likes"
        if (!v.published_text.empty()) meta += (meta.empty()?"":"   -   ") + v.published_text;
        t.l3 = meta.empty() ? type : (type + "   -   " + meta);
    } else if (v.is_playlist()) {
        t.l1 = v.title; t.l2 = v.author; t.l3 = i18n::tr(i18n::Str::TilePlaylist);
    } else {
        // Video / Short: lead the footer with the type (so it's consistent with the
        // Playlist/Post tiles and distinguishes a Short from a Video with the same
        // title/thumbnail), then the view count + age.
        t.l1 = v.title; t.l2 = v.author;
        std::string age = humanize_age(v.published_text);
        std::string meta = v.view_count_text;
        if (!age.empty()) meta += (meta.empty()?"":"   -   ") + age;
        std::string type = i18n::tr(v.is_live ? i18n::Str::TileLive
                                  : v.is_short ? i18n::Str::TileShort : i18n::Str::TileVideo);
        t.l3 = meta.empty() ? type : (type + "   -   " + meta);
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
                    float x, float y, float maxw, float s, float alpha, bool center) {
    gfx::Color tc = theme_.text.with_a(alpha), dc = theme_.text_dim.with_a(alpha);
    TileLines local;
    const TileLines* t;
    if (idx >= 0 && idx < (int)tile_lines_.size()) t = &tile_lines_[idx];
    else { local = compose_lines(v, chan_meta_); t = &local; }   // fallback
    // Channel video-count may arrive after the cache was built — refresh l3 live.
    std::string chan_l3;
    if (v.is_channel()) { chan_l3 = chan_meta_.video_count(v.channel_id);
        if (chan_l3.empty()) chan_l3 = t->l3; }
    // Search shorts have no uploader name in the response; fill l2 once resolved.
    std::string short_l2;
    if (v.is_short && t->l2.empty() && !v.channel_id.empty())
        short_l2 = chan_meta_.name(v.channel_id);
    const std::string& l1 = t->l1;
    const std::string& l2 = short_l2.empty() ? t->l2 : short_l2;
    const std::string& l3 = v.is_channel() ? chan_l3 : t->l3;
    float lh1 = font_body_->line_height(), lh2 = font_small_->line_height();
    float y2 = y + lh1 + 4*s, y3 = y2 + lh2 + 3*s;
    // Emit without copying when the line already fits (ellipsize() copies unconditionally).
    // center: center each line within [x, x+maxw] (carousel); else left-align (grid).
    auto emit = [&](gfx::Font& f, const std::string& raw, float yy, gfx::Color c) {
        if (raw.empty()) return;
        const std::string& shown = f.text_width(raw) <= maxw ? raw : f.ellipsize(raw, maxw);
        float lx = center ? x + (maxw - f.text_width(shown)) / 2 : x;
        rn.text(f, shown, lx, yy, c);
    };
    emit(*font_body_,  l1, y,  tc);
    emit(*font_small_, l2, y2, dc);
    emit(*font_small_, l3, y3, dc);
}

// Header bar (title + subtitle + count) and the channel/Home tab strip, drawn at
// vertical offset hy (grid passes -scroll_ so it slides away; carousel/coverflow
// pass 0 so it stays pinned). Shared by all browse views.
// view_label_ is an internal English identity key; translate it only for display.
static std::string tr_view_label(const std::string& label) {
    using S = i18n::Str;
    if (label == "Favorite Channels")  return i18n::tr(S::FavoriteChannels);
    if (label == "Favorite Playlists") return i18n::tr(S::FavoritePlaylists);
    if (label == "Watch Later")        return i18n::tr(S::WatchLater);
    if (label == "History")           return i18n::tr(S::History);
    return label;
}
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
    } else if (!view_label_.empty()) sub = tr_view_label(view_label_);
    else if (query_.empty())         sub = i18n::tr(i18n::Str::Home);
    else                             sub = std::string(i18n::tr(i18n::Str::Search)) + ": " + query_;
    // Right-aligned count/status; measure it first so the subtitle can be ellipsized
    // to the exact gap and never slides under it (the count grows with "loading more...").
    std::string count = std::to_string(results_.size()) + " " + i18n::tr(i18n::Str::Results);
    if (!query_.empty() && !in_channel_view_ && !results_.empty())   // search sort mode
        count += std::string("  -  ") +
                 i18n::tr(search_sort_ == 1 ? i18n::Str::SortDate
                        : search_sort_ == 2 ? i18n::Str::SortPopular : i18n::Str::SortRelevance);
    if (more_running_ || home_more_running_) count += std::string("  -  ") + i18n::tr(i18n::Str::Loading);
    if (refresh_running_) count = i18n::tr(i18n::Str::Loading);
    float count_w = font_small_->text_width(count);
    rn.text(*font_small_, count, W - count_w - 32*s, hy+34*s, theme_.text_dim);
    // Subtitle starts clear of the logo (its real width), ends clear of the count.
    float sub_x = std::max(240*s, 32*s + font_title_->text_width("YTC") + 28*s);
    float sub_w = (W - count_w - 32*s - 20*s) - sub_x;
    if (sub_w > 40*s)
        rn.text(*font_body_, font_body_->ellipsize(sub, sub_w), sub_x, hy+32*s, theme_.text_dim);

    if (channel_tabs_active() || home_tabs_active() || search_tabs_active()) {
        int active_tab = channel_tabs_active() ? chan_tab_
                       : search_tabs_active()  ? search_tab_ : home_tab_;
        GridMetrics tm = grid_metrics();
        float ty0 = hy + hbar;
        rn.quad({0, ty0, (float)W, tm.tabs_h}, theme_.panel.with_a(0.6f));
        using S = i18n::Str;
        // Search omits the Posts tab (YouTube search returns no posts) but adds Channels.
        const S kTabs[]       = {S::TabAll, S::TabVideos, S::TabShorts, S::TabPosts, S::TabPlaylists};
        const S kSearchTabs[] = {S::TabAll, S::TabVideos, S::TabShorts, S::FilterChannels, S::TabPlaylists};
        const S* tabs = search_tabs_active() ? kSearchTabs : kTabs;
        int ntabs = 5;
        float tx = 32 * s;
        for (int i = 0; i < ntabs; ++i) {
            const char* label = i18n::tr(tabs[i]);
            float tw = font_body_->text_width(label);
            float chip_w = tw + 36 * s, chip_h = tm.tabs_h - 12 * s;
            gfx::Rect chip{tx, ty0 + 6 * s, chip_w, chip_h};
            bool st = (i == active_tab);
            // Active tab is always the red accent (via D-pad on the strip OR the L/R
            // shoulders) — no focus outline.
            if (st) rn.quad(chip, theme_.accent);
            rn.text(*font_body_, label, tx + 18 * s,
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
        using S = i18n::Str;
        const char* l1; const char* l2 = i18n::tr(S::SearchHint); const char* l3;
        if (retry_pending_ || (refresh_running_ && retry_attempt_ > 0)) {
            l1 = i18n::tr(S::WaitingNetwork);
            l2 = i18n::tr(S::Reconnecting);
            l3 = "";
        } else if (channel_tabs_active() ||
            (home_tabs_active() && !home_items_.empty()) ||
            (home_tabs_active() && refresh_running_) ||
            (search_tabs_active() && !search_base_.empty())) {   // a search tab with no items of that type
            l1 = refresh_running_ ? i18n::tr(S::Loading) : i18n::tr(S::NothingInTab);
            l2 = refresh_running_ ? "" : i18n::tr(S::SwitchTabsHint);
            l3 = "";
        } else if (view_label_ == "Favorite Channels") {
            l1 = i18n::tr(S::NoFavorites);
            l2 = i18n::tr(S::FavHint2);
            l3 = i18n::tr(S::FavHint3);
        } else if (view_label_ == "Favorite Playlists") {
            l1 = i18n::tr(S::NoFavPlaylists);
            l2 = i18n::tr(S::FavPlHint2);
            l3 = "";
        } else if (view_label_ == "Watch Later") {
            l1 = i18n::tr(S::WatchLaterEmpty);
            l2 = i18n::tr(S::WlHint2);
            l3 = i18n::tr(S::WlHint3);
        } else if (view_label_ == "History") {
            l1 = i18n::tr(S::NoHistory);
            l2 = i18n::tr(S::HistHint2);
            l3 = i18n::tr(S::HistHint3);
        } else {
            l1 = query_.empty() ? i18n::tr(S::NoVideosYet) : i18n::tr(S::NoResults);
            l3 = query_.empty() ? i18n::tr(S::HomeHint3) : i18n::tr(S::SearchHint3);
        }
        rn.text(*font_title_, l1, (W - font_title_->text_width(l1))/2, H*0.40f, theme_.text);
        rn.text(*font_body_,  l2, (W - font_body_->text_width(l2))/2,  H*0.40f + 52*s, theme_.accent);
        rn.text(*font_small_, l3, (W - font_small_->text_width(l3))/2, H*0.40f + 92*s, theme_.text_dim);
        // Footer hints still useful. On a search with no results, surface Select:
        // filters so a too-narrow filter set can still be reached and loosened.
        float fh0 = 44*s;
        rn.quad({0, H-fh0, (float)W, fh0}, theme_.panel);
        rn.text(*font_small_, i18n::tr(search_tabs_active() ? i18n::Str::FooterSearchEmpty
                                                            : i18n::Str::FooterHomeMin),
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
    rn.text(*font_small_, browse_footer(),
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
    const char* l1 = i18n::tr(i18n::Str::NoVideosYet);
    if (retry_pending_ || (refresh_running_ && retry_attempt_ > 0)) l1 = i18n::tr(i18n::Str::WaitingNetwork);
    else if (refresh_running_) l1 = i18n::tr(i18n::Str::Loading);
    else if (!query_.empty()) l1 = i18n::tr(i18n::Str::NoResults);
    else if (view_label_ == "Watch Later") l1 = i18n::tr(i18n::Str::WatchLaterEmpty);
    else if (view_label_ == "History") l1 = i18n::tr(i18n::Str::NoHistory);
    else if (view_label_ == "Favorite Channels") l1 = i18n::tr(i18n::Str::NoFavorites);
    else if (view_label_ == "Favorite Playlists") l1 = i18n::tr(i18n::Str::NoFavPlaylists);
    rn.text(*font_title_, l1, (W - font_title_->text_width(l1))/2, H*0.45f, theme_.text);
    // Match the grid's empty-state: pinned footer bar with the minimal hints.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, i18n::tr(i18n::Str::FooterHomeMin), 32*s, H - fh + 12*s, theme_.text_dim);
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

    float top = hbar + (channel_tabs_active() || home_tabs_active() || search_tabs_active() ? 52*s : 0);
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
    draw_meta(rn, v, sel_, mx, my, mw, s, 1.0f, true);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, my + 82*s, theme_.text_dim);

    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, browse_footer(),
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

    float top = hbar + (channel_tabs_active() || home_tabs_active() || search_tabs_active() ? 52*s : 0);
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
    draw_meta(rn, results_[sel_], sel_, mx, my, mw, s, 1.0f, true);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, my + 82*s, theme_.text_dim);

    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, browse_footer(),
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

    float top = hbar + (channel_tabs_active() || home_tabs_active() || search_tabs_active() ? 52*s : 0);
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
    draw_meta(rn, results_[sel_], sel_, mx, my, mw, s, 1.0f, true);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, my + 82*s, theme_.text_dim);

    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, browse_footer(),
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
        const char* hint = i18n::tr(player_.paused() ? i18n::Str::FooterPlayerPause
                                                     : i18n::Str::FooterPlayerPlay);
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
    kb_mode_ = KbMode::Search;
    kb_title_ = i18n::tr(i18n::Str::Search);
    kb_placeholder_ = i18n::tr(i18n::Str::TypeToSearch);
    query_input_ = query_;      // seed with the current query for quick edits
    kb_caret_ = (int)query_input_.size();
    kb_shift_ = false;
    kb_row_ = 1; kb_col_ = 0;   // start on 'q'
    status_msg_.clear();
}
void App::input_text(const std::string& s) {
    for (char c : s) if (c >= 32 && c <= 126) {   // ASCII-only atlas
        query_input_.insert(query_input_.begin() + kb_caret_, c);
        kb_caret_++;
    }
}
void App::backspace() {
    if (kb_caret_ > 0) { query_input_.erase(query_input_.begin() + (kb_caret_-1)); kb_caret_--; }
}
void App::test_open_numeric_kb() {
    open_search();
    query_input_.clear(); kb_caret_ = 0; kb_row_ = 0; kb_col_ = 0;
    kb_mode_ = KbMode::CastCode;
    kb_title_ = i18n::tr(i18n::Str::CastCodeTitle);
    kb_placeholder_.clear();
}
void App::kb_activate() {
    const auto& kb = kb_numeric() ? KB_NUM : KB;
    const Key& k = kb[kb_row_][kb_col_];
    switch (k.type) {
        case KCHAR: {
            char c = k.ch;
            if (kb_shift_ && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            query_input_.insert(query_input_.begin() + kb_caret_, c);
            kb_caret_++;
            kb_shift_ = false;                       // one-shot capital
            break;
        }
        case KSPACE:  query_input_.insert(query_input_.begin() + kb_caret_, ' '); kb_caret_++; break;
        case KDEL:    backspace(); break;
        case KLEFT:   if (kb_caret_ > 0) kb_caret_--; break;
        case KRIGHT:  if (kb_caret_ < (int)query_input_.size()) kb_caret_++; break;
        case KSHIFT:  kb_shift_ = !kb_shift_; break;
        case KSUBMIT: submit_search(); break;
    }
}
void App::submit_search() {
    std::string q = query_input_;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (kb_mode_ == KbMode::CastCode) {   // "Add a device": pair with the code, then cast
        kb_mode_ = KbMode::Search;
        mode_ = Mode::Grid;               // picker stays open underneath
        submit_cast_code(q);
        return;
    }
    if (kb_mode_ == KbMode::LinkDevice) {   // "Link a device" (manage): pair, no cast
        kb_mode_ = KbMode::Search;
        mode_ = Mode::Grid;               // manage overlay stays open underneath
        link_device(q);
        return;
    }
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
    std::string msg = i18n::tr(i18n::Str::Loading);
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
    rn.text(*font_title_, kb_title_, 32*s, 24*s, theme_.text);

    // Query input box + blinking caret.
    float bx = 32*s, by = hbar + 28*s, bw = W - 64*s, bh = 54*s;
    rn.quad({bx, by, bw, bh}, theme_.card);
    rn.quad({bx, by, 4*s, bh}, theme_.accent);
    // Window the text so the caret (at kb_caret_) stays visible even when it overflows.
    float avail = bw - 36*s;
    int csz = (int)query_input_.size();
    int caret = std::max(0, std::min(kb_caret_, csz));
    int start = 0;   // pull start rightward until the pre-caret text fits
    while (start < caret &&
           font_body_->text_width(query_input_.substr(start, caret - start)) > avail)
        start++;
    std::string shown;   // extend from start while it fits
    for (int i = start; i < csz; ++i) {
        std::string cand = shown; cand += query_input_[i];
        if (font_body_->text_width(cand) > avail) break;
        shown = std::move(cand);
    }
    rn.text(*font_body_, shown, bx + 18*s, by + 14*s, theme_.text);
    if (query_input_.empty() && !kb_placeholder_.empty())
        rn.text(*font_body_, kb_placeholder_, bx + 18*s, by + 14*s, theme_.text_dim);
    float caret_x = bx + 18*s + font_body_->text_width(query_input_.substr(start, caret - start));
    if ((SDL_GetTicks() / 500) % 2 == 0)
        rn.quad({caret_x + 1*s, by + 12*s, 2*s, bh - 24*s}, theme_.text);

    // Keyboard grid — centered; selected key drawn white (ATV style). The numeric
    // keypad (device linking) uses square keys on a narrow, centered grid.
    const auto& kb = kb_numeric() ? KB_NUM : KB;
    bool numeric = kb_numeric();
    float gap = numeric ? 14*s : 10*s;
    float keyh = numeric ? 92*s : 60*s;
    float unit = numeric ? keyh                    // square digit keys
                         : (W * 0.94f - 9*gap) / 10.f;   // qwerty: every row totals 10 units
    float gy = by + bh + (numeric ? 26*s : 28*s);
    for (int r = 0; r < (int)kb.size(); ++r) {
        float roww = -gap;   // true laid-out width (count each span's internal gaps too)
        for (const auto& k : kb[r]) roww += unit * k.span + gap * k.span;
        float x = (W - roww) / 2;
        float ky = gy + r*(keyh+gap);
        for (int c = 0; c < (int)kb[r].size(); ++c) {
            const Key& k = kb[r][c];
            float kw = unit * k.span + gap * (k.span - 1);
            bool sel = (r == kb_row_ && c == kb_col_);
            gfx::Rect kr{x, ky, kw, keyh};
            gfx::Color bg = sel ? theme_.text
                          : (k.type == KSUBMIT) ? theme_.accent : theme_.card;
            rn.quad(kr, bg);
            if (k.type == KSPACE) {                       // draw a space bar, not text
                float bar = kw * 0.42f;
                rn.quad({kr.x + (kw-bar)/2, kr.y + keyh - 16*s, bar, 3*s},
                        sel ? theme_.bg : theme_.text_dim);
            } else {
                std::string lbl = (k.type == KSUBMIT) ? i18n::tr(i18n::Str::KbEnter) : k.label;
                if (k.type == KCHAR && kb_shift_ && lbl.size()==1 && lbl[0]>='a' && lbl[0]<='z')
                    lbl[0] = (char)(lbl[0]-'a'+'A');
                const gfx::Font& f = *font_body_;
                gfx::Color tc = sel ? theme_.bg
                              : (k.type == KSHIFT && kb_shift_) ? theme_.accent : theme_.text;
                rn.text(f, lbl, kr.x + (kw - f.text_width(lbl))/2,
                        kr.y + (keyh - f.line_height())/2 + 4*s, tc);
            }
            x += kw + gap;
        }
    }

    // Footer hints.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, i18n::tr(numeric ? i18n::Str::FooterCastCode : i18n::Str::FooterSearch),
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
    audio_override_lang_.clear();                // per-video: back to the global default
    audio_dirty_ = false;
    cc_restore_key_.clear();                     // new video: captions start Off
    cc_failed_langs_.clear();                    // new video: failures forgotten
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
    audio_override_lang_.clear();   // per-video track choice ends with its video
    audio_dirty_ = false;
    cc_restore_key_.clear();        // new video: captions start Off
    cc_failed_langs_.clear();       // new video: failures forgotten
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
        status_msg_ = std::string(i18n::tr(i18n::Str::UpNext)) + ": " + auto_next_item_.title;
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
    status_msg_ = i18n::tr(i18n::Str::FindingNext); status_until_ = SDL_GetTicks() + 6000;
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
    if (r.empty()) { mode_ = Mode::Grid; status_msg_ = i18n::tr(i18n::Str::NoMoreVideos);
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
// The mpv "hwdec" value for the current Video Decode setting. On hwdec-capable
// devices Hardware means the verified v4l2m2m path; elsewhere the historical
// auto-copy-safe default stands (falls back to software where nothing matches).
std::string App::hwdec_mode_str() const {
    if (hwdec_mode_) return "no";
    return hwdec_capable_ ? "v4l2m2m-copy" : "auto-copy-safe";
}

// The AudioPrefs.lang value for the next resolve: the per-video override wins,
// else the global setting ("app" -> the UI language; "orig" -> "" = original).
std::string App::effective_audio_lang() const {
    if (!audio_override_lang_.empty()) return audio_override_lang_;
    if (audio_lang_pref_ == "app")  return i18n::language_hl(lang_);
    if (audio_lang_pref_ == "orig") return "";
    return audio_lang_pref_;
}

void App::replay_current(double at_seconds) {
    if (now_playing_item_.video_id.empty()) return;
    // Same video, new stream (quality/audio-track change): carry the caption
    // selection over — start_captions resets it, poll_captions restores it.
    cc_restore_key_ = cc_current_key();
    // A restricted (paced) stream's FRESH url only serves ~the first 20MiB, so a deep
    // resume would 403 on load. Clamp into the fresh window (approx via bitrate).
    if (playing_paced_ && playing_vbitrate_ > 0) {
        double safe = (15.0 * 1024 * 1024 * 8) / (double)playing_vbitrate_;
        if (at_seconds > safe) {
            at_seconds = safe;
            status_msg_ = i18n::tr(i18n::Str::PacedResuming);
            status_until_ = SDL_GetTicks() + 5000;
        }
    }
    start_resolve(now_playing_item_.video_id, now_playing_item_.title, at_seconds);
}

void App::start_resolve(const std::string& video_id, const std::string& title, double start_pos) {
    if (!Player::available()) {
        status_msg_ = i18n::tr(i18n::Str::NoMpv);
        return;
    }
    if (resolve_running_) return;               // one resolve at a time
    status_msg_.clear();
    loading_title_ = title;
    resume_pos_ = start_pos;                     // applied in poll_resolve after play()
    mode_ = Mode::Loading;

    std::string fallback_title = title;
    yt::VideoPrefs prefs = play_prefs_;
    yt::AudioPrefs aprefs; aprefs.lang = effective_audio_lang();
    resolve_running_ = true;
    resolve_done_ = false;
    // "Offline mode" is scoped to the Downloads view: only there do we play the local
    // file. From any other page a downloaded video streams normally (full options).
    bool play_local = it_.is_downloaded(video_id) && view_label_ == "Downloads";
    if (resolve_thread_.joinable()) resolve_thread_.join();
    resolve_thread_ = std::thread([this, video_id, fallback_title, prefs, aprefs, play_local]() {
        bool dbg = getenv("YTC_DEBUG");
        ResolveResult r;
        r.title = fallback_title;
        // Offline download: skip the network resolve and play the local file directly.
        if (play_local) {
            r.ok = true; r.video_url = it_.download_path(video_id);
            r.audio_url.clear(); r.user_agent.clear();
            r.description = it_.download_description(video_id);   // from the .info sidecar
            { std::lock_guard<std::mutex> lk(resolve_m_); resolve_result_ = std::move(r); }
            resolve_running_ = false; resolve_done_ = true;
            return;
        }
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
            const yt::Format* af = info.best_audio(aprefs);
            if (!vf) { r.ok = false; r.status = "No playable video format"; }
            else {
                r.ok = true;
                r.video_url = vf->url;
                r.audio_url = af ? af->url : "";
                r.user_agent = info.user_agent;
                r.video_bitrate = vf->bitrate;
                r.audio_lang = af ? af->track_lang : "";
                r.audio_tracks = info.audio_tracks();   // for the player track picker
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
        mode_ = Mode::Grid; status_msg_ = i18n::tr(i18n::Str::PlayerFailed); return;
    }
    player_.set_volume(volume_);                 // apply the app-local volume level
    player_.set_speed(playback_speed_);          // apply speed (persists across re-resolve)
    start_sponsorblock(now_playing_item_.video_id);   // fetch skip segments off-thread
    start_captions(now_playing_item_.video_id);        // fetch caption track list off-thread
    now_playing_title_ = r.title;
    now_playing_desc_ = r.description;          // free: came with the resolve
    playing_audio_tracks_ = std::move(r.audio_tracks);
    playing_audio_lang_ = r.audio_lang;
    playing_paced_ = r.paced;
    playing_vbitrate_ = r.video_bitrate;
    played_max_ = start_at;                     // seek window anchors here
    if (playing_paced_) {
        status_msg_ = i18n::tr(i18n::Str::SeekLimited);
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
