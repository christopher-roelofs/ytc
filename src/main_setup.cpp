// ytc_setup — one-shot hardware-decode offer, run by YTC.sh before the app.
//
// Detects the device's video decoder (src/hwdetect.h), matches it against the
// shipped data/hwdec_manifest.json, and asks the user whether to download the
// hardware-decode ffmpeg libs (Yes / Not now / Never ask again). On Yes it
// downloads each file, verifies its SHA-256, and installs atomically into
// libs.hwdec/ (download to libs.hwdec.part/, rename when fully verified).
//
// It records the outcome in ./video_decode (the port root — this binary runs
// with GAMEDIR as cwd):
//     soc=/gpu=/decoder=/stateless=/bundle=/hwdec=   detection details (informational)
//     choice=installed|declined|never|builtin
//     manifest=<version>   written on install; YTC.sh re-prompts when the
//                          shipped data/hwdec_version no longer matches
// "Not now" writes no manifest= line, so the prompt returns next launch.
//
// --yes / --never run non-interactively (no window) for testing/scripting.
#include "gfx.h"
#include "http.h"
#include "hwdetect.h"
#include "../third_party/json.hpp"
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Minimal SHA-256 (FIPS 180-4). Self-contained so the binary needs no TLS lib
// symbols beyond what HttpClient already links.
// ---------------------------------------------------------------------------
namespace sha256 {
struct Ctx { uint32_t h[8]; uint64_t len = 0; uint8_t buf[64]; size_t n = 0; };
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void init(Ctx& c) {
    static const uint32_t H0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c.h, H0, sizeof H0); c.len = 0; c.n = 0;
}
static void block(Ctx& c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4] << 24 | p[i*4+1] << 16 | p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c.h[0],b=c.h[1],d0=c.h[2],d=c.h[3],e=c.h[4],f=c.h[5],g=c.h[6],h=c.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t mj = (a & b) ^ (a & d0) ^ (b & d0);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=d0; d0=b; b=a; a=t1+t2;
    }
    c.h[0]+=a; c.h[1]+=b; c.h[2]+=d0; c.h[3]+=d; c.h[4]+=e; c.h[5]+=f; c.h[6]+=g; c.h[7]+=h;
}
static void update(Ctx& c, const uint8_t* p, size_t n) {
    c.len += n;
    while (n) {
        size_t take = 64 - c.n; if (take > n) take = n;
        memcpy(c.buf + c.n, p, take); c.n += take; p += take; n -= take;
        if (c.n == 64) { block(c, c.buf); c.n = 0; }
    }
}
static std::string hex_final(Ctx& c) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80; update(c, &pad, 1);
    uint8_t z = 0; while (c.n != 56) update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8*i));
    update(c, lb, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) snprintf(out + i*8, 9, "%08x", c.h[i]);
    return out;
}
static std::string of_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    Ctx c; init(c);
    char buf[65536];
    while (f.read(buf, sizeof buf) || f.gcount())
        update(c, (const uint8_t*)buf, (size_t)f.gcount());
    return hex_final(c);
}
} // namespace sha256

// ---------------------------------------------------------------------------

// Everything here is for YTC.sh (prompt suppression, lib path) and for humans
// reading hardware reports. The app does NOT read this file — it resolves the
// same manifest itself against the live hardware and loaded libs.
static void write_state(const hwdetect::Info& hw, const std::string& choice,
                        const std::string& manifest_ver,
                        const hwdetect::Bundle& bundle = {}) {
    std::ofstream o("video_decode");
    o << "soc=" << hw.soc << "\n" << "gpu=" << hw.gpu << "\n"
      << "decoder=" << hw.decoder << "\n";
    if (!hw.stateless.empty()) o << "stateless=" << hw.stateless << "\n";
    if (bundle.valid()) o << "bundle=" << bundle.key << "\n"
                          << "hwdec=" << bundle.hwdec << "\n";
    o << "choice=" << choice << "\n";
    if (!manifest_ver.empty()) o << "manifest=" << manifest_ver << "\n";
}

