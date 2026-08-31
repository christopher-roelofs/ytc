#include "innertube.h"
#include "../third_party/json.hpp"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <functional>

// Windows (mingw) lacks POSIX timegm / mkdir(path,mode); map to the Win equivalents.
#if defined(_WIN32)
  #include <direct.h>
  #define timegm _mkgmtime
  static int os_mkdir(const char* p) { return _mkdir(p); }
#else
  static int os_mkdir(const char* p) { return ::mkdir(p, 0755); }
#endif

using json = nlohmann::json;

namespace yt {

static const char* kInnertubeBase = "https://www.youtube.com/youtubei/v1";

const char* codec_name(Codec c) {
    switch (c) {
        case Codec::H264: return "h264";
        case Codec::VP9:  return "vp9";
        case Codec::AV1:  return "av1";
        case Codec::AAC:  return "aac";
        case Codec::Opus: return "opus";
        default:          return "unknown";
    }
}

Codec codec_from_string(const std::string& s) {
    if (s.rfind("avc1", 0) == 0 || s.rfind("avc3", 0) == 0) return Codec::H264;
    if (s.rfind("vp9", 0) == 0 || s.rfind("vp09", 0) == 0)  return Codec::VP9;
    if (s.rfind("av01", 0) == 0)                            return Codec::AV1;
    if (s.rfind("mp4a", 0) == 0)                            return Codec::AAC;
    if (s.rfind("opus", 0) == 0)                            return Codec::Opus;
    return Codec::Unknown;
}

Codec codec_from_pref(const std::string& s) {
    std::string t;
    for (char ch : s) t += (char)std::tolower((unsigned char)ch);
    if (t == "h264" || t == "avc" || t == "avc1") return Codec::H264;
    if (t == "vp9")                               return Codec::VP9;
    if (t == "av1" || t == "av01")                return Codec::AV1;
    if (t == "aac" || t == "m4a" || t == "mp4a")  return Codec::AAC;
    if (t == "opus")                              return Codec::Opus;
    return Codec::Unknown;
}

// Index of a codec in a priority list, or npos-equivalent (size) if absent.
static size_t codec_rank(Codec c, const std::vector<Codec>& pri) {
    for (size_t i = 0; i < pri.size(); ++i) if (pri[i] == c) return i;
    return pri.size();
}

static ClientFingerprint parse_fingerprint(const json& c) {
    ClientFingerprint fp;
    fp.name = c.value("name", "");
    fp.enabled = c.value("enabled", true);
    fp.js_less = c.value("js_less", true);
    fp.innertube_client_name = c.value("innertube_client_name", "");
    fp.innertube_client_version = c.value("innertube_client_version", "");
    fp.client_name_id = c.value("client_name_id", 0);
    fp.user_agent = c.value("user_agent", "");
    fp.context_json = c.at("context").dump();
    return fp;
}

Innertube::Innertube(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f) throw std::runtime_error("cannot open config: " + config_path);
    json cfg = json::parse(f);

    auto slash = config_path.find_last_of('/');
    config_dir_ = slash == std::string::npos ? "." : config_path.substr(0, slash);
    api_key_ = cfg.value("api_key", "");
    for (const auto& c : cfg.at("clients")) {
        if (!c.value("enabled", true)) continue;
        clients_.push_back(parse_fingerprint(c));
    }
    if (clients_.empty())
        throw std::runtime_error("no enabled clients in config");

    if (cfg.contains("search_client")) {
        search_client_ = parse_fingerprint(cfg.at("search_client"));
        has_search_client_ = true;
    }
    if (cfg.contains("download_client")) {
        download_client_ = parse_fingerprint(cfg.at("download_client"));
        has_download_client_ = true;
    }
}

VideoInfo Innertube::resolve_for_download(const std::string& id) {
    if (!has_download_client_) return resolve(id);
    ensure_visitor_data();               // warm the session token first
    return try_client(download_client_, id);
}

std::vector<std::string> Innertube::client_headers(const ClientFingerprint& fp) const {
    return {
        "Content-Type: application/json",
        "User-Agent: " + fp.user_agent,
        "X-YouTube-Client-Name: " + std::to_string(fp.client_name_id),
        "X-YouTube-Client-Version: " + fp.innertube_client_version,
        "X-Goog-Visitor-Id: " + visitor_token()};
}

std::string Innertube::ensure_visitor_data() {
    std::lock_guard<std::mutex> lk(visitor_m_);
    if (visitor_data_.empty()) refresh_visitor_data_locked();
    return visitor_data_;
}
std::string Innertube::visitor_token() const {
    std::lock_guard<std::mutex> lk(visitor_m_);
    return visitor_data_;
}
void Innertube::refresh_visitor_data() {   // public: force a refresh (e.g. on 403)
    std::lock_guard<std::mutex> lk(visitor_m_);
    refresh_visitor_data_locked();
}
void Innertube::set_locale(const std::string& hl, const std::string& gl) {
    std::lock_guard<std::mutex> lk(locale_m_);
    hl_ = hl; gl_ = gl;
}
std::pair<std::string,std::string> Innertube::locale() const {
    std::lock_guard<std::mutex> lk(locale_m_);
    return {hl_, gl_};
}
// Stamp the localized hl/gl onto an Innertube client context.
static void apply_ctx_locale(json& client, const std::pair<std::string,std::string>& lc) {
    client["hl"] = lc.first; client["gl"] = lc.second;
}
void Innertube::refresh_visitor_data_locked() {   // caller holds visitor_m_
    // Use the first client's identity to request a visitor id.
    const auto& fp = clients_.front();
    json body = {
        {"context", {{"client", {
            {"clientName", fp.innertube_client_name},
            {"clientVersion", fp.innertube_client_version}}}}}};
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "User-Agent: " + fp.user_agent,
        "X-YouTube-Client-Name: " + std::to_string(fp.client_name_id),
        "X-YouTube-Client-Version: " + fp.innertube_client_version};
    std::string url = std::string(kInnertubeBase) + "/visitor_id";
    if (!api_key_.empty()) url += "?key=" + api_key_;

    HttpClient http;   // local handle — never share a CURL handle across threads
    auto r = http.post(url, body.dump(), headers);
    if (!r.ok()) throw std::runtime_error("visitor_id HTTP " + std::to_string(r.status));
    auto j = json::parse(r.body, nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error("visitor_id: bad JSON");
    visitor_data_ = j.at("responseContext").value("visitorData", "");
    if (visitor_data_.empty())
        throw std::runtime_error("visitor_id: no visitorData in response");
}

// base64url-decode (no padding needed) — enough to peek inside a format's xtags,
// a small protobuf whose readable strings include acont's "original"/"dubbed".
static std::string b64url_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return out;
}

static void parse_format(const json& f, bool progressive, VideoInfo& out) {
    Format fmt;
    fmt.itag = f.value("itag", 0);
    fmt.mime_type = f.value("mimeType", "");
    fmt.quality_label = f.value("qualityLabel", "");
    fmt.width = f.value("width", 0);
    fmt.height = f.value("height", 0);
    fmt.fps = f.value("fps", 0);
    fmt.audio_quality = f.value("audioQuality", "");
    // Multi-audio (dub) track metadata. audioTrack.id is "<lang>.<type>" where
    // type 4 = the original track, 3 = a dub; xtags decodes to a protobuf whose
    // acont field spells it out ("original"/"dubbed") — trust either signal.
    if (f.contains("audioTrack") && f["audioTrack"].is_object()) {
        const json& at = f["audioTrack"];
        fmt.track_name = at.value("displayName", "");
        fmt.track_default = at.value("audioIsDefault", false);
        std::string id = at.value("id", "");
        auto dot = id.find('.');
        fmt.track_lang = dot == std::string::npos ? id : id.substr(0, dot);
        fmt.track_original = (dot != std::string::npos && id.substr(dot + 1) == "4") ||
                             b64url_decode(f.value("xtags", "")).find("original")
                                 != std::string::npos;
    }
    if (f.contains("bitrate")) fmt.bitrate = f["bitrate"].get<long>();
    if (f.contains("contentLength"))
        fmt.content_length = std::stol(f["contentLength"].get<std::string>());
    fmt.url = f.value("url", "");

    // Parse "video/mp4; codecs=\"avc1.4d401e, mp4a.40.2\"".
    auto mt = fmt.mime_type;
    fmt.has_video = mt.rfind("video/", 0) == 0;
    fmt.has_audio = mt.rfind("audio/", 0) == 0;
    auto cpos = mt.find("codecs=\"");
    if (cpos != std::string::npos) {
        auto start = cpos + 8;
        auto end = mt.find('"', start);
        fmt.codec = mt.substr(start, end - start);
    }
    fmt.codec_family = codec_from_string(fmt.codec);
    if (progressive) { fmt.has_video = true; fmt.has_audio = true; }
    out.formats.push_back(std::move(fmt));
}

VideoInfo Innertube::try_client(const ClientFingerprint& fp,
                                const std::string& video_id) {
    VideoInfo out;
    out.video_id = video_id;
    out.resolved_client = fp.name;
    out.user_agent = fp.user_agent;

    json client = json::parse(fp.context_json);
    client["visitorData"] = ensure_visitor_data();
    apply_ctx_locale(client, locale());

    json body = {
        {"videoId", video_id},
        {"context", {{"client", client}}},
        {"contentCheckOk", true},
        {"racyCheckOk", true}};

    std::string url = std::string(kInnertubeBase) + "/player";
    if (!api_key_.empty()) url += "?key=" + api_key_;

    HttpClient http;   // local handle — resolve() runs on a worker thread
    auto r = http.post(url, body.dump(), client_headers(fp));
    if (!r.ok()) {
        out.status = "HTTP_ERROR";
        out.status_reason = "HTTP " + std::to_string(r.status);
        return out;
    }
    auto j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) {
        out.status = "BAD_JSON";
        return out;
    }
    // Debug: dump the raw /player response for offline analysis.
    if (const char* dump = getenv("YTC_DUMP_PLAYER")) {
        std::string path = std::string(dump) + "/" + video_id + "_" + fp.name + ".json";
        std::ofstream f(path);
        if (f) f << r.body;
    }

    const auto& ps = j.value("playabilityStatus", json::object());
    out.status = ps.value("status", "UNKNOWN");
    out.status_reason = ps.value("reason", "");
    // Made-for-kids marker: playabilityStatus carries disabled miniplayer/offline
    // buttons linking to the "made for kids" help article (answer 9632097). Kids
    // content gets RESTRICTED (paced) delivery — flag it so the player can adapt
    // without needing a deep-range probe. Verified: present for kids videos
    // (Pokemon TV, CoComelon via IOS), absent for normal videos on all clients.
    if (ps.dump().find("9632097") != std::string::npos) out.made_for_kids = true;
    if (out.status != "OK") return out;

    const auto& vd = j.value("videoDetails", json::object());
    out.title = vd.value("title", "");
    out.author = vd.value("author", "");
    out.description = vd.value("shortDescription", "");   // misnomer: FULL description
    out.is_live = vd.value("isLive", false);
    out.is_upcoming = vd.value("isUpcoming", false);
    if (vd.contains("lengthSeconds"))
        out.length_seconds = std::stol(vd["lengthSeconds"].get<std::string>());

    const auto& sd = j.value("streamingData", json::object());
    for (const auto& f : sd.value("formats", json::array()))
        parse_format(f, /*progressive=*/true, out);
    for (const auto& f : sd.value("adaptiveFormats", json::array()))
        parse_format(f, /*progressive=*/false, out);
    if (sd.contains("hlsManifestUrl"))
        out.hls_manifest_url = sd["hlsManifestUrl"].get<std::string>();

    return out;
}

VideoInfo Innertube::resolve(const std::string& video_id) {
    VideoInfo last;
    last.video_id = video_id;
    try {
        for (const auto& fp : clients_) {
            last = try_client(fp, video_id);
            if (last.ok() && (!last.formats.empty() || last.hls_manifest_url))
                return last;
        }
    } catch (const std::exception& e) {
        // Never let a network/parse failure escape into the resolve thread ->
        // std::terminate. Report it as a normal not-OK result.
        std::fprintf(stderr, "[innertube] resolve failed: %s\n", e.what());
        last.status = "NETWORK_ERROR";
        last.status_reason = "Network error — check connection";
    }
    return last; // last attempt carries status/reason for diagnosis
}

// "10:35" or "1:02:03" -> seconds; 0 if unparseable.
static long parse_duration(const std::string& t) {
    long total = 0, cur = 0; bool any = false;
    for (char ch : t) {
        if (ch >= '0' && ch <= '9') { cur = cur * 10 + (ch - '0'); any = true; }
        else if (ch == ':') { total = total * 60 + cur; cur = 0; }
    }
    if (!any) return 0;
    return total * 60 + cur;
}

static std::string run_text(const json& node) {
    if (node.contains("simpleText")) return node["simpleText"].get<std::string>();
    if (node.contains("runs") && node["runs"].is_array() && !node["runs"].empty())
        return node["runs"][0].value("text", "");
    return "";
}

static const json* find_key(const json& node, const char* key);   // defined below
// approx_age_secs is exposed (declared in innertube.h) so the UI can date-sort too.

