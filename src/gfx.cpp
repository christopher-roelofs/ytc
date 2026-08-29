#include "gfx.h"
#include <SDL.h>
// Desktop builds (Windows/macOS/desktop Linux) use real OpenGL via GLEW to load
// the 2.0 entry points uniformly; the ARM handhelds have only GLES2.
#ifdef YTC_GL_DESKTOP
  #include <GL/glew.h>
#else
  #include <SDL_opengles2.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

namespace gfx {

// ---------- Texture ----------
Texture::~Texture() { if (id_) glDeleteTextures(1, &id_); }
Texture::Texture(Texture&& o) noexcept : id_(o.id_), w_(o.w_), h_(o.h_) { o.id_ = 0; }
Texture& Texture::operator=(Texture&& o) noexcept {
    if (this != &o) { if (id_) glDeleteTextures(1, &id_);
        id_ = o.id_; w_ = o.w_; h_ = o.h_; o.id_ = 0; }
    return *this;
}
std::unique_ptr<Texture> Texture::from_rgba(const uint8_t* px, int w, int h) {
    auto t = std::make_unique<Texture>();
    glGenTextures(1, &t->id_);
    glBindTexture(GL_TEXTURE_2D, t->id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t->w_ = w; t->h_ = h;
    return t;
}
std::unique_ptr<Texture> Texture::from_encoded(const uint8_t* data, size_t n) {
    int w, h, ch;
    uint8_t* px = stbi_load_from_memory(data, (int)n, &w, &h, &ch, 4);
    if (!px) return nullptr;
    auto t = from_rgba(px, w, h);
    stbi_image_free(px);
    return t;
}
std::unique_ptr<Texture> Texture::create_empty(int w, int h) {
    std::vector<uint8_t> zero(w * h * 4, 0);
    return from_rgba(zero.data(), w, h);
}
void Texture::update(int x, int y, int w, int h, const uint8_t* rgba) {
    if (!id_ || w <= 0 || h <= 0) return;
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

// ---------- Font (UTF-8, on-demand glyph cache) ----------

// Decode one UTF-8 codepoint starting at s[i]; advances i. Bad bytes -> U+FFFD.
static uint32_t utf8_next(const std::string& s, size_t& i) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) { i += 1; return c; }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) {
        uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)s[i+1] & 0x3F);
        i += 2; return cp;
    }
    if ((c >> 4) == 0xE && i + 2 < s.size()) {
        uint32_t cp = ((c & 0x0F) << 12) | (((uint8_t)s[i+1] & 0x3F) << 6)
                    | ((uint8_t)s[i+2] & 0x3F);
        i += 3; return cp;
    }
    if ((c >> 3) == 0x1E && i + 3 < s.size()) {
        uint32_t cp = ((c & 0x07) << 18) | (((uint8_t)s[i+1] & 0x3F) << 12)
                    | (((uint8_t)s[i+2] & 0x3F) << 6) | ((uint8_t)s[i+3] & 0x3F);
        i += 4; return cp;
    }
    i += 1; return 0xFFFD;
}

// Emoji-ish codepoints (incl. joiners/selectors): when the font lacks them, SKIP
// silently instead of rendering tofu — titles read cleanly without the decorations.
static bool is_emoji_like(uint32_t cp) {
    return cp >= 0x1F000 ||                          // pictographs, emoticons, ...
           (cp >= 0x2600 && cp <= 0x27BF) ||         // misc symbols / dingbats
           (cp >= 0x2B00 && cp <= 0x2BFF) ||         // misc symbols and arrows
           (cp >= 0xFE00 && cp <= 0xFE0F) ||         // variation selectors
           cp == 0x200D || cp == 0x20E3;             // ZWJ, combining keycap
}

std::unique_ptr<Font> Font::load(const std::string& path, float pixel_h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    auto font = std::make_unique<Font>();
    font->ttf_.assign((std::istreambuf_iterator<char>(f)), {});
    auto* info = new stbtt_fontinfo();
    if (!stbtt_InitFont(info, font->ttf_.data(),
                        stbtt_GetFontOffsetForIndex(font->ttf_.data(), 0))) {
        delete info;
        return nullptr;
    }
    font->info_ = info;
    font->pixel_h_ = pixel_h;
    font->scale_ = stbtt_ScaleForPixelHeight(info, pixel_h);
    int asc, desc, gap; stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
    font->ascent_ = asc * font->scale_;
    font->line_h_ = (asc - desc + gap) * font->scale_;
    font->atlas_w_ = font->atlas_h_ = 1024;
    font->atlas_ = Texture::create_empty(font->atlas_w_, font->atlas_h_);
    return font;
}
Font::~Font() { delete static_cast<stbtt_fontinfo*>(info_); }

