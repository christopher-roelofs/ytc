// Minimal GLES2 2D UI toolkit: window/context (offscreen or KMSDRM), a batched
// quad+texture renderer, a stb_truetype font atlas, and PNG screenshot capture.
// GLES2 (#version 100) so the exact same path runs on Mali/KMSDRM and desktop.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace gfx {

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    static Color rgb(int hex) {
        return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f,
                (hex & 255) / 255.f, 1.f};
    }
    Color with_a(float alpha) const { return {r, g, b, alpha}; }
};

struct Rect { float x, y, w, h; };

// A GPU texture (RGBA). Owns the GL name.
class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(Texture&&) noexcept;
    Texture& operator=(Texture&&) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    static std::unique_ptr<Texture> from_rgba(const uint8_t* px, int w, int h);
    // Decode PNG/JPEG bytes (stb_image) into a texture. nullptr on failure.
    static std::unique_ptr<Texture> from_encoded(const uint8_t* data, size_t n);
    // Empty (transparent) RGBA texture, for incremental atlases.
    static std::unique_ptr<Texture> create_empty(int w, int h);
    // Upload a sub-rectangle (RGBA). GL thread only.
    void update(int x, int y, int w, int h, const uint8_t* rgba);

    unsigned id() const { return id_; }
    int width() const { return w_; }
    int height() const { return h_; }
private:
    unsigned id_ = 0;
    int w_ = 0, h_ = 0;
};

// UTF-8 aware font with an ON-DEMAND glyph cache: any codepoint the .ttf covers is
// rasterized into the atlas the first time it's drawn (Latin, accents, Cyrillic,
// Greek, punctuation, ... with DejaVu). Missing glyphs: emoji-ish codepoints are
// silently skipped (no more "????"), anything else renders as a tofu box.
class Font {
public:
    static std::unique_ptr<Font> load(const std::string& ttf_path, float pixel_h);
    ~Font();
    float line_height() const { return line_h_; }
    float text_width(const std::string& s) const;
    // Truncate s with an ellipsis to fit max_w (never splits a UTF-8 sequence).
    std::string ellipsize(const std::string& s, float max_w) const;
    struct Glyph { float x0, y0, x1, y1;      // atlas UV (pixels)
                   float xoff, yoff, xadv; int w, h; };
    // Rasterizes + caches on demand. nullptr => skip this codepoint entirely.
    const Glyph* glyph(uint32_t codepoint) const;
    Texture* atlas() const { return atlas_.get(); }
    float pixel_h() const { return pixel_h_; }
private:
    std::unique_ptr<Texture> atlas_;
    std::vector<uint8_t> ttf_;                // font file bytes (fontinfo points here)
    void* info_ = nullptr;                    // stbtt_fontinfo* (opaque; gfx.cpp only)
    mutable std::unordered_map<uint32_t, Glyph> glyphs_;
    mutable int pen_x_ = 1, pen_y_ = 1, row_h_ = 0;   // shelf packer cursor
    float line_h_ = 0, ascent_ = 0, pixel_h_ = 0, scale_ = 0;
    int atlas_w_ = 0, atlas_h_ = 0;
    friend class Renderer;
};

class Window {
public:
    // driver: "" (auto), "offscreen", "kmsdrm", "x11". Creates a GLES2 context.
    static std::unique_ptr<Window> create(int w, int h, const std::string& title,
                                          const std::string& driver = "");
    ~Window();
    int width() const { return w_; }
    int height() const { return h_; }
    void refresh_size();   // re-query drawable size (after a resize event)
    void swap();
    // Capture the current framebuffer to a PNG (flips vertically). true on success.
    bool screenshot(const std::string& path) const;
    void* sdl_window() const { return win_; }
private:
    void* win_ = nullptr;   // SDL_Window*
    void* gl_ = nullptr;    // SDL_GLContext
    int w_ = 0, h_ = 0;
};

// Batched 2D renderer. One shader; solid quads use an internal 1x1 white texture.
class Renderer {
public:
    Renderer();
    ~Renderer();
    void begin(int fb_w, int fb_h);        // set ortho + clear state
    void clear(Color c);
    void quad(const Rect& r, Color c);     // filled rectangle
    void textured(const Rect& r, const Texture& t, Color tint = {});
    // Draw a texture cropped to preserve aspect ("cover" fit) inside r.
    void textured_cover(const Rect& r, const Texture& t, Color tint = {});
    void text(const Font& f, const std::string& s, float x, float y, Color c);
    // Arbitrary 4-corner quad (TL,TR,BR,BL order) — for faux-3D coverflow tiles.
    void quad4(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3, Color c);
    void textured_quad4(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3,
                        const Texture& t, Color tint = {});
    void end();                            // flush
private:
    struct V { float x, y, u, v; float r, g, b, a; };
    void push(float x, float y, float u, float v, Color c);
    void set_texture(unsigned tex);
    void flush();
    std::vector<V> verts_;
    unsigned prog_ = 0, vbo_ = 0, white_ = 0, cur_tex_ = 0;
    int u_proj_ = -1;
    int fb_w_ = 0, fb_h_ = 0;
};

} // namespace gfx