// Parse a lockupViewModel (YouTube's newer video item, used on channel/home tabs
// and continuations) into a video SearchResult. chan_hint = the channel we're
// viewing (lockups don't carry the uploader id on a channel page).
// Standard/URL-safe base64 decode (accepts both alphabets; ignores padding).
static std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out; int buf = 0, bits = 0;
    for (char c : in) { int v = val(c); if (v < 0) continue;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xff); } }
    return out;
}
// A reel's reelWatchEndpoint.params protobuf carries the uploader's channel id in
// field 23 (a 24-char "UC..." string). Returns "" if absent/malformed.
static std::string reel_params_channel_id(const std::string& params_b64) {
    if (params_b64.empty()) return "";
    std::string b = base64_decode(params_b64);
    size_t i = 0;
    auto varint = [&](uint64_t& v) -> bool {
        v = 0; int s = 0;
        while (i < b.size()) { uint8_t x = (uint8_t)b[i++]; v |= (uint64_t)(x & 0x7f) << s; s += 7;
            if (!(x & 0x80)) return true; }
        return false;
    };
    while (i < b.size()) {
        uint64_t tag; if (!varint(tag)) break;
        int field = (int)(tag >> 3), wire = (int)(tag & 7);
        if (wire == 0) { uint64_t v; if (!varint(v)) break; }
        else if (wire == 2) { uint64_t len; if (!varint(len)) break;
            if (i + len > b.size()) break;
            if (field == 23) return b.substr(i, len);   // the channel id
            i += len;
        } else break;   // groups/fixed widths don't appear here
    }
    return "";
}
static SearchResult parse_lockup(const json& lv, const std::string& chan_hint) {
    SearchResult sr;
    std::string ctype = lv.value("contentType", "");
    std::string cid = lv.value("contentId", "");

    // Playlist lockup (search results). "RD..." ids are Mixes/radio — not browsable
    // via VL, endless auto-generated — skip those entirely (empty result).
    if (ctype == "LOCKUP_CONTENT_TYPE_PLAYLIST") {
        if (cid.rfind("RD", 0) == 0) return sr;
        sr.kind = SearchResult::Kind::Playlist;
        sr.playlist_id = cid;
        if (const json* meta = find_key(lv, "lockupMetadataViewModel")) {
            if (const json* t = find_key(*meta, "title")) sr.title = t->value("content", "");
            // The owner is the metadata part that links to a channel (browseId "UC…").
            // Other parts ("Updated today", "N videos", "View full playlist") have no
            // such link, so we must not mistake them for the author.
            std::string fallback;
            if (const json* rows = find_key(*meta, "metadataRows"))
                for (auto& row : *rows)
                    for (auto& part : row.value("metadataParts", json::array())) {
                        std::string txt = part.value(json::json_pointer("/text/content"), std::string());
                        if (txt.empty()) continue;
                        std::string bid = part.value(json::json_pointer(
                            "/text/commandRuns/0/onTap/innertubeCommand/browseEndpoint/browseId"),
                            std::string());
                        if (bid.rfind("UC", 0) == 0) {          // linked channel = the owner
                            // The owner name is the FIRST channel-linked part; later parts
                            // (e.g. "Course") link to the same channel but are just labels.
                            if (sr.author.empty()) { sr.author = txt; sr.channel_id = bid; }
                        } else if (fallback.empty() && txt != "Playlist" &&
                                   txt.find("View full") == std::string::npos &&
                                   txt.find("Updated") == std::string::npos &&
                                   txt.find(" ago") == std::string::npos &&
                                   txt.find(" · ") == std::string::npos) {
                            fallback = txt;
                        }
                    }
            if (sr.author.empty()) sr.author = fallback;
        }
        if (const json* badge = find_key(lv, "thumbnailBadgeViewModel"))
            sr.view_count_text = badge->value("text", "");   // "960 videos"
        // Cover thumb: pull the video id out of the collection-thumbnail URL and use
        // the canonical JPEG (the raw URL has sqp= params that negotiate WebP).
        if (const json* img = find_key(lv, "collectionThumbnailViewModel"))
            if (const json* srcs = find_key(*img, "sources"))
                if (srcs->is_array() && !srcs->empty()) {
                    std::string u = (*srcs)[0].value("url", "");
                    auto p = u.find("/vi/");
                    auto e = p == std::string::npos ? p : u.find('/', p + 4);
                    if (e != std::string::npos)
                        sr.thumbnail_url = "https://i.ytimg.com/vi/"
                                         + u.substr(p + 4, e - (p + 4)) + "/mqdefault.jpg";
                }
        return sr;
    }

    sr.kind = SearchResult::Kind::Video;
    sr.video_id = cid;
    sr.channel_id = chan_hint;
    if (const json* meta = find_key(lv, "lockupMetadataViewModel")) {
        if (const json* t = find_key(*meta, "title")) sr.title = t->value("content", "");
        if (const json* rows = find_key(*meta, "metadataRows"))
            for (auto& row : *rows)
                for (auto& part : row.value("metadataParts", json::array())) {
                    std::string txt = part.value(json::json_pointer("/text/content"), std::string());
                    if (txt.empty()) continue;
                    if (txt.find("view") != std::string::npos) sr.view_count_text = txt;
                    else if (txt.find("ago") != std::string::npos) sr.published_text = txt;
                    else if (sr.author.empty()) {
                        sr.author = txt;   // owner name (playlist pages name each row's uploader)
                        // The author part links to the channel — grab its browse id.
                        if (sr.channel_id.empty())
                            sr.channel_id = part.value(json::json_pointer(
                                "/text/commandRuns/0/onTap/innertubeCommand/browseEndpoint/browseId"),
                                std::string());
                    }
                }
    }
    if (const json* badge = find_key(lv, "thumbnailBadgeViewModel"))
        sr.length_text = badge->value("text", "");
    sr.length_seconds = parse_duration(sr.length_text);
    if (sr.length_text == "LIVE" || sr.length_text == "LIVE NOW") sr.is_live = true;
    if (!sr.video_id.empty())
        sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id + "/mqdefault.jpg";
    return sr;
}

// The next-page continuation token, if any.
static std::string continuation_token(const json& node) {
    if (const json* c = find_key(node, "continuationItemRenderer"))
        return c->value(json::json_pointer(
            "/continuationEndpoint/continuationCommand/token"), std::string());
    return "";
}

// The uploader's channelId from a videoRenderer byline (for "favorite channel").
static std::string video_channel_id(const json& vr) {
    for (const char* field : {"ownerText", "longBylineText", "shortBylineText"}) {
        auto it = vr.find(field);
        if (it == vr.end() || !it->contains("runs")) continue;
        for (const auto& run : (*it)["runs"]) {
            std::string bid = run.value(
                json::json_pointer("/navigationEndpoint/browseEndpoint/browseId"), std::string());
            if (!bid.empty()) return bid;
        }
    }
    return "";
}

// A currently-live broadcast: LIVE time-status overlay, or a LIVE_NOW metadata badge.
static bool node_is_live(const json& n) {
    if (const json* ov = find_key(n, "thumbnailOverlayTimeStatusRenderer"))
        if (ov->value("style", std::string()) == "LIVE") return true;
    if (const json* badges = find_key(n, "badges"))
        if (badges->is_array())
            for (const auto& b : *badges)
                if (b.value(json::json_pointer("/metadataBadgeRenderer/style"), std::string())
                        == "BADGE_STYLE_TYPE_LIVE_NOW") return true;
    return false;
}

// Unified: walk a response subtree and build SearchResults from videoRenderer,
// channelRenderer, and lockupViewModel (in document order). chan_hint = the
// channel being viewed (for lockups, which omit the uploader id).
static void collect_results(const json& node, const std::string& chan_hint,
                            std::vector<SearchResult>& out) {
    if (node.is_object()) {
        if (auto it = node.find("videoRenderer"); it != node.end() && it->is_object()) {
            const json& n = *it;
            SearchResult sr;
            sr.video_id = n.value("videoId", "");
            sr.channel_id = video_channel_id(n);
            if (n.contains("title")) sr.title = run_text(n["title"]);
            if (n.contains("ownerText")) sr.author = run_text(n["ownerText"]);
            else if (n.contains("longBylineText")) sr.author = run_text(n["longBylineText"]);
            if (n.contains("lengthText")) sr.length_text = run_text(n["lengthText"]);
            if (n.contains("viewCountText")) sr.view_count_text = run_text(n["viewCountText"]);
            if (n.contains("publishedTimeText")) sr.published_text = run_text(n["publishedTimeText"]);
            sr.length_seconds = parse_duration(sr.length_text);
            sr.is_live = node_is_live(n);
            sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id + "/mqdefault.jpg";
            if (!sr.video_id.empty() && !sr.title.empty()) out.push_back(std::move(sr));
        }
        // compactVideoRenderer: the related/up-next item shape from /next.
        if (auto it = node.find("compactVideoRenderer"); it != node.end() && it->is_object()) {
            const json& n = *it;
            SearchResult sr;
            sr.video_id = n.value("videoId", "");
            sr.channel_id = video_channel_id(n);
            if (n.contains("title")) sr.title = run_text(n["title"]);
            if (n.contains("longBylineText")) sr.author = run_text(n["longBylineText"]);
            else if (n.contains("shortBylineText")) sr.author = run_text(n["shortBylineText"]);
            if (n.contains("lengthText")) sr.length_text = run_text(n["lengthText"]);
            if (n.contains("viewCountText")) sr.view_count_text = run_text(n["viewCountText"]);
            if (n.contains("publishedTimeText")) sr.published_text = run_text(n["publishedTimeText"]);
            sr.length_seconds = parse_duration(sr.length_text);
            sr.is_live = node_is_live(n);
            sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id + "/mqdefault.jpg";
            if (!sr.video_id.empty() && !sr.title.empty()) out.push_back(std::move(sr));
        }
        if (auto it = node.find("channelRenderer"); it != node.end() && it->is_object()) {
            const json& n = *it;
            SearchResult sr;
            sr.kind = SearchResult::Kind::Channel;
            sr.channel_id = n.value("channelId", "");
            if (n.contains("title")) sr.title = run_text(n["title"]);
            std::string a = n.contains("subscriberCountText") ? run_text(n["subscriberCountText"]) : "";
            std::string b = n.contains("videoCountText") ? run_text(n["videoCountText"]) : "";
            bool as = a.find("subscriber") != std::string::npos;
            bool bs = b.find("subscriber") != std::string::npos;
            sr.subs_text = as ? a : bs ? b : (!a.empty() ? a : b);
            sr.author    = as ? b : bs ? a : "";   // @handle
            std::string av;
            if (n.contains("thumbnail") && n["thumbnail"].contains("thumbnails")) {
                const auto& th = n["thumbnail"]["thumbnails"];
                if (th.is_array() && !th.empty()) av = th.back().value("url", "");
            }
            if (av.rfind("//", 0) == 0) av = "https:" + av;
            sr.thumbnail_url = av;
            if (!sr.channel_id.empty() && !sr.title.empty()) out.push_back(std::move(sr));
        }
        if (auto it = node.find("lockupViewModel"); it != node.end() && it->is_object()) {
            SearchResult sr = parse_lockup(*it, chan_hint);
            bool has_id = sr.is_playlist() ? !sr.playlist_id.empty() : !sr.video_id.empty();
            if (has_id && !sr.title.empty()) out.push_back(std::move(sr));
        }
        // Community posts (channel Posts tab): full text + likes + age + optional
        // image / attached video.
        if (auto it = node.find("backstagePostRenderer"); it != node.end() && it->is_object()) {
            const json& n = *it;
            SearchResult sr;
            sr.kind = SearchResult::Kind::Post;
            sr.channel_id = chan_hint;
            if (sr.channel_id.empty())   // fall back to the author link (search/home posts)
                sr.channel_id = n.value(json::json_pointer(
                    "/authorEndpoint/browseEndpoint/browseId"), std::string());
            sr.post_id = n.value("postId", "");
            sr.author = n.value(json::json_pointer("/authorText/runs/0/text"),
                        n.value(json::json_pointer("/authorText/simpleText"), std::string()));
            if (n.contains("contentText"))
                for (auto& run : n["contentText"].value("runs", json::array()))
                    sr.post_text += run.value("text", "");
            // Tile preview: first ~200 bytes, cut at a UTF-8 boundary.
            size_t cut = std::min<size_t>(sr.post_text.size(), 200);
            while (cut > 0 && ((uint8_t)sr.post_text[cut] & 0xC0) == 0x80) --cut;
            sr.title = sr.post_text.substr(0, cut);
            std::string likes = n.value(json::json_pointer("/voteCount/simpleText"),
                                        std::string());
            if (!likes.empty()) sr.view_count_text = likes + " likes";
            sr.published_text = n.value(json::json_pointer("/publishedTimeText/runs/0/text"),
                                        std::string());
            if (n.contains("backstageAttachment")) {
                const json& ba = n["backstageAttachment"];
                if (const json* img = find_key(ba, "backstageImageRenderer")) {
                    if (const json* th = find_key(*img, "thumbnails"))
                        if (th->is_array() && !th->empty()) {
                            std::string u = th->back().value("url", "");
                            if (u.rfind("//", 0) == 0) u = "https:" + u;
                            sr.thumbnail_url = u;
                        }
                }
                if (const json* vr = find_key(ba, "videoRenderer")) {
                    sr.video_id = vr->value("videoId", "");   // playable attachment
                    if (sr.thumbnail_url.empty() && !sr.video_id.empty())
                        sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id
                                         + "/mqdefault.jpg";
                }
            }
            if (!sr.post_text.empty() || !sr.thumbnail_url.empty())
                out.push_back(std::move(sr));
        }
        // Shorts (reels): the API types them explicitly with their own renderer
        // (search Shorts shelves + channel Shorts tab). No duration/date here, and no
        // uploader name — but the reel params protobuf carries the channel id (field 23),
        // so the UI can resolve the name from it (like channel-tile metadata).
        if (auto it = node.find("shortsLockupViewModel"); it != node.end() && it->is_object()) {
            const json& n = *it;
            SearchResult sr;
            sr.is_short = true;
            sr.video_id = n.value(json::json_pointer(
                "/onTap/innertubeCommand/reelWatchEndpoint/videoId"), std::string());
            sr.title = n.value(json::json_pointer(
                "/overlayMetadata/primaryText/content"), std::string());
            sr.view_count_text = n.value(json::json_pointer(
                "/overlayMetadata/secondaryText/content"), std::string());
            sr.channel_id = chan_hint;
            if (sr.channel_id.empty())
                sr.channel_id = reel_params_channel_id(n.value(json::json_pointer(
                    "/onTap/innertubeCommand/reelWatchEndpoint/params"), std::string()));
            sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id + "/mqdefault.jpg";
            if (!sr.video_id.empty() && !sr.title.empty()) out.push_back(std::move(sr));
        }
        for (auto& [k, v] : node.items()) {
            // A post's subtree contains its ATTACHED videoRenderer — already captured
            // on the post itself; re-walking it would add a duplicate video row.
            if (k == "backstagePostRenderer") continue;
            collect_results(v, chan_hint, out);
        }
    } else if (node.is_array()) {
        for (auto& v : node) collect_results(v, chan_hint, out);
    }
}