const Font::Glyph* Font::glyph(uint32_t cp) const {
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end()) return it->second.w < 0 ? nullptr : &it->second;
    auto* info = static_cast<const stbtt_fontinfo*>(info_);
    int gi = stbtt_FindGlyphIndex(info, (int)cp);
    if (gi == 0) {
        // Not in the font. Emoji-ish -> skip; otherwise show a tofu box (U+25A1).
        if (!is_emoji_like(cp) && cp != 0x25A1) {
            if (const Glyph* tofu = glyph(0x25A1)) {
                glyphs_[cp] = *tofu;
                return &glyphs_[cp];
            }
        }
        Glyph miss{}; miss.w = -1;                 // sentinel: skip entirely
        glyphs_[cp] = miss;
        return nullptr;
    }
    int adv, lsb;
    stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(info, gi, scale_, scale_, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;

    if (pen_x_ + gw + 1 > atlas_w_) { pen_x_ = 1; pen_y_ += row_h_ + 1; row_h_ = 0; }
    if (pen_y_ + gh + 1 > atlas_h_) {
        // Atlas full (rare): restart packing; evicted glyphs re-cache lazily.
        glyphs_.clear();
        pen_x_ = pen_y_ = 1; row_h_ = 0;
    }

    if (gw > 0 && gh > 0) {
        std::vector<uint8_t> mono(gw * gh);
        stbtt_MakeGlyphBitmap(info, mono.data(), gw, gh, gw, scale_, scale_, gi);
        std::vector<uint8_t> rgba(gw * gh * 4);
        for (int i2 = 0; i2 < gw * gh; ++i2) {
            rgba[i2*4+0] = 255; rgba[i2*4+1] = 255; rgba[i2*4+2] = 255;
            rgba[i2*4+3] = mono[i2];
        }
        atlas_->update(pen_x_, pen_y_, gw, gh, rgba.data());
    }
    Glyph g;
    g.x0 = (float)pen_x_; g.y0 = (float)pen_y_;
    g.x1 = (float)(pen_x_ + gw); g.y1 = (float)(pen_y_ + gh);
    g.xoff = (float)x0; g.yoff = (float)y0;
    g.xadv = adv * scale_;
    g.w = gw; g.h = gh;
    if (gh > row_h_) row_h_ = gh;
    pen_x_ += gw + 1;
    auto [ins, ok] = glyphs_.emplace(cp, g);
    return &ins->second;
}
float Font::text_width(const std::string& s) const {
    float w = 0;
    size_t i = 0;
    while (i < s.size()) {
        const Glyph* g = glyph(utf8_next(s, i));
        if (g) w += g->xadv;
    }
    return w;
}
std::string Font::ellipsize(const std::string& s, float max_w) const {
    if (text_width(s) <= max_w) return s;
    float ell = text_width("...");
    std::string out;
    float w = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        const Glyph* g = glyph(utf8_next(s, i));
        float cw = g ? g->xadv : 0;
        if (w + cw + ell > max_w) break;
        out.append(s, start, i - start);           // whole UTF-8 sequence, never split
        w += cw;
    }
    return out + "...";
}

// ---------- Window ----------
std::unique_ptr<Window> Window::create(int w, int h, const std::string& title,
                                       const std::string& driver) {
    // Set the video driver via env var (portable across SDL versions; the
    // SDL_HINT_VIDEODRIVER macro only exists in SDL >= 2.0.22).
    if (!driver.empty()) SDL_setenv("SDL_VIDEODRIVER", driver.c_str(), 1);   // SDL_setenv: portable
#ifdef YTC_GL_DESKTOP
    // Desktop controllers: map face buttons by POSITION (south = A), not by the printed
    // label. Without this, SDL reports a Nintendo-layout pad with A/B and X/Y swapped,
    // which doesn't match our A=confirm/B=back layout. (Handhelds keep their behavior.)
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return nullptr;
    }
