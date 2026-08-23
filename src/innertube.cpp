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
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>

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

// Parse a lockupViewModel (YouTube's newer video item, used on channel/home tabs
// and continuations) into a video SearchResult. chan_hint = the channel we're
// viewing (lockups don't carry the uploader id on a channel page).
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
            if (const json* rows = find_key(*meta, "metadataRows"))
                for (auto& row : *rows)
                    for (auto& part : row.value("metadataParts", json::array())) {
                        std::string txt = part.value(json::json_pointer("/text/content"), std::string());
                        // First plain row part is the owner; skip labels/track previews.
                        if (!txt.empty() && sr.author.empty() && txt != "Playlist" &&
                            txt.find("View full") == std::string::npos &&
                            txt.find(" · ") == std::string::npos) {
                            sr.author = txt;
                            sr.channel_id = part.value(json::json_pointer(
                                "/text/commandRuns/0/onTap/innertubeCommand/browseEndpoint/browseId"),
                                std::string());
                        }
                    }
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
        // (search Shorts shelves + channel Shorts tab). No duration/date here.
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

Innertube::Feed Innertube::search_feed(const std::string& query) {
    Feed feed; feed.endpoint = "search";
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        json client = json::parse(fp.context_json);
        client["visitorData"] = ensure_visitor_data();
        client["hl"] = "en"; client["gl"] = "US";
        json body = {{"query", query}, {"context", {{"client", client}}}};
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
        std::string vd = visitor_data_;   // pre-warmed by callers; read-only here
        if (!vd.empty()) client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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
        std::string vd = visitor_data_;         // cached by the initial search/browse
        if (vd.empty()) return feed;
        json client = json::parse(fp.context_json);
        client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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
        for (const auto& id : channel_ids) {
            auto r = http_.get("https://www.youtube.com/feeds/videos.xml?channel_id=" + id);
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
static long long approx_age_secs(const std::string& s) {
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
                                               int max_results) {
    std::vector<SearchResult> out;
    try {
        if (channel_ids.empty()) channel_ids = favorite_channel_ids();
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

        // Channel-tab lockups don't name their own uploader — fill from favorites.
        std::unordered_map<std::string, std::string> cname;
        for (auto& [cid, nm] : favorites()) cname[cid] = nm;

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
                if (++taken >= 10) break;                 // cap per channel
            }
            taken = 0;
            for (auto& s : cd.shorts.items) {
                auto it = cd.rss_date.find(s.video_id);
                if (it == cd.rss_date.end()) continue;    // undated (older) Shorts: skip
                s.published_text = it->second;
                if (s.author.empty()) s.author = nm;
                all.push_back(std::move(s));
                if (++taken >= 5) break;                  // cap Shorts per channel
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

std::vector<SearchResult> Innertube::home_playlists(std::vector<std::string> channel_ids) {
    std::vector<SearchResult> out;
    try {
        if (channel_ids.empty()) channel_ids = favorite_channel_ids();
        if (channel_ids.empty()) return out;
        ensure_visitor_data();   // warm once; workers only read the cached token
        std::vector<std::vector<SearchResult>> per(channel_ids.size());
        std::atomic<size_t> next{0};
        std::unordered_map<std::string, std::string> cname;
        for (auto& [cid, nm] : favorites()) cname[cid] = nm;
        auto work = [&]() {
            size_t i;
            while ((i = next.fetch_add(1)) < channel_ids.size()) {
                try {
                    Feed f = channel_playlists_feed(channel_ids[i]);
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
    } catch (...) {}
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
        std::string vd = visitor_data_;   // reuse the cached session token if present
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
        client["visitorData"] = vd; client["hl"] = "en"; client["gl"] = "US";
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

std::vector<std::pair<std::string,std::string>> Innertube::history() {
    std::vector<std::pair<std::string,std::string>> out;
    std::ifstream f(config_dir_ + "/history.json");
    if (!f) return out;
    json cfg = json::parse(f, nullptr, false);
    if (cfg.is_discarded()) return out;
    for (const auto& v : cfg.value("videos", json::array())) {
        std::string id = v.value("id", "");
        if (!id.empty()) out.emplace_back(id, v.value("title", ""));
    }
    return out;
}

void Innertube::add_history(const std::string& video_id, const std::string& title) {
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
    keep.insert(keep.begin(), json{{"id", video_id}, {"title", title}});
    while ((int)keep.size() > kMaxHistory) keep.erase(keep.end() - 1);
    cfg["videos"] = keep;
    std::ofstream o(path); if (!o) return;
    o << cfg.dump(2) << "\n";
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
        std::string vd = visitor_data_;   // reuse cached session token if present
        json client = json::parse(fp->context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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
        std::string vd = visitor_data_;
        json client = json::parse(fp->context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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
        std::string vd = visitor_data_;
        json client = json::parse(fp->context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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
        std::string vd = visitor_data_;
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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

std::string Innertube::caption_vtt(const std::string& base_url) {
    if (base_url.empty()) return "";
    try {
        HttpClient http;   // LOCAL: thread-safe
        std::string url = base_url;
        if (url.find("&fmt=") == std::string::npos && url.find("?fmt=") == std::string::npos)
            url += "&fmt=vtt";
        auto r = http.get(url);
        if (!r.ok()) return "";
        return r.body;
    } catch (...) { return ""; }
}

std::string Innertube::playlist_description(const std::string& playlist_id) {
    try {
        const ClientFingerprint& fp = has_search_client_ ? search_client_ : clients_.front();
        HttpClient http;   // LOCAL: safe from a worker thread
        std::string vd = visitor_data_;
        json client = json::parse(fp.context_json);
        if (!vd.empty()) client["visitorData"] = vd;
        client["hl"] = "en"; client["gl"] = "US";
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