std::vector<SearchResult> Innertube::search(const std::string& query, int max_results) {
    Feed f = search_feed(query);
    if ((int)f.items.size() > max_results) f.items.resize(max_results);
    return f.items;
}

Innertube::Feed Innertube::search_feed(const std::string& query, const std::string& params) {
    Feed feed; feed.endpoint = "search";
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        json client = json::parse(fp.context_json);
        client["visitorData"] = ensure_visitor_data();
        apply_ctx_locale(client, locale());
        json body = {{"query", query}, {"context", {{"client", client}}}};
        if (!params.empty()) body["params"] = params;   // filters (type/duration/date/sort)
        std::string url = std::string(kInnertubeBase) + "/search";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        HttpClient http;   // LOCAL: search_feed may run on a refresh worker thread
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return feed;
        if (const char* dump = getenv("YTC_DUMP_SEARCH")) {
            std::ofstream f(std::string(dump) + "/search.json");
            if (f) f << r.body;
        }
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return feed;
        if (j.contains("contents")) collect_results(j["contents"], "", feed.items);
        feed.continuation = continuation_token(j);
        feed.ok = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[innertube] search_feed failed: %s\n", e.what());
    }
    return feed;
}

Innertube::Feed Innertube::browse_tab(const std::string& browse_id, const char* params,
                                      const std::string& chan_hint) {
    Feed feed; feed.endpoint = "browse"; feed.channel_id = chan_hint;
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        json client = json::parse(fp.context_json);
        std::string vd = visitor_token();   // pre-warmed by callers; read-only here
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"browseId", browse_id}, {"context", {{"client", client}}}};
        if (params && *params) body["params"] = params;
        std::string url = std::string(kInnertubeBase) + "/browse";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        HttpClient http;   // LOCAL: safe from worker threads (home_feed parallelism)
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return feed;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return feed;
        if (j.contains("contents")) collect_results(j["contents"], chan_hint, feed.items);
        feed.continuation = continuation_token(j);
        feed.ok = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[innertube] browse_tab failed: %s\n", e.what());
    }
    return feed;
}

Innertube::Feed Innertube::channel_feed(const std::string& channel_id) {
    ensure_visitor_data();
    // "EgZ2aWRlb3PyBgQKAjoA" = the channel's Videos tab (no Shorts, no live).
    return browse_tab(channel_id, "EgZ2aWRlb3PyBgQKAjoA", channel_id);
}

Innertube::Feed Innertube::channel_shorts_feed(const std::string& channel_id) {
    ensure_visitor_data();
    // "EgZzaG9ydHPyBgUKA5oBAA==" = the channel's Shorts tab (shortsLockupViewModel
    // items; no dates — home_feed dates them via the channel RSS).
    Feed f = browse_tab(channel_id, "EgZzaG9ydHPyBgUKA5oBAA==", channel_id);
    for (auto& r : f.items) r.is_short = true;   // belt-and-braces
    return f;
}

Innertube::Feed Innertube::channel_playlists_feed(const std::string& channel_id) {
    ensure_visitor_data();
    // "EglwbGF5bGlzdHPyBgQKAkIA" = the channel's Playlists tab (playlist lockups).
    return browse_tab(channel_id, "EglwbGF5bGlzdHPyBgQKAkIA", channel_id);
}

Innertube::Feed Innertube::channel_posts_feed(const std::string& channel_id) {
    ensure_visitor_data();
    // "Egljb21tdW5pdHnyBgQKAkoA" = the channel's Community/Posts tab
    // (backstagePostRenderer items; paginated via the generic continuation).
    return browse_tab(channel_id, "Egljb21tdW5pdHnyBgQKAkoA", channel_id);
}

Innertube::Feed Innertube::channel_all_feed(const std::string& channel_id) {
    ensure_visitor_data();
    // No params = the channel's home/featured page: mixed shelves of videos,
    // Shorts, and playlists (document order).
    Feed f = browse_tab(channel_id, nullptr, channel_id);
    // The All tab shows all *content* types, not Playlists (no good sort for them).
    f.items.erase(std::remove_if(f.items.begin(), f.items.end(),
        [](const SearchResult& r) { return r.is_playlist(); }), f.items.end());
    // Fold in the channel's community Posts, then date-sort the whole thing.
    try {
        Feed pf = channel_posts_feed(channel_id);
        f.items.insert(f.items.end(), std::make_move_iterator(pf.items.begin()),
                       std::make_move_iterator(pf.items.end()));
        std::stable_sort(f.items.begin(), f.items.end(),
            [](const SearchResult& a, const SearchResult& b) {
                return approx_age_secs(a.published_text) < approx_age_secs(b.published_text);
            });
    } catch (...) {}
    return f;
}

Innertube::Feed Innertube::playlist_feed(const std::string& playlist_id) {
    ensure_visitor_data();
    // Playlist contents = /browse of "VL" + playlist id (no params). Items are plain
    // video lockups (title/author/views/age/duration); paginated via continuation.
    // No chan_hint: each row names its own uploader.
    return browse_tab("VL" + playlist_id, nullptr, "");
}

Innertube::Feed Innertube::continue_feed(const Feed& prev) {
    Feed feed; feed.endpoint = prev.endpoint; feed.channel_id = prev.channel_id;
    if (prev.continuation.empty()) return feed;
    try {
        HttpClient http;   // LOCAL handle: continue_feed runs on a worker thread
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        std::string vd = visitor_token();         // cached by the initial search/browse
        if (vd.empty()) return feed;
        json client = json::parse(fp.context_json);
        client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"continuation", prev.continuation}, {"context", {{"client", client}}}};
        std::string url = std::string(kInnertubeBase) + "/" + prev.endpoint;
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return feed;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return feed;
        collect_results(j, prev.channel_id, feed.items);   // in onResponseReceivedCommands
        feed.continuation = continuation_token(j);
        feed.ok = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[innertube] continue_feed failed: %s\n", e.what());
    }
    return feed;
}

// Extract the first <tag>...</tag> inner text starting at/after `from`.
static std::string xml_tag(const std::string& s, const std::string& tag, size_t from = 0) {
    std::string open = "<" + tag, close = "</" + tag + ">";
    auto a = s.find(open, from); if (a == std::string::npos) return "";
    a = s.find('>', a); if (a == std::string::npos) return "";
    auto b = s.find(close, ++a); if (b == std::string::npos) return "";
    return s.substr(a, b - a);
}
// Minimal XML entity decode for RSS titles.
static std::string xml_unescape(std::string s) {
    struct { const char* e; const char* c; } map[] = {
        {"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&#39;","'"},{"&apos;","'"}};
    for (auto& m : map) { std::string e = m.e; size_t p = 0;
        while ((p = s.find(e, p)) != std::string::npos) { s.replace(p, e.size(), m.c); p += 1; } }
    return s;
}

std::vector<SearchResult> Innertube::latest(std::vector<std::string> channel_ids,
                                            int max_results) {
    std::vector<SearchResult> out;
    try {
        // Default: read the user's favorite channels from channels.json.
        if (channel_ids.empty()) {
            std::ifstream f(config_dir_ + "/channels.json");
            if (f) {
                json cfg = json::parse(f, nullptr, false);
                if (!cfg.is_discarded())
                    for (const auto& c : cfg.value("channels", json::array())) {
                        std::string id = c.value("id", "");
                        if (!id.empty()) channel_ids.push_back(id);
                    }
            }
        }
        // published (ISO-8601) sorts lexicographically == chronologically.
        HttpClient http;   // local handle (this may run on a worker thread)
        for (const auto& id : channel_ids) {
            auto r = http.get("https://www.youtube.com/feeds/videos.xml?channel_id=" + id);
            if (!r.ok()) continue;
            const std::string& xml = r.body;
            // Skip the leading channel-level <title>; entries follow.
            size_t pos = xml.find("<entry>");
            while (pos != std::string::npos) {
                size_t end = xml.find("</entry>", pos);
                if (end == std::string::npos) break;
                std::string e = xml.substr(pos, end - pos);
                SearchResult sr;
                sr.video_id = xml_tag(e, "yt:videoId");
                sr.title = xml_unescape(xml_tag(e, "title"));
                sr.author = xml_unescape(xml_tag(e, "name"));
                sr.published_text = xml_tag(e, "published");   // ISO-8601, used as sort key
                if (!sr.video_id.empty() && !sr.title.empty()) {
                    sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id + "/mqdefault.jpg";
                    out.push_back(std::move(sr));
                }
                pos = xml.find("<entry>", end);
            }
        }
        // Newest first, then cap.
        std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
            return a.published_text > b.published_text; });
        if ((int)out.size() > max_results) out.resize(max_results);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[innertube] latest failed: %s\n", ex.what());
        return {};
    }
    return out;
}

// Approximate age in seconds for ordering, from either an ISO-8601 timestamp
// ("2026-08-20T12:34:56+00:00", RSS) or YouTube's relative text ("3 weeks ago",
// "Streamed 2 days ago"). Unknown -> very old (sorts last).
long long approx_age_secs(const std::string& s) {
    if (s.empty()) return (long long)1e12;
    // ISO form?
    if (s.size() >= 19 && s[4] == '-' && s[10] == 'T') {
        std::tm tm{};
        if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900; tm.tm_mon -= 1;
            time_t t = timegm(&tm);
            long long age = (long long)time(nullptr) - (long long)t;
            return age < 0 ? 0 : age;
        }
    }
    // Relative form: first number + unit word.
    long long n = 0; size_t i = 0;
    while (i < s.size() && !isdigit((unsigned char)s[i])) ++i;
    while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
    if (n == 0) return (long long)1e12;
    struct { const char* w; long long mult; } units[] = {
        {"second", 1}, {"minute", 60}, {"hour", 3600}, {"day", 86400},
        {"week", 604800}, {"month", 2629800LL}, {"year", 31557600LL}};
    for (auto& u : units)
        if (s.find(u.w) != std::string::npos) return n * u.mult;
    return (long long)1e12;
}

