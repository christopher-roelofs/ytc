#include "innertube.h"
#include "../third_party/json.hpp"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

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
}

std::vector<std::string> Innertube::client_headers(const ClientFingerprint& fp) const {
    return {
        "Content-Type: application/json",
        "User-Agent: " + fp.user_agent,
        "X-YouTube-Client-Name: " + std::to_string(fp.client_name_id),
        "X-YouTube-Client-Version: " + fp.innertube_client_version,
        "X-Goog-Visitor-Id: " + visitor_data_};
}

std::string Innertube::ensure_visitor_data() {
    if (!visitor_data_.empty()) return visitor_data_;
    refresh_visitor_data();
    return visitor_data_;
}

void Innertube::refresh_visitor_data() {
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

    auto r = http_.post(url, body.dump(), headers);
    if (!r.ok()) throw std::runtime_error("visitor_id HTTP " + std::to_string(r.status));
    auto j = json::parse(r.body, nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error("visitor_id: bad JSON");
    visitor_data_ = j.at("responseContext").value("visitorData", "");
    if (visitor_data_.empty())
        throw std::runtime_error("visitor_id: no visitorData in response");
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
    client["hl"] = "en";
    client["gl"] = "US";

    json body = {
        {"videoId", video_id},
        {"context", {{"client", client}}},
        {"contentCheckOk", true},
        {"racyCheckOk", true}};

    std::string url = std::string(kInnertubeBase) + "/player";
    if (!api_key_.empty()) url += "?key=" + api_key_;

    auto r = http_.post(url, body.dump(), client_headers(fp));
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

    const auto& ps = j.value("playabilityStatus", json::object());
    out.status = ps.value("status", "UNKNOWN");
    out.status_reason = ps.value("reason", "");
    if (out.status != "OK") return out;

    const auto& vd = j.value("videoDetails", json::object());
    out.title = vd.value("title", "");
    out.author = vd.value("author", "");
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
    for (const auto& fp : clients_) {
        last = try_client(fp, video_id);
        if (last.ok() && (!last.formats.empty() || last.hls_manifest_url))
            return last;
    }
    return last; // return the last attempt (carries status/reason for diagnosis)
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

// Recursively collect every "videoRenderer" object; robust to layout changes
// across clients (web/mobile/tv wrap them in different section renderers).
static void collect_video_renderers(const json& node, std::vector<const json*>& out) {
    if (node.is_object()) {
        auto it = node.find("videoRenderer");
        if (it != node.end() && it->is_object()) out.push_back(&*it);
        for (auto& [k, v] : node.items()) collect_video_renderers(v, out);
    } else if (node.is_array()) {
        for (const auto& v : node) collect_video_renderers(v, out);
    }
}

std::vector<SearchResult> Innertube::search(const std::string& query, int max_results) {
    const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
    json client = json::parse(fp.context_json);
    client["visitorData"] = ensure_visitor_data();
    client["hl"] = "en";
    client["gl"] = "US";

    json body = {{"query", query}, {"context", {{"client", client}}}};
    std::string url = std::string(kInnertubeBase) + "/search";
    if (!api_key_.empty()) url += "?key=" + api_key_;

    auto r = http_.post(url, body.dump(), client_headers(fp));
    std::vector<SearchResult> results;
    if (!r.ok()) return results;
    auto j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) return results;

    std::vector<const json*> vrs;
    if (j.contains("contents")) collect_video_renderers(j["contents"], vrs);

    for (const json* vrp : vrs) {
        const json& vr = *vrp;
        if (!vr.contains("videoId")) continue;
        SearchResult sr;
        sr.video_id = vr.value("videoId", "");
        if (vr.contains("title")) sr.title = run_text(vr["title"]);
        if (vr.contains("ownerText")) sr.author = run_text(vr["ownerText"]);
        else if (vr.contains("longBylineText")) sr.author = run_text(vr["longBylineText"]);
        if (vr.contains("lengthText")) sr.length_text = run_text(vr["lengthText"]);
        if (vr.contains("viewCountText")) sr.view_count_text = run_text(vr["viewCountText"]);
        if (vr.contains("publishedTimeText")) sr.published_text = run_text(vr["publishedTimeText"]);
        sr.length_seconds = parse_duration(sr.length_text);
        // Use the canonical named thumbnail built from the video id. The
        // thumbnail URLs in the response carry sqp/rs params that make YouTube
        // content-negotiate WebP (which our stb-based decoder can't read);
        // mqdefault.jpg (320x180, 16:9) reliably returns baseline JPEG.
        sr.thumbnail_url = "https://i.ytimg.com/vi/" + sr.video_id + "/mqdefault.jpg";
        if (sr.video_id.empty() || sr.title.empty()) continue;
        results.push_back(std::move(sr));
        if ((int)results.size() >= max_results) break;
    }
    return results;
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

const Format* VideoInfo::best_audio(const AudioPrefs& prefs) const {
    const Format* best = nullptr;
    size_t best_rank = 0;
    for (const auto& f : formats) {
        if (!f.has_audio || f.has_video) continue;         // audio-only
        if (f.url.empty()) continue;
        size_t rank = codec_rank(f.codec_family, prefs.codec_priority);
        if (rank == prefs.codec_priority.size()) continue;
        if (!best) { best = &f; best_rank = rank; continue; }
        bool better = rank < best_rank ||
                      (rank == best_rank && f.bitrate > best->bitrate);
        if (better) { best = &f; best_rank = rank; }
    }
    return best;
}

} // namespace yt
