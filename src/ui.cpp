#include "ui.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace ui {

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
    return it == tex_.end() ? nullptr : it->second.get();
}
void ThumbCache::worker() {
    HttpClient http;
    while (!stop_) {
        std::string url;
        { std::lock_guard<std::mutex> lk(m_);
          if (!queue_.empty()) { url = queue_.front(); queue_.pop_front(); } }
        if (url.empty()) { SDL_Delay(10); continue; }
        auto r = http.get(url);
        if (getenv("YTNATIVE_DEBUG"))
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
        if (!p.ok || p.bytes.empty()) continue;
        auto t = gfx::Texture::from_encoded(p.bytes.data(), p.bytes.size());
        if (getenv("YTNATIVE_DEBUG"))
            std::fprintf(stderr, "[thumb] decode %.40s -> %s\n", p.url.c_str(), t ? "OK" : "FAIL");
        if (t) tex_[p.url] = std::move(t);
    }
}

// ---------- App ----------
void App::load_fonts() {
    // Portable font lookup: env override, then the port/app-local copy in
    // data/, then the Debian system path (dev machines). Handhelds don't
    // ship DejaVu — the port bundles it in data/.
    auto pick_font = []() -> std::string {
        const char* env = std::getenv("YTNATIVE_FONT");
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
    carousel_ = getenv("YTNATIVE_CAROUSEL") != nullptr;   // start in carousel view

    // Default quality cap: 1080p (not 4K). Override with YTNATIVE_MAXHEIGHT.
    play_prefs_.max_height = 1080;
    if (const char* mh = getenv("YTNATIVE_MAXHEIGHT")) play_prefs_.max_height = atoi(mh);
}

App::~App() {
    if (resolve_thread_.joinable()) resolve_thread_.join();
}

void App::set_results(std::vector<yt::SearchResult> r) {
    results_ = std::move(r);
    sel_ = 0; scroll_ = 0;
    for (const auto& v : results_) thumbs_.request(v.thumbnail_url);
}
void App::search(const std::string& query) {
    query_ = query;
    set_results(it_.search(query, 24));
}

const yt::SearchResult* App::selected() const {
    if (sel_ < 0 || sel_ >= (int)results_.size()) return nullptr;
    return &results_[sel_];
}
void App::input(Action a) {
    if (mode_ == Mode::Playing) {
        switch (a) {
            case Action::Select: player_.toggle_pause(); break;
            case Action::Back:   player_.stop(); mode_ = Mode::Grid; break;
            case Action::Left:   player_.seek_relative(-10); break;
            case Action::Right:  player_.seek_relative(10); break;
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
    // Grid / carousel browse.
    if (a == Action::Search) { open_search(); return; }
    int n = (int)results_.size();
    if (n == 0) return;
    if (carousel_) {
        switch (a) {
            case Action::Left:  if (sel_ > 0) sel_--; break;
            case Action::Right: if (sel_ + 1 < n) sel_++; break;
            case Action::Select: request_playback(); break;
            default: break;
        }
        return;   // carousel_pos_ eases toward sel_ in render_carousel
    }
    switch (a) {
        case Action::Left:  if (sel_ % cols_ != 0) sel_--; break;
        case Action::Right: if (sel_ % cols_ != cols_-1 && sel_+1 < n) sel_++; break;
        case Action::Up:    if (sel_ - cols_ >= 0) sel_ -= cols_; break;
        case Action::Down:  if (sel_ + cols_ < n) sel_ += cols_; break;
        case Action::Select: request_playback(); break;
        default: break;
    }
    ensure_visible();
}

void App::pump_async() {
    thumbs_.pump();
    poll_resolve();
    if (mode_ == Mode::Playing) {
        if (!player_.pump()) {       // playback ended
            player_.stop();
            mode_ = Mode::Grid;
        }
    }
}

void App::toggle_view() { carousel_ = !carousel_; carousel_pos_ = sel_; }

void App::render(gfx::Renderer& rn) {
    if (mode_ == Mode::Playing) { render_player(rn); return; }
    if (mode_ == Mode::Search)  { render_search(rn); return; }
    if (carousel_) render_carousel(rn);
    else           render_grid(rn);
    if (mode_ == Mode::Loading) render_loading(rn);
}

void App::render_grid(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);

    // Header bar.
    float hbar = 84 * s;
    rn.quad({0, 0, (float)W, hbar}, theme_.panel);
    rn.quad({0, hbar-3*s, (float)W, 3*s}, theme_.accent);
    rn.text(*font_title_, "ytnative", 32*s, 24*s, theme_.text);
    std::string sub = query_.empty() ? "" : ("search: " + query_);
    rn.text(*font_body_, sub, 240*s, 32*s, theme_.text_dim);
    std::string count = std::to_string(results_.size()) + " results";
    rn.text(*font_small_, count, W - font_small_->text_width(count) - 32*s, 34*s, theme_.text_dim);

    // Grid geometry.
    float pad = 28*s;
    float top = hbar + pad;
    float gutter = 22*s;
    float cardw = (W - pad*2 - gutter*(cols_-1)) / cols_;
    float thumbh = cardw * 9.f/16.f;
    float meta_h = 74*s;
    float cardh = thumbh + meta_h;
    float rowstep = cardh + gutter;

    for (int i = 0; i < (int)results_.size(); ++i) {
        int col = i % cols_, row = i / cols_;
        float x = pad + col*(cardw+gutter);
        float y = top + row*rowstep - scroll_;
        if (y + cardh < hbar || y > H) continue;    // cull offscreen
        bool sel = (i == sel_);

        // Card background + selection ring.
        gfx::Rect card{x, y, cardw, cardh};
        if (sel) rn.quad({x-4*s, y-4*s, cardw+8*s, cardh+8*s}, theme_.accent);
        rn.quad(card, sel ? theme_.card_sel : theme_.card);

        // Thumbnail (cover-cropped) or placeholder.
        gfx::Rect trect{x, y, cardw, thumbh};
        rn.quad(trect, theme_.thumb_bg);
        if (auto* tex = thumbs_.get(results_[i].thumbnail_url))
            rn.textured_cover(trect, *tex);
        else
            rn.text(*font_small_, "loading...", x + 12*s, y + thumbh/2 - 9*s, theme_.text_dim);

        // Duration pill.
        const auto& v = results_[i];
        if (!v.length_text.empty()) {
            float pw = font_small_->text_width(v.length_text) + 12*s;
            gfx::Rect pill{x + cardw - pw - 8*s, y + thumbh - 26*s, pw, 22*s};
            rn.quad(pill, gfx::Color::rgb(0x000000).with_a(0.75f));
            rn.text(*font_small_, v.length_text, pill.x + 6*s, pill.y + 2*s, theme_.text);
        }

        // Title (2 lines max) + author + views.
        float tx = x + 12*s, ty = y + thumbh + 8*s;
        std::string title = font_body_->ellipsize(v.title, cardw - 24*s);
        rn.text(*font_body_, title, tx, ty, sel ? theme_.text : theme_.text);
        std::string line2 = v.author;
        if (!v.view_count_text.empty()) line2 += "   -   " + v.view_count_text;
        rn.text(*font_small_, font_small_->ellipsize(line2, cardw - 24*s),
                tx, ty + 28*s, theme_.text_dim);
    }

    // Footer hint bar.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "D-Pad: navigate    A: play    B: back    Y: search",
            32*s, H - fh + 12*s, theme_.text_dim);

    // Transient status banner (e.g. "This live event will begin in 3 days.").
    if (!status_msg_.empty() && SDL_GetTicks() < status_until_) {
        float tw = font_body_->text_width(status_msg_);
        float bw = tw + 48*s, bh = 52*s;
        float bx = (W - bw)/2, by = hbar + 22*s;
        rn.quad({bx, by, bw, bh}, theme_.panel);
        rn.quad({bx, by, 5*s, bh}, theme_.accent);
        rn.text(*font_body_, status_msg_, bx + 24*s, by + (bh-font_body_->line_height())/2 + 3*s,
                theme_.text);
    }
    rn.end();
}

void App::render_carousel(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    rn.begin(W, H);
    rn.clear(theme_.bg);

    // Header.
    float hbar = 84 * s;
    rn.quad({0, 0, (float)W, hbar}, theme_.panel);
    rn.quad({0, hbar-3*s, (float)W, 3*s}, theme_.accent);
    rn.text(*font_title_, "ytnative", 32*s, 24*s, theme_.text);
    if (!query_.empty())
        rn.text(*font_body_, "search: " + query_, 240*s, 32*s, theme_.text_dim);
    std::string count = std::to_string(results_.size()) + " results";
    rn.text(*font_small_, count, W - font_small_->text_width(count) - 32*s, 34*s, theme_.text_dim);

    int n = (int)results_.size();
    if (n == 0) { rn.end(); return; }

    // Ease the animated position toward the selection.
    carousel_pos_ += (sel_ - carousel_pos_) * 0.20f;
    if (std::abs(sel_ - carousel_pos_) < 0.01f) carousel_pos_ = sel_;

    float cx = W / 2.f, cy = hbar + (H - hbar) * 0.44f;
    float big_w = std::min(W * 0.46f, (H - hbar) * 0.62f * 16.f/9.f);
    float big_h = big_w * 9.f / 16.f;
    float spacing = big_w * 0.62f;

    // Ensure thumbnails near the center are requested.
    for (int i = std::max(0, sel_-3); i < std::min(n, sel_+4); ++i)
        thumbs_.request(results_[i].thumbnail_url);

    // Build the visible set and draw far-to-near so the centered card is on top.
    std::vector<int> vis;
    for (int i = 0; i < n; ++i)
        if (std::abs(i - carousel_pos_) <= 3.2f) vis.push_back(i);
    std::sort(vis.begin(), vis.end(), [&](int a, int b){
        return std::abs(a - carousel_pos_) > std::abs(b - carousel_pos_); });

    for (int i : vis) {
        float d = i - carousel_pos_;
        float dist = std::abs(d);
        float scale = std::max(0.55f, 1.0f - dist * 0.16f);
        float w = big_w * scale, h = big_h * scale;
        float x = cx + d * spacing - w/2;
        float y = cy - h/2;
        float a = std::max(0.30f, 1.0f - dist * 0.28f);
        bool center = (i == sel_);
        gfx::Rect r{x, y, w, h};
        if (center) rn.quad({x-4*s, y-4*s, w+8*s, h+8*s}, theme_.accent);
        rn.quad(r, theme_.thumb_bg.with_a(a));
        if (auto* tex = thumbs_.get(results_[i].thumbnail_url))
            rn.textured_cover(r, *tex, gfx::Color{1,1,1,a});
        // Duration pill on the centered card.
        if (center && !results_[i].length_text.empty()) {
            float pw = font_small_->text_width(results_[i].length_text) + 12*s;
            gfx::Rect pill{x + w - pw - 8*s, y + h - 26*s, pw, 22*s};
            rn.quad(pill, gfx::Color::rgb(0x000000).with_a(0.75f));
            rn.text(*font_small_, results_[i].length_text, pill.x + 6*s, pill.y + 2*s, theme_.text);
        }
    }

    // Centered item title + metadata below the strip.
    const auto& v = results_[sel_];
    float ty = cy + big_h/2 + 26*s;
    std::string title = font_title_->ellipsize(v.title, W * 0.8f);
    rn.text(*font_title_, title, cx - font_title_->text_width(title)/2, ty, theme_.text);
    std::string line2 = v.author;
    if (!v.view_count_text.empty()) line2 += "   -   " + v.view_count_text;
    rn.text(*font_body_, line2, cx - font_body_->text_width(line2)/2, ty + 42*s, theme_.text_dim);
    std::string pos = std::to_string(sel_+1) + " / " + std::to_string(n);
    rn.text(*font_small_, pos, cx - font_small_->text_width(pos)/2, ty + 76*s, theme_.text_dim);

    // Footer.
    float fh = 44*s;
    rn.quad({0, H-fh, (float)W, fh}, theme_.panel);
    rn.text(*font_small_, "Left/Right: browse    A: play    Y: search    V/LB: grid view",
            32*s, H - fh + 12*s, theme_.text_dim);
    if (!status_msg_.empty() && SDL_GetTicks() < status_until_) {
        float tw = font_body_->text_width(status_msg_);
        rn.quad({(W-tw-48*s)/2, hbar+16*s, tw+48*s, 46*s}, theme_.panel);
        rn.text(*font_body_, status_msg_, (W-tw)/2, hbar+26*s, theme_.text);
    }
    rn.end();
}

void App::render_player(gfx::Renderer& rn) {
    const int W = win_->width(), H = win_->height();
    float s = H / 720.f;
    // mpv paints the video frame into the framebuffer first...
    static int fc = 0;
    if (getenv("YTNATIVE_DEBUG") && (fc++ % 20 == 0))
        std::fprintf(stderr, "[play] frame %d pos=%.1f dur=%.1f\n", fc, player_.position(), player_.duration());
    player_.render(W, H);
    // ...then we overlay UI on top (mpv leaves GL state; begin() resets ours).
    rn.begin(W, H);

    double pos = player_.position(), dur = player_.duration();
    float bar_h = 96 * s;
    // Bottom gradient-ish panel for legibility.
    rn.quad({0, (float)H - bar_h, (float)W, bar_h}, theme_.bg.with_a(0.66f));

    // Title.
    rn.text(*font_body_, font_body_->ellipsize(now_playing_title_, W - 64*s),
            32*s, H - bar_h + 12*s, theme_.text);

    // Progress bar.
    float px = 32*s, pw = W - 64*s, py = H - 40*s, ph = 6*s;
    rn.quad({px, py, pw, ph}, theme_.card_sel);
    float frac = dur > 0 ? (float)(pos / dur) : 0.f;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    rn.quad({px, py, pw * frac, ph}, theme_.accent);
    // Playhead knob.
    rn.quad({px + pw*frac - 5*s, py - 5*s, 10*s, ph + 10*s}, theme_.text);

    // Time + state text.
    auto tstr = [](double t){ int m=(int)t/60, sec=(int)t%60; char b[16];
        std::snprintf(b,sizeof b,"%d:%02d",m,sec); return std::string(b); };
    std::string time = tstr(pos) + " / " + (dur>0?tstr(dur):"--:--");
    rn.text(*font_small_, time, 32*s, H - 30*s, theme_.text_dim);
    std::string hint = std::string(player_.paused()?"[A] resume":"[A] pause")
                     + "   [<>] seek 10s   [B] back";
    rn.text(*font_small_, hint, W - font_small_->text_width(hint) - 32*s,
            H - 30*s, theme_.text_dim);
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
    std::string shown = query_input_.empty() ? "" : query_input_;
    rn.text(*font_body_, font_body_->ellipsize(shown, bw - 40*s),
            bx + 18*s, by + 14*s, theme_.text);
    float caret_x = bx + 18*s + font_body_->text_width(query_input_);
    if ((SDL_GetTicks() / 500) % 2 == 0 && caret_x < bx + bw - 16*s)
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
    if (!v) return;
    if (!Player::available()) {
        status_msg_ = "playback unavailable (no libmpv in this build)";
        return;
    }
    if (resolve_running_) return;               // one resolve at a time
    status_msg_.clear();
    loading_title_ = v->title;
    mode_ = Mode::Loading;

    std::string video_id = v->video_id;
    std::string fallback_title = v->title;
    yt::VideoPrefs prefs = play_prefs_;
    resolve_running_ = true;
    resolve_done_ = false;
    if (resolve_thread_.joinable()) resolve_thread_.join();
    resolve_thread_ = std::thread([this, video_id, fallback_title, prefs]() {
        bool dbg = getenv("YTNATIVE_DEBUG");
        if (dbg) std::fprintf(stderr, "[play] resolving %s (cap=%d)...\n",
                              video_id.c_str(), prefs.max_height);
        yt::VideoInfo info = it_.resolve(video_id);   // background thread owns it_ here
        ResolveResult r;
        r.title = info.title.empty() ? fallback_title : info.title;
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
                if (dbg) std::fprintf(stderr, "[play] resolved: video itag %d audio itag %d\n",
                                      vf->itag, af ? af->itag : -1);
            }
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
    if (!player_.play(r.video_url, r.audio_url, r.user_agent)) {
        mode_ = Mode::Grid; status_msg_ = "player failed to start"; return;
    }
    now_playing_title_ = r.title;
    mode_ = Mode::Playing;
}

void App::ensure_visible() {
    const int H = win_->height();
    float s = H / 720.f;
    float hbar = 84*s, pad = 28*s, gutter = 22*s;
    float cardw = (win_->width() - pad*2 - gutter*(cols_-1)) / cols_;
    float cardh = cardw*9.f/16.f + 74*s;
    float rowstep = cardh + gutter;
    float top = hbar + pad;
    int row = sel_ / cols_;
    float y = top + row*rowstep - scroll_;
    float fh = 44*s;
    if (y < hbar + pad) scroll_ -= (hbar + pad - y);
    if (y + cardh > H - fh) scroll_ += (y + cardh) - (H - fh);
    if (scroll_ < 0) scroll_ = 0;
}

} // namespace ui