std::vector<SearchResult> Innertube::home_feed(std::vector<std::string> channel_ids,
                                               int max_results, bool include_history,
                                               HomeCursor* cursor) {
    std::vector<SearchResult> out;
    try {
        if (channel_ids.empty()) {
            channel_ids = favorite_channel_ids();
            if (include_history) {   // union in distinct recent history channels
                std::unordered_map<std::string,bool> have;
                for (auto& id : channel_ids) have[id] = true;
                for (auto& [cid, nm] : history_channels())
                    if (!have.count(cid)) { have[cid] = true; channel_ids.push_back(cid); }
            }
        }
        if (channel_ids.empty()) return out;
        ensure_visitor_data();   // warm ONCE; workers then only read the cached token

        // Per channel: Videos tab (rich, no Shorts) + Shorts tab (typed, no dates)
        // + RSS (exact dates for the newest ~15 uploads incl. Shorts).
        struct ChanData {
            Feed vids, shorts;
            std::unordered_map<std::string, std::string> rss_date;   // id -> ISO
        };
        std::unordered_map<std::string, ChanData> per;
        for (auto& id : channel_ids) per[id];   // pre-create (workers only fill)

        struct Job { std::string id; int kind; };   // 0 vids, 1 shorts, 2 rss
        std::vector<Job> jobs;
        for (auto& id : channel_ids)
            for (int k = 0; k < 3; ++k) jobs.push_back({id, k});
        std::atomic<size_t> next{0};
        std::mutex m;
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < jobs.size()) {
                const Job& job = jobs[i];
                try {
                    if (job.kind == 0) {
                        Feed f = browse_tab(job.id, "EgZ2aWRlb3PyBgQKAjoA", job.id);
                        std::lock_guard<std::mutex> lk(m); per[job.id].vids = std::move(f);
                    } else if (job.kind == 1) {
                        Feed f = browse_tab(job.id, "EgZzaG9ydHPyBgUKA5oBAA==", job.id);
                        for (auto& r : f.items) r.is_short = true;
                        std::lock_guard<std::mutex> lk(m); per[job.id].shorts = std::move(f);
                    } else {
                        HttpClient http;   // local (thread-safe)
                        auto r = http.get("https://www.youtube.com/feeds/videos.xml?channel_id=" + job.id);
                        if (r.ok()) {
                            std::unordered_map<std::string, std::string> dates;
                            size_t pos = r.body.find("<entry>");
                            while (pos != std::string::npos) {
                                size_t end = r.body.find("</entry>", pos);
                                if (end == std::string::npos) break;
                                std::string e = r.body.substr(pos, end - pos);
                                std::string vid = xml_tag(e, "yt:videoId");
                                if (!vid.empty()) dates[vid] = xml_tag(e, "published");
                                pos = r.body.find("<entry>", end);
                            }
                            std::lock_guard<std::mutex> lk(m);
                            per[job.id].rss_date = std::move(dates);
                        }
                    }
                } catch (...) {}   // a failed job just means fewer rows
            }
        };
        int nw = (int)std::min<size_t>(4, jobs.size());
        std::vector<std::thread> threads;
        for (int w = 0; w < nw; ++w) threads.emplace_back(work);
        for (auto& t : threads) t.join();

        // Channel-tab lockups don't name their own uploader — fill from favorites,
        // and from history channels (for the Favorites+History source).
        std::unordered_map<std::string, std::string> cname;
        for (auto& [cid, nm] : favorites()) cname[cid] = nm;
        if (include_history)
            for (auto& [cid, nm] : history_channels())
                if (!nm.empty() && !cname.count(cid)) cname[cid] = nm;

        // Hand back each channel's Videos-tab continuation so the UI can page deeper.
        if (cursor) {
            cursor->channel_ids = channel_ids;
            cursor->cname = cname;
            for (auto& id : channel_ids) {
                cursor->vids_cont[id]   = per[id].vids.continuation;
                cursor->shorts_cont[id] = per[id].shorts.continuation;
            }
        }

        std::vector<SearchResult> all;
        for (auto& id : channel_ids) {
            ChanData& cd = per[id];
            const std::string& nm = cname[id];
            int taken = 0;
            for (auto& v : cd.vids.items) {
                if (v.is_channel()) continue;
                auto it = cd.rss_date.find(v.video_id);
                if (it != cd.rss_date.end()) v.published_text = it->second;  // exact date
                if (v.author.empty()) v.author = nm;
                all.push_back(std::move(v));
                if (++taken >= 30) break;                 // cap per channel (~one browse page)
            }
            taken = 0;
            for (auto& s : cd.shorts.items) {
                auto it = cd.rss_date.find(s.video_id);
                if (it == cd.rss_date.end()) continue;    // undated (older) Shorts: skip
                s.published_text = it->second;
                if (s.author.empty()) s.author = nm;
                all.push_back(std::move(s));
                if (++taken >= 10) break;                 // cap Shorts per channel
            }
        }
        std::stable_sort(all.begin(), all.end(), [](const SearchResult& a, const SearchResult& b) {
            return approx_age_secs(a.published_text) < approx_age_secs(b.published_text);
        });
        out = std::move(all);
        if ((int)out.size() > max_results) out.resize(max_results);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[innertube] home_feed failed: %s\n", ex.what());
    }
    return out;
}

std::vector<SearchResult> Innertube::home_all(std::vector<std::string> channel_ids,
                                              bool include_history, HomeCursor* cursor) {
    // Videos + Shorts (with the Videos/Shorts continuations in cursor) ...
    std::vector<SearchResult> out = home_feed(channel_ids, 120, include_history, cursor);
    // ... then community Posts folded in and the whole thing re-sorted by age. Posts
    // paginate only on the dedicated Posts tab, so we don't keep their cursor here.
    try {
        std::vector<SearchResult> posts = home_posts(channel_ids, cursor);
        out.insert(out.end(), std::make_move_iterator(posts.begin()),
                   std::make_move_iterator(posts.end()));
        std::stable_sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
            return approx_age_secs(a.published_text) < approx_age_secs(b.published_text);
        });
    } catch (...) {}
    return out;
}

std::vector<SearchResult> Innertube::home_feed_more(HomeCursor& cursor, int per_channel) {
    std::vector<SearchResult> out;
    if (!cursor.has_more()) return out;
    try {
        ensure_visitor_data();   // warm once; workers only read the cached token

        // Continue every channel that still has a token, in parallel. Each result page
        // is the NEXT (older) slice of that channel's uploads. kind: 0 Videos, 1 Shorts.
        struct Job { std::string id; int kind; };
        struct Res { std::string id; int kind; Feed feed; };
        std::vector<Job> jobs;
        for (auto& id : cursor.channel_ids) {
            if (!cursor.vids_cont[id].empty())   jobs.push_back({id, 0});
            if (!cursor.shorts_cont[id].empty()) jobs.push_back({id, 1});
        }
        std::vector<Res> results(jobs.size());
        std::atomic<size_t> next{0};
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < jobs.size()) {
                const Job& job = jobs[i];
                Feed cur; cur.endpoint = "browse"; cur.channel_id = job.id;
                cur.continuation = job.kind == 0 ? cursor.vids_cont[job.id]
                                                 : cursor.shorts_cont[job.id];
                Feed f;
                try { f = continue_feed(cur); } catch (...) {}
                results[i] = {job.id, job.kind, std::move(f)};
            }
        };
        int nw = (int)std::min<size_t>(4, jobs.size());
        std::vector<std::thread> threads;
        for (int w = 0; w < nw; ++w) threads.emplace_back(work);
        for (auto& t : threads) t.join();

        // Merge the new pages; advance (or exhaust) each channel's per-tab token.
        for (auto& r : results) {
            if (r.kind == 0) cursor.vids_cont[r.id]   = r.feed.continuation;
            else             cursor.shorts_cont[r.id] = r.feed.continuation;  // "" -> done
            const std::string& nm = cursor.cname[r.id];
            int taken = 0;
            for (auto& v : r.feed.items) {
                if (v.is_channel()) continue;
                if (r.kind == 1) v.is_short = true;   // Shorts-tab items type as Shorts
                if (v.author.empty()) v.author = nm;
                out.push_back(std::move(v));
                if (++taken >= per_channel) break;
            }
        }
        // Sort the batch newest-first among itself (browse lockups carry "N ago" text).
        std::stable_sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
            return approx_age_secs(a.published_text) < approx_age_secs(b.published_text);
        });
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[innertube] home_feed_more failed: %s\n", ex.what());
    }
    return out;
}

std::vector<SearchResult> Innertube::home_playlists(std::vector<std::string> channel_ids,
                                                    HomeCursor* cursor) {
    std::vector<SearchResult> out;
    try {
        if (channel_ids.empty()) channel_ids = favorite_channel_ids();
        if (channel_ids.empty()) return out;
        ensure_visitor_data();   // warm once; workers only read the cached token
        std::vector<std::vector<SearchResult>> per(channel_ids.size());
        std::vector<std::string> conts(channel_ids.size());   // per-channel next-page token
        std::atomic<size_t> next{0};
        std::unordered_map<std::string, std::string> cname;
        for (auto& [cid, nm] : favorites()) cname[cid] = nm;
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < channel_ids.size()) {
                try {
                    Feed f = channel_playlists_feed(channel_ids[i]);
                    conts[i] = f.continuation;                          // keep for paging
                    if ((int)f.items.size() > 12) f.items.resize(12);   // cap per channel
                    // Playlists on a channel's own tab don't repeat the owner name.
                    auto it = cname.find(channel_ids[i]);
                    if (it != cname.end())
                        for (auto& r : f.items)
                            if (r.author.empty()) r.author = it->second;
                    per[i] = std::move(f.items);
                } catch (...) {}
            }
        };
        int nw = (int)std::min<size_t>(4, channel_ids.size());
        std::vector<std::thread> threads;
        for (int w = 0; w < nw; ++w) threads.emplace_back(work);
        for (auto& t : threads) t.join();
        for (auto& v : per) out.insert(out.end(),
            std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
        if (cursor) {
            cursor->channel_ids = channel_ids;
            cursor->cname = cname;
            for (size_t i = 0; i < channel_ids.size(); ++i)
                cursor->pl_cont[channel_ids[i]] = conts[i];
        }
    } catch (...) {}
    return out;
}

std::vector<SearchResult> Innertube::home_playlists_more(HomeCursor& cursor, int per_channel) {
    std::vector<SearchResult> out;
    if (!cursor.has_more()) return out;
    try {
        ensure_visitor_data();
        std::vector<std::string> ids;
        for (auto& id : cursor.channel_ids)
            if (!cursor.pl_cont[id].empty()) ids.push_back(id);
        struct Res { std::string id; Feed feed; };
        std::vector<Res> results(ids.size());
        std::atomic<size_t> next{0};
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < ids.size()) {
                const std::string& id = ids[i];
                Feed cur; cur.endpoint = "browse"; cur.channel_id = id;
                cur.continuation = cursor.pl_cont[id];
                Feed f;
                try { f = continue_feed(cur); } catch (...) {}
                results[i] = {id, std::move(f)};
            }
        };
        int nw = (int)std::min<size_t>(4, ids.size());
        std::vector<std::thread> threads;
        for (int w = 0; w < nw; ++w) threads.emplace_back(work);
        for (auto& t : threads) t.join();
        // Grouped per channel (playlists have no useful cross-channel date order).
        for (auto& r : results) {
            cursor.pl_cont[r.id] = r.feed.continuation;   // "" -> that channel is done
            const std::string& nm = cursor.cname[r.id];
            int taken = 0;
            for (auto& v : r.feed.items) {
                if (!v.is_playlist()) continue;
                if (v.author.empty()) v.author = nm;
                out.push_back(std::move(v));
                if (++taken >= per_channel) break;
            }
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[innertube] home_playlists_more failed: %s\n", ex.what());
    }
    return out;
}

std::vector<SearchResult> Innertube::home_posts(std::vector<std::string> channel_ids,
                                                HomeCursor* cursor) {
    std::vector<SearchResult> out;
    try {
        if (channel_ids.empty()) channel_ids = favorite_channel_ids();
        if (channel_ids.empty()) return out;
        ensure_visitor_data();   // warm once; workers only read the cached token
        std::vector<std::vector<SearchResult>> per(channel_ids.size());
        std::vector<std::string> conts(channel_ids.size());
        std::atomic<size_t> next{0};
        std::unordered_map<std::string, std::string> cname;
        for (auto& [cid, nm] : favorites()) cname[cid] = nm;
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < channel_ids.size()) {
                try {
                    Feed f = channel_posts_feed(channel_ids[i]);
                    conts[i] = f.continuation;
                    if ((int)f.items.size() > 12) f.items.resize(12);   // cap per channel
                    auto it = cname.find(channel_ids[i]);
                    if (it != cname.end())
                        for (auto& r : f.items) if (r.author.empty()) r.author = it->second;
                    per[i] = std::move(f.items);
                } catch (...) {}
            }
        };
        int nw = (int)std::min<size_t>(4, channel_ids.size());
        std::vector<std::thread> threads;
        for (int w = 0; w < nw; ++w) threads.emplace_back(work);
        for (auto& t : threads) t.join();
        for (auto& v : per) out.insert(out.end(),
            std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
        // Interleave all channels' posts by age (newest first) instead of grouping by
        // channel. Relative "N days ago" text is parsed to an approximate age.
        std::stable_sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
            return approx_age_secs(a.published_text) < approx_age_secs(b.published_text);
        });
        if (cursor) {
            cursor->channel_ids = channel_ids;
            cursor->cname = cname;
            for (size_t i = 0; i < channel_ids.size(); ++i)
                cursor->posts_cont[channel_ids[i]] = conts[i];
        }
    } catch (...) {}
    return out;
}

std::vector<SearchResult> Innertube::home_posts_more(HomeCursor& cursor, int per_channel) {
    std::vector<SearchResult> out;
    if (!cursor.has_more()) return out;
    try {
        ensure_visitor_data();
        std::vector<std::string> ids;
        for (auto& id : cursor.channel_ids)
            if (!cursor.posts_cont[id].empty()) ids.push_back(id);
        struct Res { std::string id; Feed feed; };
        std::vector<Res> results(ids.size());
        std::atomic<size_t> next{0};
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < ids.size()) {
                const std::string& id = ids[i];
                Feed cur; cur.endpoint = "browse"; cur.channel_id = id;
                cur.continuation = cursor.posts_cont[id];
                Feed f;
                try { f = continue_feed(cur); } catch (...) {}
                results[i] = {id, std::move(f)};
            }
        };
        int nw = (int)std::min<size_t>(4, ids.size());
        std::vector<std::thread> threads;
        for (int w = 0; w < nw; ++w) threads.emplace_back(work);
        for (auto& t : threads) t.join();
        for (auto& r : results) {
            cursor.posts_cont[r.id] = r.feed.continuation;   // "" -> that channel is done
            const std::string& nm = cursor.cname[r.id];
            int taken = 0;
            for (auto& v : r.feed.items) {
                if (!v.is_post()) continue;
                if (v.author.empty()) v.author = nm;
                v.channel_id = r.id;                 // continuation loses chan_hint attribution
                out.push_back(std::move(v));
                if (++taken >= per_channel) break;
            }
        }
        // Keep this page's newly fetched posts interleaved by age too.
        std::stable_sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
            return approx_age_secs(a.published_text) < approx_age_secs(b.published_text);
        });
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[innertube] home_posts_more failed: %s\n", ex.what());
    }
    return out;
}

