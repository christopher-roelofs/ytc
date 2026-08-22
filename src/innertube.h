// Innertube client: resolves a YouTube videoId into playable stream URLs
// without yt-dlp, a JS engine, or the device ffmpeg binary.
//
// Flow (verified 2026-08-22):
//   1. POST /youtubei/v1/visitor_id  -> session visitorData token
//   2. POST /youtubei/v1/player      -> streamingData with direct URLs
// Client fingerprints come from config/clients.json and are tried in order
// until one returns playabilityStatus == OK.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include "http.h"

namespace yt {

// Codec families we care about for selection. Video: H264 (broadest hw support,
// but YouTube caps it at 1080p), VP9 and AV1 (needed for 1440p/4K/higher
// efficiency, want hardware decode). Audio: AAC (m4a, most compatible) and Opus.
enum class Codec { H264, VP9, AV1, AAC, Opus, Unknown };

const char* codec_name(Codec c);
Codec codec_from_string(const std::string& codec_str);
// Parse "h264"/"vp9"/"av1"/"aac"/"opus" (case-insensitive); Unknown otherwise.
Codec codec_from_pref(const std::string& s);

struct Format {
    int itag = 0;
    std::string mime_type;        // e.g. "video/mp4; codecs=\"avc1.4d401e\""
    std::string codec;            // parsed codec string, e.g. "avc1.4d401e"
    Codec codec_family = Codec::Unknown;
    std::string quality_label;    // "1080p60", "480p", "" for audio
    int width = 0, height = 0, fps = 0;
    long bitrate = 0;             // average bitrate, bits/s
    long content_length = 0;      // bytes, 0 if unknown
    std::string audio_quality;    // "AUDIO_QUALITY_MEDIUM" etc, "" for video
    std::string url;              // direct, ready-to-play (empty if ciphered)
    bool has_audio = false;
    bool has_video = false;
    bool is_hls = false;          // true if this came from an HLS manifest client
};

// Video selection policy. codec_priority is BOTH an allowlist and a ranking:
// only listed codecs are eligible, earlier = preferred at equal resolution.
// Height is the primary key, so with all three codecs allowed a 4K VP9 beats a
// 1080p H264 — giving "allow all resolutions" while still preferring H264 ties.
struct VideoPrefs {
    int max_height = 0;           // 0 = unlimited
    std::vector<Codec> codec_priority = {Codec::H264, Codec::VP9, Codec::AV1};
};

struct AudioPrefs {
    std::vector<Codec> codec_priority = {Codec::AAC, Codec::Opus};
};

// One search result row (video only; channels/playlists are skipped for now).
struct SearchResult {
    std::string video_id;
    std::string title;
    std::string author;
    std::string length_text;      // "10:35"
    std::string view_count_text;  // "36,247,182 views"
    std::string published_text;   // "11 years ago"
    std::string thumbnail_url;    // best available
    long length_seconds = 0;      // parsed from length_text when possible
};

struct VideoInfo {
    std::string video_id;
    std::string title;
    std::string author;
    long length_seconds = 0;
    std::string status;           // playabilityStatus, "OK" on success
    std::string status_reason;    // populated on failure (human-readable)
    std::string resolved_client;  // which fingerprint succeeded
    std::string user_agent;       // UA that must be reused when fetching the URLs
    std::vector<Format> formats;  // progressive + adaptive, video and audio
    std::optional<std::string> hls_manifest_url;
    bool is_live = false;         // currently broadcasting
    bool is_upcoming = false;     // scheduled premiere not yet started

    bool ok() const { return status == "OK"; }
    // Best video-only format under the given policy (resolution-primary,
    // codec_priority as allowlist + tiebreak). Returns nullptr if none match.
    const Format* best_video(const VideoPrefs& prefs = {}) const;
    // Best audio-only format under the given policy.
    const Format* best_audio(const AudioPrefs& prefs = {}) const;
};

struct ClientFingerprint {
    std::string name;
    bool enabled = true;
    bool js_less = true;
    std::string innertube_client_name;
    std::string innertube_client_version;
    int client_name_id = 0;
    std::string user_agent;
    std::string context_json;     // the "client" object, serialized
};

class Innertube {
public:
    // Loads and parses config/clients.json. Throws on parse error.
    explicit Innertube(const std::string& config_path);

    // Resolve a videoId to stream URLs, trying each enabled client in order.
    VideoInfo resolve(const std::string& video_id);

    // Full-text search. Uses the search_client fingerprint (WEB by default,
    // which returns clean metadata without a bot wall). Returns video rows.
    std::vector<SearchResult> search(const std::string& query, int max_results = 20);

    // visitorData is cached after first fetch; call to force refresh (e.g. on 403).
    void refresh_visitor_data();

private:
    HttpClient http_;
    std::string api_key_;
    std::vector<ClientFingerprint> clients_;
    ClientFingerprint search_client_;
    bool has_search_client_ = false;
    std::string visitor_data_;

    std::string ensure_visitor_data();
    VideoInfo try_client(const ClientFingerprint& fp, const std::string& video_id);
    std::vector<std::string> client_headers(const ClientFingerprint& fp) const;
};

} // namespace yt