#ifdef YTC_GL_DESKTOP
    // Desktop OpenGL 2.1 (compatibility) — same feature set our GLES2 code needs.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const char* drv = SDL_GetCurrentVideoDriver();
    bool headless = drv && std::strcmp(drv, "offscreen") == 0;
    Uint32 mode = getenv("YTC_WINDOWED") ? (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)
                                              : SDL_WINDOW_FULLSCREEN_DESKTOP;
    Uint32 flags = SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : mode);
    SDL_Window* win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!win) { std::fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return nullptr; }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { std::fprintf(stderr, "GL_CreateContext: %s\n", SDL_GetError()); return nullptr; }
    SDL_GL_MakeCurrent(win, gl);
#ifdef YTC_GL_DESKTOP
    glewExperimental = GL_TRUE;
    glewInit();          // with glewExperimental this returns a benign error yet loads
    glGetError();        // fully; swallow the spurious GL_INVALID_ENUM it leaves behind.
#endif

    auto out = std::make_unique<Window>();
    out->win_ = win; out->gl_ = gl;
    SDL_GL_GetDrawableSize(win, &out->w_, &out->h_);
    if (out->w_ == 0) { out->w_ = w; out->h_ = h; }
    return out;
}
Window::~Window() {
    if (gl_) SDL_GL_DeleteContext((SDL_GLContext)gl_);
    if (win_) SDL_DestroyWindow((SDL_Window*)win_);
    SDL_Quit();
}
void Window::refresh_size() {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize((SDL_Window*)win_, &w, &h);
    if (w > 0 && h > 0) { w_ = w; h_ = h; }
}
void Window::swap() { SDL_GL_SwapWindow((SDL_Window*)win_); }
bool Window::screenshot(const std::string& path) const {
    std::vector<uint8_t> px(w_ * h_ * 4);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    // Flip vertically (GL origin is bottom-left).
    std::vector<uint8_t> flip(w_ * h_ * 4);
    for (int y = 0; y < h_; ++y)
        std::memcpy(&flip[y * w_ * 4], &px[(h_ - 1 - y) * w_ * 4], w_ * 4);
    return stbi_write_png(path.c_str(), w_, h_, 4, flip.data(), w_ * 4) != 0;
}

