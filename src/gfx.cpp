#include "gfx.h"
#include <SDL.h>
#include <SDL_opengles2.h>
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

// ---------- Font ----------
std::unique_ptr<Font> Font::load(const std::string& path, float pixel_h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    std::vector<uint8_t> ttf((std::istreambuf_iterator<char>(f)), {});
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)))
        return nullptr;

    const int AW = 512, AH = 512;
    std::vector<uint8_t> bitmap(AW * AH, 0);
    stbtt_bakedchar cd[95];
    stbtt_BakeFontBitmap(ttf.data(), 0, pixel_h, bitmap.data(), AW, AH, 32, 95, cd);

    // Expand single-channel coverage to RGBA (white, alpha=coverage).
    std::vector<uint8_t> rgba(AW * AH * 4);
    for (int i = 0; i < AW * AH; ++i) {
        rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = bitmap[i];
    }
    auto font = std::make_unique<Font>();
    font->atlas_ = Texture::from_rgba(rgba.data(), AW, AH);
    font->atlas_w_ = AW; font->atlas_h_ = AH;
    font->pixel_h_ = pixel_h;

    float scale = stbtt_ScaleForPixelHeight(&info, pixel_h);
    int asc, desc, gap; stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    font->ascent_ = asc * scale;
    font->line_h_ = (asc - desc + gap) * scale;

    font->glyphs_.resize(95);
    for (int i = 0; i < 95; ++i) {
        auto& g = font->glyphs_[i];
        g.x0 = cd[i].x0; g.y0 = cd[i].y0; g.x1 = cd[i].x1; g.y1 = cd[i].y1;
        g.xoff = cd[i].xoff; g.yoff = cd[i].yoff; g.xadv = cd[i].xadvance;
        g.w = cd[i].x1 - cd[i].x0; g.h = cd[i].y1 - cd[i].y0;
    }
    return font;
}
const Font::Glyph* Font::glyph(char c) const {
    if (c < 32 || c > 126) return &glyphs_['?' - 32];
    return &glyphs_[c - 32];
}
float Font::text_width(const std::string& s) const {
    float w = 0;
    for (char c : s) w += glyph(c)->xadv;
    return w;
}
std::string Font::ellipsize(const std::string& s, float max_w) const {
    if (text_width(s) <= max_w) return s;
    float ell = text_width("...");
    std::string out;
    float w = 0;
    for (char c : s) {
        float cw = glyph(c)->xadv;
        if (w + cw + ell > max_w) break;
        out += c; w += cw;
    }
    return out + "...";
}

// ---------- Window ----------
std::unique_ptr<Window> Window::create(int w, int h, const std::string& title,
                                       const std::string& driver) {
    // Set the video driver via env var (portable across SDL versions; the
    // SDL_HINT_VIDEODRIVER macro only exists in SDL >= 2.0.22).
    if (!driver.empty()) setenv("SDL_VIDEODRIVER", driver.c_str(), 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return nullptr;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const char* drv = SDL_GetCurrentVideoDriver();
    bool headless = drv && std::strcmp(drv, "offscreen") == 0;
    Uint32 mode = getenv("YTNATIVE_WINDOWED") ? (SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE)
                                              : SDL_WINDOW_FULLSCREEN_DESKTOP;
    Uint32 flags = SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : mode);
    SDL_Window* win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!win) { std::fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return nullptr; }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { std::fprintf(stderr, "GL_CreateContext: %s\n", SDL_GetError()); return nullptr; }
    SDL_GL_MakeCurrent(win, gl);

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
    const char* vs =
        "attribute vec2 aPos; attribute vec2 aUV; attribute vec4 aCol;"
        "uniform vec2 uProj; varying vec2 vUV; varying vec4 vCol;"
        "void main(){ vUV=aUV; vCol=aCol;"
        " gl_Position=vec4(aPos.x/uProj.x*2.0-1.0, 1.0-aPos.y/uProj.y*2.0, 0.0, 1.0);}";
    const char* fs =
        "precision mediump float; varying vec2 vUV; varying vec4 vCol;"
        "uniform sampler2D uTex;"
        "void main(){ gl_FragColor = texture2D(uTex, vUV) * vCol; }";
    prog_ = glCreateProgram();
    unsigned v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
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
void Renderer::text(const Font& f, const std::string& s, float x, float y, Color c) {
    set_texture(f.atlas()->id());
    float aw = 512, ah = 512;
    float pen_x = x, pen_y = y + f.ascent_;   // y is top of line
    for (char ch : s) {
        const Font::Glyph* g = f.glyph(ch);
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