// First node under `node` with the given key (recursive).
static const json* find_key(const json& node, const char* key) {
    if (node.is_object()) {
        auto it = node.find(key);
        if (it != node.end()) return &*it;
        for (auto& [k, v] : node.items()) { if (auto* r = find_key(v, key)) return r; }
    } else if (node.is_array()) {
        for (auto& v : node) { if (auto* r = find_key(v, key)) return r; }
    }
    return nullptr;
}

ChannelInfo Innertube::channel_info(const std::string& channel_id) {
    ChannelInfo info; info.channel_id = channel_id;
    try {
        HttpClient http;   // LOCAL handle: safe to call from a worker thread
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        std::string vd = visitor_token();   // reuse the cached session token if present
        if (vd.empty()) {
            json vb = {{"context", {{"client", {{"clientName", fp.innertube_client_name},
                        {"clientVersion", fp.innertube_client_version}}}}}};
            std::vector<std::string> vh = {"Content-Type: application/json",
                "User-Agent: " + fp.user_agent,
                "X-YouTube-Client-Name: " + std::to_string(fp.client_name_id),
                "X-YouTube-Client-Version: " + fp.innertube_client_version};
            std::string vurl = std::string(kInnertubeBase) + "/visitor_id";
            if (!api_key_.empty()) vurl += "?key=" + api_key_;
            auto vr = http.post(vurl, vb.dump(), vh);
            if (vr.ok()) { auto vj = json::parse(vr.body, nullptr, false);
                if (!vj.is_discarded()) vd = vj.at("responseContext").value("visitorData", ""); }
        }
        if (getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[chinfo] fp=%s vd.len=%zu id=%s ctx.len=%zu\n",
                         fp.name.c_str(), vd.size(), channel_id.c_str(), fp.context_json.size());
        json client = json::parse(fp.context_json);
        client["visitorData"] = vd; apply_ctx_locale(client, locale());
        json body = {{"browseId", channel_id}, {"context", {{"client", client}}}};
        std::vector<std::string> headers = {"Content-Type: application/json",
            "User-Agent: " + fp.user_agent,
            "X-YouTube-Client-Name: " + std::to_string(fp.client_name_id),
            "X-YouTube-Client-Version: " + fp.innertube_client_version,
            "X-Goog-Visitor-Id: " + vd};
        std::string url = std::string(kInnertubeBase) + "/browse";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), headers);
        if (!r.ok()) return info;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return info;
        // Debug: dump the raw /browse response for offline analysis.
        if (const char* dump = getenv("YTC_DUMP_BROWSE")) {
            std::ofstream f(std::string(dump) + "/browse_" + channel_id + ".json");
            if (f) f << r.body;
        }

        if (auto* meta = find_key(j, "channelMetadataRenderer")) {
            info.name = meta->value("title", "");
            info.description = meta->value("description", "");
            // avatar.thumbnails[] is ordered small->large; take the largest.
            if (auto* av = find_key(*meta, "avatar"))
                if (auto* th = find_key(*av, "thumbnails"))
                    if (th->is_array() && !th->empty())
                        info.avatar_url = th->back().value("url", "");
        }
        // Fallback: some layouts only carry the avatar in the page header image.
        if (info.avatar_url.empty()) {
            const json* ph2 = find_key(j, "pageHeaderViewModel");
            if (auto* img = ph2 ? find_key(*ph2, "image") : nullptr)
                if (auto* th = find_key(*img, "sources"))
                    if (th->is_array() && !th->empty())
                        info.avatar_url = th->back().value("url", "");
        }
        // Scope to the page HEADER's metadata (else find_key grabs a video's).
        const json* ph = find_key(j, "pageHeaderViewModel");
        const json* cmv = ph ? find_key(*ph, "contentMetadataViewModel") : nullptr;
        if (cmv) {
            for (auto& row : cmv->value("metadataRows", json::array()))
                for (auto& part : row.value("metadataParts", json::array())) {
                    std::string t = part.value(json::json_pointer("/text/content"), std::string());
                    if (t.empty()) continue;
                    if (t.find("subscriber") != std::string::npos) info.subs_text = t;
                    else if (t.find("video") != std::string::npos) info.video_count_text = t;
                    else if (t[0] == '@') info.handle = t;
                }
        }
        if (info.name.empty()) info.name = channel_id;
        info.ok = !info.subs_text.empty() || !info.video_count_text.empty() || !info.description.empty();
        if (getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[chinfo] parsed name=%s subs=[%s] videos=[%s]\n",
                info.name.c_str(), info.subs_text.c_str(), info.video_count_text.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[innertube] channel_info failed: %s\n", e.what());
    }
    return info;
}

std::vector<std::string> Innertube::favorite_channel_ids() {
    std::vector<std::string> ids;
    std::ifstream f(config_dir_ + "/channels.json");
    if (!f) return ids;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return ids;
    for (const auto& c : cfg.value("channels", json::array())) {
        std::string id = c.value("id", "");
        if (!id.empty()) ids.push_back(id);
    }
    return ids;
}

bool Innertube::add_favorite(const std::string& channel_id, const std::string& name) {
    if (channel_id.empty()) return false;
    std::string path = config_dir_ + "/channels.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    if (!cfg.contains("channels") || !cfg["channels"].is_array()) cfg["channels"] = json::array();
    for (const auto& c : cfg["channels"])
        if (c.value("id", "") == channel_id) return false;      // already a favorite
    cfg["channels"].push_back({{"name", name}, {"id", channel_id}});
    std::ofstream o(path);
    if (!o) return false;
    o << cfg.dump(2) << "\n";
    return true;
}

bool Innertube::remove_favorite(const std::string& channel_id) {
    std::string path = config_dir_ + "/channels.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.contains("channels")) return false;
    json keep = json::array();
    bool removed = false;
    for (const auto& c : cfg["channels"]) {
        if (c.value("id", "") == channel_id) removed = true;
        else keep.push_back(c);
    }
    if (!removed) return false;
    cfg["channels"] = keep;
    std::ofstream o(path);
    if (!o) return false;
    o << cfg.dump(2) << "\n";
    return true;
}

std::vector<std::string> Innertube::watch_later_ids() {
    std::vector<std::string> ids;
    std::ifstream f(config_dir_ + "/watch_later.json");
    if (!f) return ids;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return ids;
    for (const auto& v : cfg.value("videos", json::array())) {
        std::string id = v.value("id", "");
        if (!id.empty()) ids.push_back(id);
    }
    return ids;
}
bool Innertube::add_watch_later(const std::string& id, const std::string& title,
                                bool is_playlist, const std::string& thumb,
                                const std::string& author, const std::string& count) {
    if (id.empty()) return false;
    std::string path = config_dir_ + "/watch_later.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    if (!cfg.contains("videos") || !cfg["videos"].is_array()) cfg["videos"] = json::array();
    for (const auto& v : cfg["videos"]) if (v.value("id", "") == id) return false;
    // newest additions first
    json entry = {{"id", id}, {"title", title}};
    if (is_playlist) entry["playlist"] = true;
    if (!thumb.empty())  entry["thumb"]  = thumb;
    if (!author.empty()) entry["author"] = author;
    if (!count.empty())  entry["count"]  = count;
    cfg["videos"].insert(cfg["videos"].begin(), entry);
    std::ofstream o(path); if (!o) return false;
    o << cfg.dump(2) << "\n";
    return true;
}
bool Innertube::remove_watch_later(const std::string& video_id) {
    std::string path = config_dir_ + "/watch_later.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.contains("videos")) return false;
    json keep = json::array(); bool removed = false;
    for (const auto& v : cfg["videos"]) {
        if (v.value("id", "") == video_id) removed = true; else keep.push_back(v);
    }
    if (!removed) return false;
    cfg["videos"] = keep;
    std::ofstream o(path); if (!o) return false;
    o << cfg.dump(2) << "\n";
    return true;
}

// --- (id, name/title) accessors over our own local stores ------------------
// These are NOT Innertube API calls; anonymous playback has no server-side
// favorites/watch-later/history. Everything here reads the JSON files this app
// writes itself, so the menu views can render tiles for each list.

// ---- Custom feed (saved searches) ----
// config/feeds.json: { "feeds": [ { "name": "Custom", "sources": [
//   { "query": "...", "type": 0, "duration": 0, "upload_date": 0, "sort": 0 } ] } ] }
// The named-feeds array leaves room to grow; only "Custom" is used today.
static json load_feeds_json(const std::string& config_dir) {
    std::ifstream f(config_dir + "/feeds.json");
    if (f) { json j = json::parse(f, nullptr, false); if (!j.is_discarded() && j.is_object()) return j; }
    return json{{"feeds", json::array({ json{{"name","Custom"},{"sources",json::array()}} })}};
}
static json* custom_sources_of(json& cfg) {
    if (!cfg.contains("feeds") || !cfg["feeds"].is_array()) return nullptr;
    for (auto& fd : cfg["feeds"])
        if (fd.value("name", "") == "Custom" && fd.contains("sources") && fd["sources"].is_array())
            return &fd["sources"];
    return nullptr;
}

std::vector<FeedSource> Innertube::custom_feed_sources() {
    std::vector<FeedSource> out;
    json cfg = load_feeds_json(config_dir_);
    json* src = custom_sources_of(cfg);
    if (!src) return out;
    for (const auto& s : *src) {
        FeedSource fs;
        fs.query = s.value("query", "");
        fs.type = s.value("type", 0); fs.duration = s.value("duration", 0);
        fs.upload_date = s.value("upload_date", 0); fs.sort = s.value("sort", 0);
        if (!fs.query.empty()) out.push_back(std::move(fs));
    }
    return out;
}

bool Innertube::add_custom_feed_source(const FeedSource& s) {
    for (const auto& e : custom_feed_sources()) if (e == s) return false;
    json cfg = load_feeds_json(config_dir_);
    json* src = custom_sources_of(cfg);
    if (!src) return false;
    src->push_back(json{{"query", s.query}, {"type", s.type}, {"duration", s.duration},
                        {"upload_date", s.upload_date}, {"sort", s.sort}});
    std::ofstream o(config_dir_ + "/feeds.json"); if (!o) return false;
    o << cfg.dump(2) << "\n";
    return true;
}

void Innertube::remove_custom_feed_source(size_t index) {
    json cfg = load_feeds_json(config_dir_);
    json* src = custom_sources_of(cfg);
    if (!src || index >= src->size()) return;
    src->erase(src->begin() + index);
    std::ofstream o(config_dir_ + "/feeds.json"); if (!o) return;
    o << cfg.dump(2) << "\n";
}

std::vector<SearchResult> Innertube::custom_feed(int per_source) {
    // UI filter index -> protobuf value (same tables as the search screen).
    static const int kTypePb[] = {0, 1, 2, 3};
    static const int kDurPb[]  = {0, 4, 5, 2};
    static const int kDatePb[] = {0, 2, 3, 4, 5};
    static const int kSortPb[] = {0, 0, 3};
    std::vector<std::vector<SearchResult>> per;
    for (const auto& s : custom_feed_sources()) {
        std::string params = build_search_params(kTypePb[s.type & 3], kDurPb[s.duration & 3],
                                                 kDatePb[s.upload_date % 5], kSortPb[s.sort % 3]);
        Feed f = search_feed(s.query, params);
        if ((int)f.items.size() > per_source) f.items.resize(per_source);
        if (!f.items.empty()) per.push_back(std::move(f.items));
    }
    // Round-robin interleave so every saved search is represented up top; dedupe.
    std::vector<SearchResult> out;
    std::unordered_map<std::string, bool> seen;
    for (size_t i = 0; !per.empty(); ++i) {
        bool any = false;
        for (auto& list : per) {
            if (i >= list.size()) continue;
            any = true;
            SearchResult& r = list[i];
            std::string key = !r.video_id.empty() ? r.video_id
                            : !r.playlist_id.empty() ? r.playlist_id : r.channel_id;
            if (key.empty() || seen.count(key)) continue;
            seen[key] = true;
            out.push_back(std::move(r));
        }
        if (!any) break;
    }
    return out;
}

std::vector<std::pair<std::string,std::string>> Innertube::favorites() {
    std::vector<std::pair<std::string,std::string>> out;
    std::ifstream f(config_dir_ + "/channels.json");
    if (!f) return out;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return out;
    for (const auto& c : cfg.value("channels", json::array())) {
        std::string id = c.value("id", "");
        if (!id.empty()) out.emplace_back(id, c.value("name", ""));
    }
    return out;
}