// ---------- Renderer ----------
static unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log); }
    return s;
}
Renderer::Renderer() {
    // Desktop GLSL 1.20 needs a #version line and rejects the ES `precision` qualifier;
    // GLES defaults to #version 100 and requires the precision. `attribute`/`varying`/
    // `texture2D`/`gl_FragColor` are valid in both, so only the preamble differs.
#ifdef YTC_GL_DESKTOP
    const char* VER = "#version 120\n"; const char* PREC = "";
#else
    const char* VER = ""; const char* PREC = "precision mediump float;";
#endif
    std::string vs = std::string(VER) +
        "attribute vec2 aPos; attribute vec2 aUV; attribute vec4 aCol;"
        "uniform vec2 uProj; varying vec2 vUV; varying vec4 vCol;"
        "void main(){ vUV=aUV; vCol=aCol;"
        " gl_Position=vec4(aPos.x/uProj.x*2.0-1.0, 1.0-aPos.y/uProj.y*2.0, 0.0, 1.0);}";
    std::string fs = std::string(VER) + PREC +
        " varying vec2 vUV; varying vec4 vCol;"
        "uniform sampler2D uTex;"
        "void main(){ gl_FragColor = texture2D(uTex, vUV) * vCol; }";
    prog_ = glCreateProgram();
    unsigned v = compile(GL_VERTEX_SHADER, vs.c_str()), f = compile(GL_FRAGMENT_SHADER, fs.c_str());
    glAttachShader(prog_, v); glAttachShader(prog_, f);
    glBindAttribLocation(prog_, 0, "aPos");
    glBindAttribLocation(prog_, 1, "aUV");
    glBindAttribLocation(prog_, 2, "aCol");
    glLinkProgram(prog_);
    glDeleteShader(v); glDeleteShader(f);
    u_proj_ = glGetUniformLocation(prog_, "uProj");
    glGenBuffers(1, &vbo_);
    uint8_t white[4] = {255,255,255,255};
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}
Renderer::~Renderer() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (white_) glDeleteTextures(1, &white_);
    if (prog_) glDeleteProgram(prog_);
}
void Renderer::begin(int fb_w, int fb_h) {
    fb_w_ = fb_w; fb_h_ = fb_h;
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glUniform2f(u_proj_, (float)fb_w, (float)fb_h);
    cur_tex_ = 0;
}
void Renderer::clear(Color c) {
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
}
void Renderer::set_texture(unsigned tex) {
    if (tex != cur_tex_) { flush(); cur_tex_ = tex; }
}
void Renderer::push(float x, float y, float u, float v, Color c) {
    verts_.push_back({x, y, u, v, c.r, c.g, c.b, c.a});
}
void Renderer::quad(const Rect& r, Color c) {
    set_texture(white_);
    push(r.x, r.y, 0,0, c);       push(r.x+r.w, r.y, 1,0, c);   push(r.x+r.w, r.y+r.h, 1,1, c);
    push(r.x, r.y, 0,0, c);       push(r.x+r.w, r.y+r.h, 1,1, c); push(r.x, r.y+r.h, 0,1, c);
}
void Renderer::textured(const Rect& r, const Texture& t, Color tint) {
    set_texture(t.id());
    push(r.x, r.y, 0,0, tint);       push(r.x+r.w, r.y, 1,0, tint);   push(r.x+r.w, r.y+r.h, 1,1, tint);
    push(r.x, r.y, 0,0, tint);       push(r.x+r.w, r.y+r.h, 1,1, tint); push(r.x, r.y+r.h, 0,1, tint);
}
void Renderer::textured_cover(const Rect& r, const Texture& t, Color tint) {
    // "cover" crop: scale to fill r, center-crop the overflow via UVs.
    float tw = t.width(), th = t.height();
    float ra = r.w / r.h, ta = tw / th;
    float u0=0, v0=0, u1=1, v1=1;
    if (ta > ra) { float s = ra / ta; u0 = (1-s)/2; u1 = 1-u0; }
    else         { float s = ta / ra; v0 = (1-s)/2; v1 = 1-v0; }
    set_texture(t.id());
    push(r.x, r.y, u0,v0, tint);     push(r.x+r.w, r.y, u1,v0, tint);   push(r.x+r.w, r.y+r.h, u1,v1, tint);
    push(r.x, r.y, u0,v0, tint);     push(r.x+r.w, r.y+r.h, u1,v1, tint); push(r.x, r.y+r.h, u0,v1, tint);
}
void Renderer::quad4(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3, Color c) {
    set_texture(white_);
    push(x0,y0,0,0,c); push(x1,y1,1,0,c); push(x2,y2,1,1,c);
    push(x0,y0,0,0,c); push(x2,y2,1,1,c); push(x3,y3,0,1,c);
}
void Renderer::textured_quad4(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3,
                              const Texture& t, Color tint) {
    set_texture(t.id());
    push(x0,y0,0,0,tint); push(x1,y1,1,0,tint); push(x2,y2,1,1,tint);
    push(x0,y0,0,0,tint); push(x2,y2,1,1,tint); push(x3,y3,0,1,tint);
}
void Renderer::text(const Font& f, const std::string& s, float x, float y, Color c) {
    set_texture(f.atlas()->id());
    float aw = (float)f.atlas_w_, ah = (float)f.atlas_h_;
    float pen_x = x, pen_y = y + f.ascent_;   // y is top of line
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_next(s, i);
        const Font::Glyph* g = f.glyph(cp);   // may rasterize into the atlas now
        if (!g) continue;                     // unsupported (e.g. emoji) — skip
        float x0 = pen_x + g->xoff, y0 = pen_y + g->yoff;
        float x1 = x0 + g->w, y1 = y0 + g->h;
        float u0 = g->x0/aw, v0 = g->y0/ah, u1 = g->x1/aw, v1 = g->y1/ah;
        push(x0,y0,u0,v0,c); push(x1,y0,u1,v0,c); push(x1,y1,u1,v1,c);
        push(x0,y0,u0,v0,c); push(x1,y1,u1,v1,c); push(x0,y1,u0,v1,c);
        pen_x += g->xadv;
    }
}
void Renderer::flush() {
    if (verts_.empty()) return;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts_.size()*sizeof(V), verts_.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(V),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(V),(void*)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(V),(void*)16);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cur_tex_ ? cur_tex_ : white_);
    glDrawArrays(GL_TRIANGLES, 0, (int)verts_.size());
    verts_.clear();
}
void Renderer::end() { flush(); }

} // namespace gfx
