// YouTube casting (Option B): discover screens on the LAN, play a video on the
// TV's own YouTube app via the Lounge API, and send remote commands. No SDK, no
// account; reuses our HttpClient. Two device classes are auto-detected:
//   - DIAL-YouTube TVs (Samsung/LG/Roku): screenId comes free from DIAL -> no code.
//   - Google Cast / Android-TV (Shield/Chromecast): native app needs a one-time
//     "Link with TV code"; once paired, the screenId is stored.
#pragma once
#include <string>
#include <vector>

namespace yt {

class Cast {
public:
    explicit Cast(const std::string& config_dir);

    enum class Kind { DialYouTube, CastDevice, Unknown };
    struct Device {
        std::string name;        // friendly name ("[TV] Samsung...", "SHIELD")
        std::string ip;
        std::string app_url;     // DIAL Application-URL
        std::string screen_id;   // known for DIAL-YouTube or a previously-paired device
        Kind kind = Kind::Unknown;
        bool paired = false;     // screenId came from our stored pairings
        // A Cast/Android-TV device with no stored screenId must be paired by TV code.
        // A DIAL-YouTube TV is launchable even with no screenId yet, so it's "ready".
        bool needs_code() const { return screen_id.empty() && kind != Kind::DialYouTube; }
    };

    // Blocking SSDP discovery (~timeout_ms). Classifies each device, fills screen_id
    // for DIAL-YouTube ones, and merges in any previously-paired screenIds.
    std::vector<Device> discover(int timeout_ms = 3000);

    // Exchange a TV "Link with TV code" (spaces ok) for a screenId; persists it under
    // `name`. Returns the screenId ("" on failure).
    std::string pair_with_code(const std::string& code, const std::string& name = "");

    // A live cast session (returned by play()); carries the ids for follow-up commands.
    struct Session {
        std::string screen_id, lounge_token, sid, gsession;
        int rid = 0;
        bool ok = false;
    };
    // Play a video on a device: lounge token -> bind -> setPlaylist (optionally at
    // start_seconds, to hand off the current position). For a DIAL-YouTube TV whose
    // app isn't running yet, launches it first to obtain a screenId. Session.ok ==
    // false on failure (e.g. a Cast device that hasn't been paired by code).
    Session play(const Device& dev, const std::string& video_id, int start_seconds = 0);
    // Follow-up remote command on a live session. type: play|pause|stop|next|previous|
    // seekTo (arg = seconds) | setVolume (arg = 0..100). Returns false on failure.
    bool command(Session& s, const std::string& type, double arg = 0);

    // Previously-paired screens (from cast.json).
    std::vector<Device> paired() const;
    void forget(const std::string& screen_id);

private:
    std::string config_dir_;
    std::string sender_id_;   // our persistent 128-bit sender id (generated once)

    std::string lounge_token(const std::string& screen_id);   // get_lounge_token_batch
    bool bind(Session& s);                                     // establish SID/gsession
    bool send_cmd(Session& s, const std::string& body);        // one bc/bind command POST
    std::string bind_qs(const Session& s, bool with_session);  // build the query string
    void load_pairings(std::vector<Device>& out) const;
    void save_pairing(const std::string& screen_id, const std::string& name);
    void ensure_sender_id();
};

} // namespace yt
