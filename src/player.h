// Video playback via libmpv rendering into the app's shared GLES2 context.
// The header is mpv-free so ui.cpp/App can use it whether or not libmpv is
// present; the implementation (player.cpp) is real only when YTNATIVE_HAVE_MPV
// is defined, otherwise a no-op stub (lets the UI build/iterate without mpv).
#pragma once
#include <string>
#include <memory>

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
    bool play(const std::string& video_url, const std::string& audio_url,
              const std::string& user_agent);
    void stop();

    // Pump mpv events; returns false when playback has ended (EOF/error).
    bool pump();
    // Render the current video frame into the bound framebuffer (fb_w x fb_h).
    void render(int fb_w, int fb_h);

    void toggle_pause();
    void seek_relative(double seconds);

    bool active() const;      // a file is loaded
    bool paused() const;
    double position() const;  // seconds
    double duration() const;  // seconds, 0 if unknown

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ui