static std::string font_path() {
    const char* cands[] = {"data/DejaVuSans.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
    for (const char* c : cands) { std::ifstream f(c); if (f) return c; }
    return cands[0];
}

int main(int argc, char** argv) {
    bool auto_yes = false, auto_never = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--yes"))   auto_yes = true;
        if (!strcmp(argv[i], "--never")) auto_never = true;
    }

    hwdetect::Info hw = hwdetect::detect();
    std::fprintf(stderr, "[setup] soc=%s gpu=%s decoder=%s stateless=%s\n",
                 hw.soc.c_str(), hw.gpu.c_str(), hw.decoder.c_str(),
                 hw.stateless.c_str());

    // Resolve the bundle through the shared manifest contract (same resolver the
    // app uses): the first bundle whose detect primitive this device satisfies.
    // None (no decode hardware, or nothing we can drive) -> record unsupported
    // so YTC.sh never asks again on this device.
    std::string manifest_ver;
    hwdetect::Bundle bundle = hwdetect::pick_bundle(hw, "data/hwdec_manifest.json", &manifest_ver);
    if (manifest_ver.empty()) { std::fprintf(stderr, "[setup] bad/missing manifest\n"); return 1; }
    if (!bundle.valid()) { write_state(hw, "never", "unsupported"); return 0; }
    if (bundle.files.empty()) {
        // The matched backend ships in the base libs (e.g. v4l2m2m): nothing to
        // download, nothing to ask. Record it (manifest= keeps YTC.sh quiet).
        write_state(hw, "builtin", manifest_ver, bundle);
        return 0;
    }
    const std::vector<hwdetect::BundleFile>& files = bundle.files;
    long long total_size = bundle.total_size;
    std::fprintf(stderr, "[setup] bundle=%s hwdec=%s check=%s\n",
                 bundle.key.c_str(), bundle.hwdec.c_str(), bundle.check.c_str());

    // ---- decide (UI or flags) ----
    int choice = -1;   // 0 yes, 1 not now, 2 never
    if (auto_yes) choice = 0;
    else if (auto_never) choice = 2;

    gfx::Window* win = nullptr;
    std::unique_ptr<gfx::Window> win_owned;
    std::unique_ptr<gfx::Renderer> rn;
    std::unique_ptr<gfx::Font> font_big, font_body;
    auto open_ui = [&]() {
        if (win) return true;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) return false;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
        win_owned = gfx::Window::create(1280, 720, "YTC Setup");
        if (!win_owned) return false;
        win = win_owned.get();
        rn = std::make_unique<gfx::Renderer>();
        float s = win->height() / 720.f;
        font_big  = gfx::Font::load(font_path(), 34 * s);
        font_body = gfx::Font::load(font_path(), 24 * s);
        return font_big && font_body;
    };

    const char* opts[3] = {"Yes, download now", "Not now", "No, never ask again"};
    int sel = 0;
    std::string status;     // bottom line during download
    float progress = -1;    // 0..1 while downloading

    auto draw = [&](bool downloading) {
        if (!win) return;
        int W = win->width(), H = win->height();
        float s = H / 720.f;
        rn->begin(W, H);
        rn->clear(gfx::Color::rgb(0x101418));
        float x = 90 * s, y = 80 * s;
        rn->text(*font_big, "Hardware Video Decode", x, y, {1,1,1,1});
        y += 70 * s;
        char line[256];
        snprintf(line, sizeof line, "This device can decode video in hardware (%s).",
                 hw.decoder.c_str());
        rn->text(*font_body, line, x, y, {0.85f,0.85f,0.85f,1}); y += 40 * s;
        snprintf(line, sizeof line,
                 "Download the decoder libraries (%.1f MB)? Playback will use less",
                 total_size / 1048576.0);
        rn->text(*font_body, line, x, y, {0.85f,0.85f,0.85f,1}); y += 34 * s;
        rn->text(*font_body, "battery and run cooler. You can change this later.",
                 x, y, {0.85f,0.85f,0.85f,1});
        y += 70 * s;
        if (!downloading) {
            for (int i = 0; i < 3; ++i) {
                bool cur = i == sel;
                if (cur) rn->quad({x - 16*s, y - 8*s, 560*s, 48*s}, gfx::Color::rgb(0x2a3542));
                rn->text(*font_body, opts[i], x, y, cur ? gfx::Color{1,1,1,1}
                                                        : gfx::Color{0.6f,0.6f,0.6f,1});
                y += 56 * s;
            }
            rn->text(*font_body, "Up/Down: choose    A: confirm", x, (float)H - 70*s,
                     {0.5f,0.5f,0.5f,1});
        } else {
            rn->text(*font_body, status, x, y, {0.9f,0.9f,0.9f,1});
            y += 44 * s;
            if (progress >= 0) {
                float bw = 560 * s;
                rn->quad({x, y, bw, 18*s}, gfx::Color::rgb(0x2a3542));
                rn->quad({x, y, bw * progress, 18*s}, gfx::Color::rgb(0x3d8bfd));
            }
        }
        rn->end();
        win->swap();
    };

    if (choice < 0) {
        if (!open_ui()) { std::fprintf(stderr, "[setup] no UI available\n"); return 1; }
        bool done = false;
        while (!done) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                int dir = 0; bool activate = false;
                if (e.type == SDL_QUIT) { choice = 1; done = true; }
                else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:   dir = -1; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: dir = +1; break;
                        case SDL_CONTROLLER_BUTTON_A:         activate = true; break;
                        case SDL_CONTROLLER_BUTTON_B: choice = 1; done = true; break;
                    }
                } else if (e.type == SDL_KEYDOWN) {
                    switch (e.key.keysym.sym) {
                        case SDLK_UP:     dir = -1; break;
                        case SDLK_DOWN:   dir = +1; break;
                        case SDLK_RETURN: activate = true; break;
                        case SDLK_ESCAPE: choice = 1; done = true; break;
                    }
                }
                if (dir) sel = (sel + dir + 3) % 3;
                if (activate) { choice = sel; done = true; }
            }
            draw(false);
            SDL_Delay(16);
        }
    }

    if (choice == 1) { write_state(hw, "declined", ""); return 0; }
    if (choice == 2) { write_state(hw, "never", manifest_ver, bundle); return 0; }

    // ---- download + verify + atomic install ----
    mkdir("libs.hwdec.part", 0755);
    bool ok = true;
    HttpClient http;
    for (size_t i = 0; i < files.size() && ok; ++i) {
        const hwdetect::BundleFile& f = files[i];
        std::string dest = "libs.hwdec.part/" + f.name;
        char st[160];
        snprintf(st, sizeof st, "Downloading %s (%zu/%zu)...", f.name.c_str(),
                 i + 1, files.size());
        status = st; progress = 0; draw(true);
        std::fprintf(stderr, "[setup] %s\n", st);
        // Skip re-downloading a leftover that already verifies (resumed setup).
        if (sha256::of_file(dest) != f.sha256) {
            ok = http.download(f.url, dest, {},
                [&](long long dn, long long tot) {
                    if (tot > 0) { progress = (float)dn / (float)tot; draw(true); }
                    return true;
                });
            if (ok && sha256::of_file(dest) != f.sha256) {
                std::fprintf(stderr, "[setup] SHA-256 mismatch: %s\n", f.name.c_str());
                ok = false;
            }
        }
    }
    if (ok) {
        // SONAME symlinks aren't in the download; the files ARE the sonames.
        ::rename("libs.hwdec", "libs.hwdec.old");   // tolerate a previous install
        ok = ::rename("libs.hwdec.part", "libs.hwdec") == 0;
        if (ok) { std::string rm = "rm -rf libs.hwdec.old"; (void)!system(rm.c_str()); }
        else    ::rename("libs.hwdec.old", "libs.hwdec");
    }
    if (ok) {
        write_state(hw, "installed", manifest_ver, bundle);
        status = "Installed. Hardware decode is ready."; progress = -1;
    } else {
        write_state(hw, "declined", "");            // failed: offer again next launch
        status = "Download failed - will offer again next launch."; progress = -1;
    }
    std::fprintf(stderr, "[setup] %s\n", status.c_str());
    if (win) {                                       // show the outcome briefly
        Uint32 until = SDL_GetTicks() + 2200;
        while (SDL_GetTicks() < until) {
            SDL_Event e; while (SDL_PollEvent(&e)) {}
            draw(true); SDL_Delay(16);
        }
    }
    return ok ? 0 : 1;
}
