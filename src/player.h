// Video playback via libmpv rendering into the app's shared GLES2 context.
// The header is mpv-free so ui.cpp/App can use it whether or not libmpv is
// present; the implementation (player.cpp) is real only when YTNATIVE_HAVE_MPV
// is defined, otherwise a no-op stub (lets the UI build/iterate without mpv).
#pragma once
#include <string>
#include <memory>
#include <vector>

namespace ui {

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // True if this build actually has libmpv.
    static bool available();

    // Start playing. video_url is the (video-only) DASH stream; audio_url is the
    // separate audio stream (mpv muxes/syncs it); user_agent must match the
    // fingerprint that resolved the URLs. Returns false on failure.
    // paced: restricted delivery — cap demuxer readahead so prefetch never outruns
    // the server's sliding window (which only advances with paced consumption).
    bool play(const std::string& video_url, const std::string& audio_url,
              const std::string& user_agent, double start_seconds = 0,
              bool paced = false);
    void stop();

    // Pump mpv events; returns false when playback has ended (EOF/error).
    bool pump();
    // Render the current video frame into the bound framebuffer (fb_w x fb_h).
    void render(int fb_w, int fb_h);

    void toggle_pause();
    void set_pause(bool paused);
    void seek_relative(double seconds);

    // App-local volume (independent of the OS mixer). 0..150 percent; 100 = the
    // stream's original level, above 100 amplifies quiet videos. Clamped.
    void set_volume(int percent);
    int  volume() const;

    // Hardware-decode mode, applied to the NEXT play() (mpv "hwdec" must be set
    // before init). Empty restores the built-in default. The YTNATIVE_HWDEC env
    // var, if set, always overrides this. Typical: "auto-copy-safe" / "no".
    void set_hwdec(const std::string& mode);

    bool active() const;      // a file is loaded
    bool paused() const;
    double position() const;  // seconds
    double duration() const;  // seconds, 0 if unknown
    double cached_until() const;  // timestamp buffered up to (demuxer cache end), 0 if unknown

    // "Stats for nerds" — a few decode/stream properties as "Label: value" lines.
    std::vector<std::string> stats_lines() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ui