std::vector<SearchResult> Innertube::watch_later() {
    std::vector<SearchResult> out;
    std::ifstream f(config_dir_ + "/watch_later.json");
    if (!f) return out;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return out;
    for (const auto& v : cfg.value("videos", json::array())) {
        std::string id = v.value("id", "");
        if (id.empty()) continue;
        SearchResult r;
        r.title = v.value("title", "");
        r.author = v.value("author", "");
        r.thumbnail_url = v.value("thumb", "");
        if (v.value("playlist", false)) {
            r.kind = SearchResult::Kind::Playlist;
            r.playlist_id = id;
            r.view_count_text = v.value("count", "");   // "960 videos"
        } else {
            r.video_id = id;
            if (r.thumbnail_url.empty())
                r.thumbnail_url = "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::tuple<std::string,std::string,std::string>> Innertube::history() {
    std::vector<std::tuple<std::string,std::string,std::string>> out;
    std::ifstream f(config_dir_ + "/history.json");
    if (!f) return out;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return out;
    for (const auto& v : cfg.value("videos", json::array())) {
        std::string id = v.value("id", "");
        if (!id.empty()) out.emplace_back(id, v.value("title", ""), v.value("channel", ""));
    }
    return out;
}

void Innertube::add_history(const std::string& video_id, const std::string& title,
                            const std::string& channel_id, const std::string& channel_name) {
    if (video_id.empty()) return;
    const int kMaxHistory = 200;
    std::string path = config_dir_ + "/history.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    if (!cfg.contains("videos") || !cfg["videos"].is_array()) cfg["videos"] = json::array();
    // Move-to-front dedupe: drop any existing entry for this id, then prepend.
    json keep = json::array();
    for (const auto& v : cfg["videos"])
        if (v.value("id", "") != video_id) keep.push_back(v);
    json entry{{"id", video_id}, {"title", title}};
    if (!channel_id.empty())   entry["channel_id"] = channel_id;
    if (!channel_name.empty()) entry["channel"] = channel_name;
    keep.insert(keep.begin(), entry);
    while ((int)keep.size() > kMaxHistory) keep.erase(keep.end() - 1);
    cfg["videos"] = keep;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
}

std::vector<std::pair<std::string,std::string>> Innertube::history_channels(int max_channels) {
    std::vector<std::pair<std::string,std::string>> out;
    std::ifstream f(config_dir_ + "/history.json");
    if (!f) return out;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return out;
    std::unordered_map<std::string,bool> seen;
    for (const auto& v : cfg.value("videos", json::array())) {   // most-recent first
        std::string cid = v.value("channel_id", "");
        if (cid.empty() || seen.count(cid)) continue;
        seen[cid] = true;
        out.emplace_back(cid, v.value("channel", ""));
        if ((int)out.size() >= max_channels) break;
    }
    return out;
}

void Innertube::clear_history() {
    std::remove((config_dir_ + "/history.json").c_str());
}

double Innertube::resume_pos(const std::string& video_id) {
    std::ifstream f(config_dir_ + "/resume.json");
    if (!f) return 0;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return 0;
    json vids = cfg.value("videos", json::array());
    for (auto& v : vids)
        if (v.value("id", "") == video_id) return v.value("pos", 0.0);
    return 0;
}
void Innertube::set_resume_pos(const std::string& video_id, double seconds) {
    if (video_id.empty()) return;
    const int kMax = 300;
    std::string path = config_dir_ + "/resume.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    if (!cfg.contains("videos") || !cfg["videos"].is_array()) cfg["videos"] = json::array();
    json keep = json::array();
    for (const auto& v : cfg["videos"])
        if (v.value("id", "") != video_id) keep.push_back(v);
    keep.insert(keep.begin(), json{{"id", video_id}, {"pos", seconds}});
    while ((int)keep.size() > kMax) keep.erase(keep.end() - 1);
    cfg["videos"] = keep;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
}
void Innertube::clear_resume_pos(const std::string& video_id) {
    std::string path = config_dir_ + "/resume.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.contains("videos")) return;
    json keep = json::array(); bool removed = false;
    for (const auto& v : cfg["videos"]) {
        if (v.value("id", "") == video_id) removed = true; else keep.push_back(v);
    }
    if (!removed) return;
    cfg["videos"] = keep;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
}

int Innertube::check_video_restricted(const std::string& video_id) {
    try {
        // Use the IOS fingerprint: it both plays kids videos AND carries the
        // made-for-kids marker (support answer 9632097) in playabilityStatus.
        const ClientFingerprint* fp = nullptr;
        for (const auto& c : clients_) if (c.name == "IOS") fp = &c;
        if (!fp && !clients_.empty()) fp = &clients_.back();
        if (!fp) return -1;

        HttpClient http;   // LOCAL: safe from a worker thread
        std::string vd = visitor_token();   // reuse cached session token if present
        json client = json::parse(fp->context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"videoId", video_id},
                     {"context", {{"client", client}}},
                     {"contentCheckOk", true}, {"racyCheckOk", true}};
        std::vector<std::string> headers = {"Content-Type: application/json",
            "User-Agent: " + fp->user_agent,
            "X-YouTube-Client-Name: " + std::to_string(fp->client_name_id),
            "X-YouTube-Client-Version: " + fp->innertube_client_version};
        if (!vd.empty()) headers.push_back("X-Goog-Visitor-Id: " + vd);
        std::string url = std::string(kInnertubeBase) + "/player";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), headers);
        if (!r.ok()) return -1;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return -1;
        json ps = j.value("playabilityStatus", json::object());
        std::string psdump = ps.dump();
        if (psdump.find("9632097") != std::string::npos) return 1;   // made for kids
        // Only trust a clean verdict from a definitive answer (bot-wall / errors
        // lack the marker but prove nothing).
        return ps.value("status", "") == "OK" ? 0 : -1;
    } catch (...) { return -1; }
}

std::string Innertube::video_description(const std::string& video_id) {
    try {
        const ClientFingerprint* fp = nullptr;   // IOS: plays the widest set of videos
        for (const auto& c : clients_) if (c.name == "IOS") fp = &c;
        if (!fp && !clients_.empty()) fp = &clients_.back();
        if (!fp) return "";
        HttpClient http;   // LOCAL: safe from a worker thread
        std::string vd = visitor_token();
        json client = json::parse(fp->context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"videoId", video_id}, {"context", {{"client", client}}},
                     {"contentCheckOk", true}, {"racyCheckOk", true}};
        std::vector<std::string> headers = {"Content-Type: application/json",
            "User-Agent: " + fp->user_agent,
            "X-YouTube-Client-Name: " + std::to_string(fp->client_name_id),
            "X-YouTube-Client-Version: " + fp->innertube_client_version};
        if (!vd.empty()) headers.push_back("X-Goog-Visitor-Id: " + vd);
        std::string url = std::string(kInnertubeBase) + "/player";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), headers);
        if (!r.ok()) return "";
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return "";
        return j.value("videoDetails", json::object()).value("shortDescription", "");
    } catch (...) { return ""; }
}

std::vector<CaptionTrack> Innertube::caption_tracks(const std::string& video_id) {
    std::vector<CaptionTrack> out;
    try {
        const ClientFingerprint* fp = nullptr;   // IOS: widest playability + has captions
        for (const auto& c : clients_) if (c.name == "IOS") fp = &c;
        if (!fp && !clients_.empty()) fp = &clients_.back();
        if (!fp) return out;
        HttpClient http;   // LOCAL: safe from a worker thread
        std::string vd = visitor_token();
        json client = json::parse(fp->context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"videoId", video_id}, {"context", {{"client", client}}},
                     {"contentCheckOk", true}, {"racyCheckOk", true}};
        std::vector<std::string> headers = {"Content-Type: application/json",
            "User-Agent: " + fp->user_agent,
            "X-YouTube-Client-Name: " + std::to_string(fp->client_name_id),
            "X-YouTube-Client-Version: " + fp->innertube_client_version};
        if (!vd.empty()) headers.push_back("X-Goog-Visitor-Id: " + vd);
        std::string url = std::string(kInnertubeBase) + "/player";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), headers);
        if (!r.ok()) return out;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return out;
        const json* list = find_key(j, "captionTracks");
        if (!list || !list->is_array()) return out;
        for (const auto& t : *list) {
            CaptionTrack ct;
            ct.base_url = t.value("baseUrl", "");
            ct.language_code = t.value("languageCode", "");
            ct.auto_generated = (t.value("kind", "") == "asr");
            ct.translatable = t.value("isTranslatable", false);
            if (t.contains("name")) {
                const json& n = t["name"];
                if (n.contains("simpleText")) ct.name = n["simpleText"].get<std::string>();
                else if (n.contains("runs") && n["runs"].is_array() && !n["runs"].empty())
                    ct.name = n["runs"][0].value("text", "");
            }
            if (ct.name.empty()) ct.name = ct.language_code;
            if (!ct.base_url.empty()) out.push_back(std::move(ct));
        }
    } catch (...) { out.clear(); }
    return out;
}

std::vector<SearchResult> Innertube::related_videos(const std::string& video_id) {
    std::vector<SearchResult> out;
    try {
        // WEB (search_client) returns the richest watch-next secondaryResults.
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        HttpClient http;   // LOCAL: thread-safe
        std::string vd = visitor_token();
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"videoId", video_id}, {"context", {{"client", client}}}};
        std::string url = std::string(kInnertubeBase) + "/next";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return out;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return out;
        // secondaryResults holds the related list; parse compactVideoRenderer/lockups.
        if (const json* sec = find_key(j, "secondaryResults"))
            collect_results(*sec, "", out);
        // Keep only playable videos (drop channels/playlists/posts) and self.
        std::vector<SearchResult> vids;
        for (auto& s : out)
            if (!s.video_id.empty() && s.kind == SearchResult::Kind::Video &&
                s.video_id != video_id)
                vids.push_back(std::move(s));
        return vids;
    } catch (...) { return {}; }
}

// ---- Comments ----------------------------------------------------------------
// YouTube serves comment CONTENT as "entity payloads" in frameworkUpdates.mutations,
// keyed by an entity key; the continuationItems only reference those keys via a
// commentViewModel. We build the key->content maps first, then walk the thread list.

// Minimal protobuf + base64url, used to build the FEpost_detail browse params
// (community-post detail page) from a channelId + postId.
static void pb_varint(std::string& out, uint64_t v) {
    do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; out += (char)b; } while (v);
}
static void pb_string(std::string& out, int field, const std::string& val) {
    pb_varint(out, (uint64_t)field << 3 | 2);
    pb_varint(out, val.size());
    out += val;
}
static void pb_int(std::string& out, int field, uint64_t v) {
    pb_varint(out, (uint64_t)field << 3 | 0);   // wire type 0 (varint)
    pb_varint(out, v);
}
static std::string base64_std(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t n = (uint8_t)in[i]<<16 | (uint8_t)in[i+1]<<8 | (uint8_t)in[i+2];
        out += T[n>>18&63]; out += T[n>>12&63]; out += T[n>>6&63]; out += T[n&63]; i += 3;
    }
    if (size_t rem = in.size() - i) {
        uint32_t n = (uint8_t)in[i]<<16 | (rem>1 ? (uint8_t)in[i+1]<<8 : 0);
        out += T[n>>18&63]; out += T[n>>12&63];
        out += rem>1 ? T[n>>6&63] : '='; out += '=';
    }
    return out;
}
// SearchParams = { sort_by=1 varint, filters=2 { upload_date=1, type=2, duration=3 } }.
std::string build_search_params(int type, int duration, int upload_date, int sort_by) {
    std::string inner;
    if (upload_date) pb_int(inner, 1, upload_date);
    if (type)        pb_int(inner, 2, type);
    if (duration)    pb_int(inner, 3, duration);
    std::string outer;
    if (sort_by)         pb_int(outer, 1, sort_by);
    if (!inner.empty())  pb_string(outer, 2, inner);
    return outer.empty() ? std::string() : base64_std(outer);
}
static std::string base64url_nopad(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out; size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t n = (uint8_t)in[i]<<16 | (uint8_t)in[i+1]<<8 | (uint8_t)in[i+2];
        out += T[n>>18&63]; out += T[n>>12&63]; out += T[n>>6&63]; out += T[n&63]; i += 3;
    }
    if (size_t rem = in.size() - i) {
        uint32_t n = (uint8_t)in[i]<<16 | (rem>1 ? (uint8_t)in[i+1]<<8 : 0);
        out += T[n>>18&63]; out += T[n>>12&63];
        if (rem > 1) out += T[n>>6&63];
    }
    return out;
}
// FEpost_detail params = base64url( field56{ field2:channelId, field3:postId, field11:channelId } ).
static std::string post_detail_params(const std::string& post_id, const std::string& channel_id) {
    std::string inner;
    pb_string(inner, 2, channel_id);
    pb_string(inner, 3, post_id);
    pb_string(inner, 11, channel_id);
    std::string outer;
    pb_string(outer, 56, inner);   // field 56, wire type 2 (a nested message)
    return base64url_nopad(outer);
}

