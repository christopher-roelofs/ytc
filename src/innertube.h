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

// One result row — a video, a channel, a playlist, or a community post.
struct SearchResult {
    enum class Kind { Video, Channel, Playlist, Post };
    Kind kind = Kind::Video;
    std::string video_id;         // video only
    std::string playlist_id;      // playlist only ("PL...", never "RD..." mixes)
    std::string channel_id;       // video: uploader's channel; channel: the channel
    std::string title;            // video title, or channel name
    std::string author;           // video only: uploader name
    std::string length_text;      // video: "10:35"
    std::string view_count_text;  // video: "36,247,182 views"
    std::string published_text;   // video: "11 years ago" / RSS ISO
    std::string subs_text;        // channel: "21.1M subscribers"
    std::string thumbnail_url;    // video: mqdefault; channel: avatar
    long length_seconds = 0;
    bool is_short = false;        // YouTube Short (reel) — typed by the API
    std::string post_text;        // post only: full text (title holds a preview);
                                  // video_id set = the post has an attached video
    bool is_channel() const { return kind == Kind::Channel; }
    bool is_playlist() const { return kind == Kind::Playlist; }
    bool is_post() const { return kind == Kind::Post; }
};

struct VideoInfo {
    std::string video_id;
    std::string title;
    std::string author;
    std::string description;      // full plain-text description (videoDetails)
    long length_seconds = 0;
    std::string status;           // playabilityStatus, "OK" on success
    std::string status_reason;    // populated on failure (human-readable)
    std::string resolved_client;  // which fingerprint succeeded
    std::string user_agent;       // UA that must be reused when fetching the URLs
    std::vector<Format> formats;  // progressive + adaptive, video and audio
    std::optional<std::string> hls_manifest_url;
    bool is_live = false;         // currently broadcasting
    bool is_upcoming = false;     // scheduled premiere not yet started
    bool made_for_kids = false;   // kids content => restricted (paced) delivery

    bool ok() const { return status == "OK"; }
    // Best video-only format under the given policy (resolution-primary,
    // codec_priority as allowlist + tiebreak). Returns nullptr if none match.
    const Format* best_video(const VideoPrefs& prefs = {}) const;
    // Best audio-only format under the given policy.
    const Format* best_audio(const AudioPrefs& prefs = {}) const;
};

// Full metadata for one channel (from its /browse page).
struct ChannelInfo {
    bool ok = false;
    std::string channel_id;
    std::string name;
    std::string handle;             // "@veritasium"
    std::string subs_text;          // "21.1M subscribers"
    std::string video_count_text;   // "528 videos"
    std::string description;
    std::string avatar_url;         // channel avatar (largest thumbnail)
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

    // A page of results plus a continuation token for the next page.
    struct Feed {
        std::vector<SearchResult> items;
        std::string continuation;   // "" when there are no more pages
        std::string endpoint;       // "search" or "browse" (which API to continue on)
        std::string channel_id;     // set for channel feeds (hint for lockup parsing)
        bool ok = false;
    };

    // Full-text search. Uses the search_client fingerprint (WEB by default,
    // which returns clean metadata without a bot wall). Returns video rows.
    std::vector<SearchResult> search(const std::string& query, int max_results = 20);

    // Paginated variants: initial page + continuation token, then continue_feed().
    Feed search_feed(const std::string& query);
    Feed channel_feed(const std::string& channel_id);          // channel's Videos tab
    Feed channel_shorts_feed(const std::string& channel_id);   // channel's Shorts tab
    Feed channel_playlists_feed(const std::string& channel_id);// channel's Playlists tab
    Feed channel_posts_feed(const std::string& channel_id);    // channel's Community posts
    Feed channel_all_feed(const std::string& channel_id);      // channel home/featured (mixed)
    Feed playlist_feed(const std::string& playlist_id);        // a playlist's videos
    Feed continue_feed(const Feed& feed);               // next page (never throws)

    // "Home": merged latest uploads (videos + Shorts) from the favorite channels via
    // the Innertube tabs (rich metadata), dated/ordered via each channel's RSS.
    // channel_ids empty -> favorites from channels.json. Never throws.
    std::vector<SearchResult> home_feed(std::vector<std::string> channel_ids = {},
                                        int max_results = 60);
    // Playlists from all favorite channels (parallel; grouped per channel). Never throws.
    std::vector<SearchResult> home_playlists(std::vector<std::string> channel_ids = {});