// Find the comments engagement-panel continuation token in a /next(videoId) response.
static std::string comment_section_token(const json& j) {
    const json* panels = find_key(j, "engagementPanels");
    if (panels && panels->is_array()) {
        for (const auto& p : *panels) {
            std::string id = p.value(json::json_pointer(
                "/engagementPanelSectionListRenderer/panelIdentifier"), std::string());
            if (id.find("comment") != std::string::npos) {
                std::string tok = continuation_token(p);
                if (!tok.empty()) return tok;
            }
        }
    }
    return "";
}
// The Top/Newest sort continuations from the comments header's sort submenu. YouTube
// orders them [Top, Newest]; titles are localized so we key off position.
static void comment_sort_tokens(const json& j, std::string& top, std::string& newest) {
    const json* sm = find_key(j, "sortFilterSubMenuRenderer");
    if (!sm) return;
    const json* items = find_key(*sm, "subMenuItems");
    if (!items || !items->is_array()) return;
    auto tok = [](const json& it) {
        return it.value(json::json_pointer("/serviceEndpoint/continuationCommand/token"),
                        std::string());
    };
    if (items->size() > 0) top = tok((*items)[0]);
    if (items->size() > 1) newest = tok((*items)[1]);
}
// The total comment count ("176") from the comments panel header's contextualInfo.
static std::string comment_count_text(const json& j) {
    const json* panels = find_key(j, "engagementPanels");
    if (panels && panels->is_array()) {
        for (const auto& p : *panels) {
            std::string id = p.value(json::json_pointer(
                "/engagementPanelSectionListRenderer/panelIdentifier"), std::string());
            if (id.find("comment") == std::string::npos) continue;
            if (const json* hdr = find_key(p, "engagementPanelTitleHeaderRenderer"))
                if (const json* ci = find_key(*hdr, "contextualInfo"))
                    return ci->value(json::json_pointer("/runs/0/text"), std::string());
        }
    }
    return "";
}

CommentPage Innertube::comments_page(const std::string& continuation, bool use_browse,
                                     int hop_budget) {
    CommentPage page;
    if (continuation.empty()) return page;
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        HttpClient http;
        std::string vd = visitor_token();
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"continuation", continuation}, {"context", {{"client", client}}}};
        // Video comments continue on /next; community-post comments continue on /browse.
        std::string url = std::string(kInnertubeBase) + (use_browse ? "/browse" : "/next");
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return page;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return page;

        // 1) Build key -> content and surfaceKey -> pinned maps from the mutations.
        std::unordered_map<std::string, Comment> content;
        std::unordered_map<std::string, bool> pinned_by_surface;
        if (const json* muts = find_key(j, "mutations")) if (muts->is_array()) {
            for (const auto& m : *muts) {
                const json* pl = m.contains("payload") ? &m["payload"] : nullptr;
                if (!pl) continue;
                if (const json* ce = pl->contains("commentEntityPayload")
                                     ? &(*pl)["commentEntityPayload"] : nullptr) {
                    std::string key = ce->value("key", "");
                    if (key.empty()) continue;
                    Comment c;
                    c.author = ce->value(json::json_pointer("/author/displayName"), std::string());
                    c.is_creator = ce->value(json::json_pointer("/author/isCreator"), false);
                    c.text = ce->value(json::json_pointer("/properties/content/content"), std::string());
                    c.time = ce->value(json::json_pointer("/properties/publishedTime"), std::string());
                    c.likes = ce->value(json::json_pointer("/toolbar/likeCountNotliked"), std::string());
                    std::string rc = ce->value(json::json_pointer("/toolbar/replyCount"), std::string());
                    c.reply_count = rc.empty() ? 0 : std::atoi(rc.c_str());
                    content[key] = std::move(c);
                } else if (const json* se = pl->contains("commentSurfaceEntityPayload")
                                            ? &(*pl)["commentSurfaceEntityPayload"] : nullptr) {
                    std::string key = se->value("key", "");
                    std::string pin = se->value(json::json_pointer("/pinnedText/content"), std::string());
                    if (!key.empty()) pinned_by_surface[key] = !pin.empty();
                }
            }
        }

        // Build a Comment from a commentViewModel node (found in top-level threads and in
        // bare reply items). th_for_reply, when set, is the enclosing commentThreadRenderer
        // (top level) so we can grab its reply continuation.
        auto push_vm = [&](const json& vm_holder, const json* th_for_reply) {
            const json* ckn = find_key(vm_holder, "commentKey");
            const json* skn = find_key(vm_holder, "commentSurfaceKey");
            std::string ckey = ckn && ckn->is_string() ? ckn->get<std::string>() : "";
            std::string skey = skn && skn->is_string() ? skn->get<std::string>() : "";
            auto f = content.find(ckey);
            if (f == content.end()) return false;
            Comment c = f->second;
            if (auto p = pinned_by_surface.find(skey); p != pinned_by_surface.end())
                c.pinned = p->second;
            if (th_for_reply) {   // reply continuation lives on the thread's replies renderer
                if (const json* rp = find_key(*th_for_reply, "commentRepliesRenderer"))
                    c.reply_token = rp->value(json::json_pointer(
                        "/contents/0/continuationItemRenderer/continuationEndpoint"
                        "/continuationCommand/token"), std::string());
                if (c.reply_token.empty())
                    if (const json* cir = th_for_reply->contains("replies")
                                          ? find_key((*th_for_reply)["replies"], "continuationItemRenderer")
                                          : nullptr)
                        c.reply_token = cir->value(json::json_pointer(
                            "/continuationEndpoint/continuationCommand/token"), std::string());
                c.has_creator_reply = find_key(*th_for_reply, "viewRepliesCreatorThumbnail") != nullptr;
            }
            if (!c.text.empty() || !c.author.empty()) page.items.push_back(std::move(c));
            return true;
        };
        // 2) Collect the continuation items (reload/append commands).
        auto handle_items = [&](const json& items) {
            for (const auto& it : items) {
                if (const json* th = it.contains("commentThreadRenderer")
                                     ? &it["commentThreadRenderer"] : nullptr) {
                    if (const json* vm = th->contains("commentViewModel")
                                         ? &(*th)["commentViewModel"] : nullptr) {
                        push_vm(*vm, th); continue;
                    }
                    // Legacy inline commentRenderer fallback.
                    if (const json* cr = find_key(*th, "commentRenderer")) {
                        Comment c;
                        c.author = cr->value(json::json_pointer("/authorText/simpleText"), std::string());
                        if (const json* runs = find_key(*cr, "contentText"))
                            for (auto& run : runs->value("runs", json::array()))
                                c.text += run.value("text", "");
                        c.time = cr->value(json::json_pointer("/publishedTimeText/runs/0/text"), std::string());
                        c.likes = cr->value(json::json_pointer("/voteCount/simpleText"), std::string());
                        c.pinned = find_key(*th, "pinnedCommentBadge") != nullptr;
                        if (!c.text.empty() || !c.author.empty()) page.items.push_back(std::move(c));
                    }
                } else if (it.contains("commentViewModel")) {   // bare reply item
                    push_vm(it, nullptr);
                } else if (it.contains("continuationItemRenderer")) {
                    std::string tok = it["continuationItemRenderer"].value(json::json_pointer(
                        "/continuationEndpoint/continuationCommand/token"), std::string());
                    // The reply-thread continuations also live under commentThreadRenderer;
                    // this top-level one is the next page of comments.
                    if (!tok.empty()) page.continuation = tok;
                }
            }
        };
        // The comment list lands under continuationItems, but the wrapping command varies
        // by page (reloadContinuationItemsCommand / appendContinuationItemsCommand /
        // appendContinuationItemsAction). Just process every continuationItems array.
        std::function<void(const json&)> walk = [&](const json& n) {
            if (n.is_object()) {
                auto it = n.find("continuationItems");
                if (it != n.end() && it->is_array()) handle_items(*it);
                for (auto& kv : n.items()) walk(kv.value());
            } else if (n.is_array()) {
                for (auto& e : n) walk(e);
            }
        };
        walk(j);

        // 3) Header-only response (comments live one hop deeper): follow it through.
        // A community-post token first opens the comments engagement panel, whose own
        // continuation (comment_section_token) then yields the threads.
        if (page.items.empty() && page.continuation.empty())
            page.continuation = comment_section_token(j);
        if (std::getenv("YTC_DEBUG"))
            std::fprintf(stderr, "[comments_page] browse=%d body=%zu items=%zu cont=%zu hop=%d\n",
                         (int)use_browse, r.body.size(), page.items.size(),
                         page.continuation.size(), hop_budget);
        if (page.items.empty() && !page.continuation.empty() && hop_budget > 0)
            return comments_page(page.continuation, use_browse, hop_budget - 1);
        return page;
    } catch (...) { return page; }
}

Innertube::CommentInit Innertube::video_comment_init(const std::string& video_id) {
    CommentInit ci;
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        HttpClient http;
        std::string vd = visitor_token();
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"videoId", video_id}, {"context", {{"client", client}}}};
        std::string url = std::string(kInnertubeBase) + "/next";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return ci;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return ci;
        ci.count = comment_count_text(j);
        comment_sort_tokens(j, ci.sort_top, ci.sort_newest);
        ci.token = comment_section_token(j);
        return ci;
    } catch (...) { return ci; }
}

Innertube::CommentInit Innertube::post_comment_init(const std::string& post_id,
                                                    const std::string& channel_id) {
    CommentInit ci;
    if (channel_id.empty()) return ci;   // need the channel to address the post-detail page
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        HttpClient http;
        std::string vd = visitor_token();
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        // The single-post detail page carries the comments engagement panel, same as a
        // watch page. It's addressed by browseId "FEpost_detail" + a params protobuf.
        json body = {{"browseId", "FEpost_detail"},
                     {"params", post_detail_params(post_id, channel_id)},
                     {"context", {{"client", client}}}};
        std::string url = std::string(kInnertubeBase) + "/browse";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return ci;
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return ci;
        ci.count = comment_count_text(j);
        comment_sort_tokens(j, ci.sort_top, ci.sort_newest);
        ci.token = comment_section_token(j);
        if (ci.token.empty()) ci.token = continuation_token(j);   // inline continuation
        return ci;
    } catch (...) { return ci; }
}

CommentPage Innertube::video_comments(const std::string& video_id,
                                                 const std::string& continuation) {
    CommentInit ci; std::string tok = continuation;
    if (continuation.empty()) { ci = video_comment_init(video_id); tok = ci.token; }
    CommentPage pg = comments_page(tok, /*use_browse=*/false, 3);
    pg.total = ci.count; pg.sort_top = ci.sort_top; pg.sort_newest = ci.sort_newest;
    return pg;
}
CommentPage Innertube::post_comments(const std::string& post_id, const std::string& channel_id,
                                     const std::string& continuation) {
    CommentInit ci; std::string tok = continuation;
    if (continuation.empty()) { ci = post_comment_init(post_id, channel_id); tok = ci.token; }
    CommentPage pg = comments_page(tok, /*use_browse=*/true, 3);
    pg.total = ci.count; pg.sort_top = ci.sort_top; pg.sort_newest = ci.sort_newest;
    return pg;
}
CommentPage Innertube::comment_replies(const std::string& reply_token, bool is_post) {
    CommentPage all;
    std::string cont = reply_token;
    for (int i = 0; i < 20 && !cont.empty(); ++i) {   // follow to the end (replies are few)
        CommentPage pg = comments_page(cont, /*use_browse=*/is_post, 3);
        all.items.insert(all.items.end(),
                         std::make_move_iterator(pg.items.begin()),
                         std::make_move_iterator(pg.items.end()));
        if (pg.items.empty() || pg.continuation == cont) break;   // no progress -> stop
        cont = pg.continuation;
    }
    return all;   // continuation intentionally left empty (fully loaded)
}

std::string Innertube::caption_vtt(const std::string& base_url, const std::string& tlang) {
    if (base_url.empty()) return "";
    try {
        HttpClient http;   // LOCAL: thread-safe
        std::string url = base_url;
        if (url.find("&fmt=") == std::string::npos && url.find("?fmt=") == std::string::npos)
            url += "&fmt=vtt";
        // tlang => YouTube auto-translates the track into that language server-side.
        if (!tlang.empty() && url.find("&tlang=") == std::string::npos)
            url += "&tlang=" + tlang;
        // The timedtext endpoint intermittently 429s — a Google-side rate limit
        // that typically clears within seconds (verified: 200/429/429/200 for the
        // identical URL). Retry a couple of times before reporting "unavailable";
        // callers run this on a worker thread, so the waits cost the UI nothing.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            auto r = http.get(url);
            if (r.ok() && !r.body.empty()) return r.body;
        }
        return "";
    } catch (...) { return ""; }
}

std::string Innertube::playlist_description(const std::string& playlist_id) {
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        HttpClient http;   // LOCAL: safe from a worker thread
        std::string vd = visitor_token();
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        apply_ctx_locale(client, locale());
        json body = {{"browseId", "VL" + playlist_id}, {"context", {{"client", client}}}};
        std::string url = std::string(kInnertubeBase) + "/browse";
        if (!api_key_.empty()) url += "?key=" + api_key_;
        auto r = http.post(url, body.dump(), client_headers(fp));
        if (!r.ok()) return "";
        auto j = json::parse(r.body, nullptr, false);
        if (j.is_discarded()) return "";
        if (const json* pm = find_key(j, "playlistMetadataRenderer"))
            return pm->value("description", "");
        return "";
    } catch (...) { return ""; }
}

std::vector<std::pair<std::string, bool>> Innertube::restricted_cache() {
    std::vector<std::pair<std::string, bool>> out;
    std::ifstream f(config_dir_ + "/restricted_cache.json");
    if (!f) return out;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return out;
    // NOTE: bind before .items() — iterating value(...).items() dangles (the proxy
    // outlives the temporary json it references) and segfaults.
    json chans = cfg.value("channels", json::object());
    for (auto& [k, v] : chans.items())
        if (v.is_boolean()) out.emplace_back(k, v.get<bool>());
    return out;
}