    // "Latest" feed: fetch each channel's public RSS (no login/key), merge newest
    // first. channel_ids empty -> read them from config/channels.json (next to the
    // clients config). Never throws (network failure -> fewer/no rows).
    std::vector<SearchResult> latest(std::vector<std::string> channel_ids = {},
                                     int max_results = 40);

    // Full channel metadata (subs, video count, description) from /browse.
    ChannelInfo channel_info(const std::string& channel_id);

    // Favorite channels (drive the Latest feed), persisted in channels.json.
    std::vector<std::string> favorite_channel_ids();
    bool add_favorite(const std::string& channel_id, const std::string& name); // true if newly added
    bool remove_favorite(const std::string& channel_id);                        // true if removed

    // Watch Later list, persisted in watch_later.json. Entries can be videos OR
    // playlists (is_playlist + enough metadata to rebuild the tile).
    std::vector<std::string> watch_later_ids();      // ids of ALL entries (either kind)
    bool add_watch_later(const std::string& id, const std::string& title,
                         bool is_playlist = false, const std::string& thumb = "",
                         const std::string& author = "", const std::string& count = "");
    bool remove_watch_later(const std::string& id);
    std::vector<SearchResult> watch_later();         // ready-to-show tiles

    // Stored lists as (id, name/title) pairs, for building the menu views.
    std::vector<std::pair<std::string,std::string>> favorites();     // (channel_id, name)
    std::vector<std::pair<std::string,std::string>> history();       // (video_id, title), recent first

    // Watch history, persisted in history.json (most-recent first, deduped, capped).
    void add_history(const std::string& video_id, const std::string& title);

    // Per-video resume positions, persisted in resume.json (for ask-to-resume).
    double resume_pos(const std::string& video_id);              // seconds, 0 if none
    void   set_resume_pos(const std::string& video_id, double seconds);
    void   clear_resume_pos(const std::string& video_id);        // finished -> forget

    // App settings, persisted in settings.json (key -> int).
    int  setting_int(const std::string& key, int def);
    void set_setting_int(const std::string& key, int value);

    // Restricted-delivery (paced/kids) detection for the "hide restricted" filter.
    // check_video_restricted: ONE lightweight /player call (IOS client, the one that
    // exposes the made-for-kids marker) -> 1 restricted, 0 clean, -1 unknown (don't
    // cache). Thread-safe (local HttpClient). restricted_cache() / set_restricted_cached
    // persist per-CHANNEL verdicts in restricted_cache.json so checks aren't repeated.
    int check_video_restricted(const std::string& video_id);
    // Full plain-text description for one video ("" on failure). Thread-safe
    // (local HttpClient); one lightweight /player call.
    std::string video_description(const std::string& video_id);
    // Playlist description via its VL /browse (playlistMetadataRenderer). Thread-safe.
    std::string playlist_description(const std::string& playlist_id);
    std::vector<std::pair<std::string, bool>> restricted_cache();
    void set_restricted_cached(const std::string& channel_id, bool restricted);

    // True once we've obtained a session token — a proxy for "network reachable"
    // (every Innertube call needs it; it's empty until the first success).
    bool has_visitor_data() const { return !visitor_data_.empty(); }

    // visitorData is cached after first fetch; call to force refresh (e.g. on 403).
    void refresh_visitor_data();

private:
    HttpClient http_;
    std::string api_key_;
    std::string config_dir_;      // dir of clients.json (to find channels.json)
    std::vector<ClientFingerprint> clients_;
    ClientFingerprint search_client_;
    bool has_search_client_ = false;
    std::string visitor_data_;

    std::string ensure_visitor_data();
    VideoInfo try_client(const ClientFingerprint& fp, const std::string& video_id);
    std::vector<std::string> client_headers(const ClientFingerprint& fp) const;
    // One /browse call (thread-safe: local HttpClient; only READS visitor_data_,
    // so warm it via ensure_visitor_data() before calling from worker threads).
    // params may be null (playlists); chan_hint attributes lockups to a channel.
    Feed browse_tab(const std::string& browse_id, const char* params,
                    const std::string& chan_hint);
};

} // namespace yt