void Innertube::set_restricted_cached(const std::string& channel_id, bool restricted) {
    if (channel_id.empty()) return;
    std::string path = config_dir_ + "/restricted_cache.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    if (!cfg.contains("channels") || !cfg["channels"].is_object())
        cfg["channels"] = json::object();
    cfg["channels"][channel_id] = restricted;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
}

int Innertube::setting_int(const std::string& key, int def) {
    std::ifstream f(config_dir_ + "/settings.json");
    if (!f) return def;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return def;
    auto it = cfg.find(key);
    if (it == cfg.end() || !it->is_number_integer()) return def;
    return it->get<int>();
}

void Innertube::set_setting_int(const std::string& key, int value) {
    std::string path = config_dir_ + "/settings.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    cfg[key] = value;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
}

std::string Innertube::setting_str(const std::string& key, const std::string& def) {
    std::ifstream f(config_dir_ + "/settings.json");
    if (!f) return def;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return def;
    auto it = cfg.find(key);
    if (it == cfg.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

void Innertube::set_setting_str(const std::string& key, const std::string& value) {
    std::string path = config_dir_ + "/settings.json";
    json cfg;
    { std::ifstream f(path); if (f) cfg = json::parse(f, nullptr, false); }
    if (cfg.is_discarded() || !cfg.is_object()) cfg = json::object();
    cfg[key] = value;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
}

// Effective quality: YouTube labels quality by the SHORTER side, so a vertical
// "1080p" Short is 1080x1920 -> quality 1080, not 1920. Using raw height would
// wrongly reject vertical formats under a height cap and pick a lower quality.
static int quality_px(const Format& f) {
    if (f.width > 0 && f.height > 0) return std::min(f.width, f.height);
    return f.height;
}

const Format* VideoInfo::best_video(const VideoPrefs& prefs) const {
    const Format* best = nullptr;
    size_t best_rank = 0;
    int best_q = 0;
    for (const auto& f : formats) {
        if (!f.has_video || f.has_audio) continue;         // video-only adaptive
        if (f.url.empty()) continue;                       // must be playable
        size_t rank = codec_rank(f.codec_family, prefs.codec_priority);
        if (rank == prefs.codec_priority.size()) continue; // codec not allowed
        int q = quality_px(f);
        if (prefs.max_height > 0 && q > prefs.max_height) continue;
        if (!best) { best = &f; best_rank = rank; best_q = q; continue; }
        // Quality-primary, then codec preference, then fps, then bitrate.
        bool better =
            q > best_q ||
            (q == best_q && rank < best_rank) ||
            (q == best_q && rank == best_rank && f.fps > best->fps) ||
            (q == best_q && rank == best_rank && f.fps == best->fps &&
             f.bitrate > best->bitrate);
        if (better) { best = &f; best_rank = rank; best_q = q; }
    }
    return best;
}

// Primary-subtag language match: "en" matches "en" and "en-US".
static bool lang_match(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    auto primary = [](const std::string& s) { return s.substr(0, s.find('-')); };
    return primary(a) == primary(b);
}

const Format* VideoInfo::best_audio(const AudioPrefs& prefs) const {
    // Multi-audio videos carry one full set of audio formats per dub language,
    // with near-identical bitrates — ranking across all of them picks a dub at
    // random. Choose ONE track first: the preferred language if present, else
    // the original, else the flagged default. Single-audio videos (no track
    // metadata) skip this and rank everything as before.
    std::string target;
    bool any_track = false, have_target = false, have_orig = false, have_def = false;
    std::string orig, def;
    for (const auto& f : formats) {
        if (!f.has_audio || f.has_video || f.track_lang.empty()) continue;
        any_track = true;
        if (!have_target && lang_match(f.track_lang, prefs.lang)) {
            target = f.track_lang; have_target = true;
        }
        if (!have_orig && f.track_original) { orig = f.track_lang; have_orig = true; }
        if (!have_def && f.track_default)   { def = f.track_lang; have_def = true; }
    }
    if (any_track && !have_target) target = have_orig ? orig : def;

    const Format* best = nullptr;
    size_t best_rank = 0;
    for (const auto& f : formats) {
        if (!f.has_audio || f.has_video) continue;         // audio-only
        if (f.url.empty()) continue;
        if (any_track && !lang_match(f.track_lang, target)) continue;
        size_t rank = codec_rank(f.codec_family, prefs.codec_priority);
        if (rank == prefs.codec_priority.size()) continue;
        if (!best) { best = &f; best_rank = rank; continue; }
        bool better = rank < best_rank ||
                      (rank == best_rank && f.bitrate > best->bitrate);
        if (better) { best = &f; best_rank = rank; }
    }
    return best;
}

std::vector<AudioTrackInfo> VideoInfo::audio_tracks() const {
    std::vector<AudioTrackInfo> out;
    for (const auto& f : formats) {
        if (!f.has_audio || f.has_video || f.track_lang.empty()) continue;
        bool seen = false;
        for (const auto& t : out) if (t.lang == f.track_lang) { seen = true; break; }
        if (seen) continue;
        out.push_back({f.track_lang, f.track_name, f.track_original, f.track_default});
    }
    // Original first; the rest keep YouTube's order (alphabetical by language).
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].original) { std::rotate(out.begin(), out.begin() + i, out.begin() + i + 1); break; }
    return out;
}

const Format* VideoInfo::best_progressive(int max_height) const {
    const Format* best = nullptr;   // tallest at/below the cap
    const Format* small = nullptr;  // smallest overall (fallback if all exceed the cap)
    for (const auto& f : formats) {
        if (!f.has_video || !f.has_audio || f.url.empty() || f.is_hls) continue;
        if (!small || f.height < small->height) small = &f;
        if (max_height > 0 && f.height > max_height) continue;
        if (!best || f.height > best->height) best = &f;
    }
    return best ? best : small;
}

// ---- Offline downloads ----
std::string Innertube::downloads_dir() {
    // Top-level "downloads/" beside the port (sibling of config/), not inside config/.
    // config_dir_ is ".../ytc/config" (or "config"); step up one to reach the port root.
    auto slash = config_dir_.find_last_of('/');
    std::string root = slash == std::string::npos ? "." : config_dir_.substr(0, slash);
    std::string d = root + "/downloads";
    os_mkdir(d.c_str());   // no-op if it already exists
    return d;
}
std::string Innertube::download_path(const std::string& id) {
    return downloads_dir() + "/" + id + ".mp4";
}
std::string Innertube::download_thumb_path(const std::string& id) {
    return downloads_dir() + "/" + id + ".jpg";
}
bool Innertube::is_downloaded(const std::string& id) {
    if (id.empty()) return false;
    std::ifstream f(download_path(id), std::ios::binary);
    return (bool)f;
}
void Innertube::write_download_info(const std::string& id, const std::string& title,
                                    const std::string& author, const std::string& channel_id,
                                    long length_seconds, const std::string& thumb_url,
                                    const std::string& description) {
    json j;
    j["id"] = id; j["title"] = title; j["author"] = author;
    j["channel_id"] = channel_id; j["length"] = length_seconds;
    j["thumb"] = thumb_url; j["description"] = description;
    j["saved"] = (long long)time(nullptr);
    std::ofstream o(downloads_dir() + "/" + id + ".info");
    if (o) o << j.dump(2) << "\n";
}
std::string Innertube::download_description(const std::string& id) {
    std::ifstream f(downloads_dir() + "/" + id + ".info");
    if (!f) return "";
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return "";
    return j.value("description", std::string());
}
bool Innertube::remove_download(const std::string& id) {
    if (id.empty()) return false;
    std::string dir = downloads_dir();
    std::remove((dir + "/" + id + ".mp4").c_str());
    std::remove((dir + "/" + id + ".info").c_str());
    std::remove((dir + "/" + id + ".jpg").c_str());
    return true;
}
std::vector<std::string> Innertube::download_ids() {
    std::vector<std::string> ids;
    std::string dir = downloads_dir();
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".mp4")
                ids.push_back(n.substr(0, n.size() - 4));
        }
        closedir(d);
    }
    return ids;
}
std::vector<SearchResult> Innertube::downloads() {
    std::string dir = downloads_dir();
    std::vector<std::pair<long long, SearchResult>> items;   // (mtime, tile)
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() <= 5 || n.substr(n.size() - 5) != ".info") continue;
            std::string id = n.substr(0, n.size() - 5);
            std::string mp4 = dir + "/" + id + ".mp4";
            { std::ifstream mf(mp4, std::ios::binary); if (!mf) continue; }   // completed only
            json j; { std::ifstream f(dir + "/" + n); j = json::parse(f, nullptr, false); }
            if (j.is_discarded()) continue;
            SearchResult sr;
            sr.kind = SearchResult::Kind::Video;
            sr.video_id = id;
            sr.title = j.value("title", id);
            sr.author = j.value("author", std::string());
            sr.channel_id = j.value("channel_id", std::string());
            sr.length_seconds = j.value("length", 0);
            std::string thumb = dir + "/" + id + ".jpg";
            { std::ifstream tf(thumb, std::ios::binary);
              sr.thumbnail_url = tf ? thumb : ("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg"); }
            struct stat st{}; long long mt = 0;
            if (stat((dir + "/" + n).c_str(), &st) == 0) mt = (long long)st.st_mtime;
            items.emplace_back(mt, std::move(sr));
        }
        closedir(d);
    }
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });   // newest first
    std::vector<SearchResult> out;
    for (auto& p : items) out.push_back(std::move(p.second));
    return out;
}

// ---------------- SponsorBlock ----------------
namespace {
// Minimal SHA-256 (public-domain style) — only needed for the privacy-preserving
// hash-prefix query, so we don't pull in a crypto dependency for the app code.
struct Sha256 {
    uint32_t h[8]; uint64_t len = 0; uint8_t buf[64]; size_t n = 0;
    static uint32_t ror(uint32_t x,int r){ return (x>>r)|(x<<(32-r)); }
    Sha256(){ static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
              0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; std::memcpy(h,iv,sizeof h); }
    void block(const uint8_t* p){
        static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for(int i=0;i<16;i++) w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
        for(int i=16;i<64;i++){ uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1; }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;i++){ uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25);
            uint32_t ch=(e&f)^(~e&g), t1=hh+S1+ch+k[i]+w[i];
            uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), mj=(a&b)^(a&c)^(b&c), t2=S0+mj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void add(const uint8_t* p,size_t l){ len+=l;
        while(l){ size_t t=64-n; if(t>l)t=l; std::memcpy(buf+n,p,t); n+=t; p+=t; l-=t;
            if(n==64){ block(buf); n=0; } } }
    std::string hex(){ uint64_t bl=len*8; uint8_t pad=0x80; add(&pad,1);
        uint8_t z=0; while(n!=56) add(&z,1);
        uint8_t lb[8]; for(int i=0;i<8;i++) lb[i]=(uint8_t)(bl>>(56-i*8)); add(lb,8);
        static const char* hx="0123456789abcdef"; std::string o;
        for(int i=0;i<8;i++) for(int j=3;j>=0;j--){ uint8_t by=(h[i]>>(j*8))&0xff;
            o+=hx[by>>4]; o+=hx[by&0xf]; } return o; }
};
std::string sha256_hex(const std::string& s){ Sha256 x; x.add((const uint8_t*)s.data(), s.size()); return x.hex(); }
std::string url_encode(const std::string& s){ static const char* hx="0123456789ABCDEF";
    std::string o; for(unsigned char c: s){
        if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') o+=(char)c;
        else { o+='%'; o+=hx[c>>4]; o+=hx[c&0xf]; } } return o; }
} // namespace

std::vector<SponsorSegment> Innertube::sponsor_segments(const std::string& video_id,
                                                        const std::string& categories_csv) {
    std::vector<SponsorSegment> out;
    if (video_id.empty()) return out;
    try {
        // Build the categories JSON array from the CSV.
        std::string cats = "[";
        { std::string tok; bool first = true;
          std::string csv = categories_csv + ",";
          for (char c : csv) {
              if (c == ',') { if (!tok.empty()) { if(!first) cats += ","; cats += "\""+tok+"\""; first=false; tok.clear(); } }
              else if (!std::isspace((unsigned char)c)) tok += c;
          } }
        cats += "]";
        std::string prefix = sha256_hex(video_id).substr(0, 4);
        std::string url = "https://sponsor.ajay.app/api/skipSegments/" + prefix
                        + "?categories=" + url_encode(cats)
                        + "&actionTypes=" + url_encode("[\"skip\"]");
        HttpClient http;   // local -> thread-safe
        auto r = http.get(url, {"User-Agent: ytc/1.0"});
        if (!r.ok()) return out;                     // 404 = no segments for this prefix
        json arr = json::parse(r.body, nullptr, false);
        if (arr.is_discarded() || !arr.is_array()) return out;
        for (const auto& entry : arr) {
            if (entry.value("videoID", "") != video_id) continue;   // exact match (privacy)
            for (const auto& seg : entry.value("segments", json::array())) {
                if (seg.value("actionType", "skip") != "skip") continue;
                auto a = seg.value("segment", json::array());
                if (!a.is_array() || a.size() < 2) continue;
                SponsorSegment s;
                s.start = a[0].get<double>();
                s.end   = a[1].get<double>();
                s.category = seg.value("category", "");
                if (s.end > s.start) out.push_back(s);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const SponsorSegment& a, const SponsorSegment& b){ return a.start < b.start; });
    } catch (...) { out.clear(); }
    return out;
}

} // namespace yt
