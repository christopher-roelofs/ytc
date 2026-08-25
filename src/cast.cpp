#include "cast.h"
#include "http.h"
#include "../third_party/json.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <random>
#include <fstream>
#include <algorithm>
using json = nlohmann::json;

namespace yt {

static const char* LOUNGE = "https://www.youtube.com/api/lounge";

// ---- small helpers ----
static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') o += (char)c;
        else { o += '%'; o += hex[c>>4]; o += hex[c&0xf]; }
    }
    return o;
}
static std::string form(const std::vector<std::pair<std::string,std::string>>& kv) {
    std::string o;
    for (auto& p : kv) { if (!o.empty()) o += '&'; o += url_encode(p.first); o += '='; o += url_encode(p.second); }
    return o;
}
// First <tag>..</tag> inner text (naive; DIAL/UPnP XML is flat enough).
static std::string xml_tag(const std::string& s, const std::string& tag) {
    std::string open = "<" + tag, close = "</" + tag + ">";
    auto a = s.find(open); if (a == std::string::npos) return "";
    a = s.find('>', a); if (a == std::string::npos) return "";
    auto b = s.find(close, ++a); if (b == std::string::npos) return "";
    return s.substr(a, b - a);
}
static std::string find_header(const std::string& resp, const std::string& key) {
    size_t i = 0;
    while (i < resp.size()) {
        size_t nl = resp.find('\n', i);
        std::string line = resp.substr(i, (nl==std::string::npos?resp.size():nl) - i);
        auto c = line.find(':');
        if (c != std::string::npos) {
            std::string k = line.substr(0, c);
            if (k.size() == key.size()) {
                bool eq = true;
                for (size_t j=0;j<k.size();++j) if (std::tolower((unsigned char)k[j])!=std::tolower((unsigned char)key[j])){eq=false;break;}
                if (eq) { size_t v=c+1; while(v<line.size()&&(line[v]==' '||line[v]=='\t'||line[v]=='\r'))++v;
                    std::string val=line.substr(v); while(!val.empty()&&(val.back()=='\r'||val.back()==' '))val.pop_back(); return val; }
            }
        }
        if (nl == std::string::npos) break; i = nl + 1;
    }
    return "";
}
static std::mt19937_64& rng() {
    static std::mt19937_64 r(std::random_device{}()); return r;
}
static std::string rand_hex(int bytes) {
    static const char* hex = "0123456789abcdef";
    std::string o; for (int i=0;i<bytes;++i){ unsigned b = (unsigned)(rng()() & 0xff); o+=hex[b>>4]; o+=hex[b&0xf]; } return o;
}

// ---- construction / persistence ----
Cast::Cast(const std::string& config_dir) : config_dir_(config_dir) { ensure_sender_id(); }

std::string Cast::bind_qs(const Session& s, bool with_session) {
    std::vector<std::pair<std::string,std::string>> p = {
        {"device","REMOTE_CONTROL"},{"mdx-version","3"},{"ui","1"},{"v","2"},
        {"name","YTC"},{"app","youtube-desktop"},{"loungeIdToken",s.lounge_token},
        {"id",sender_id_},{"VER","8"},{"CVER","1"},{"zx",rand_hex(4)},{"t","1"},
        {"RID",std::to_string(s.rid)},
    };
    if (with_session) { p.push_back({"SID",s.sid}); p.push_back({"gsessionid",s.gsession}); p.push_back({"AID","0"}); }
    return form(p);
}

void Cast::ensure_sender_id() {
    std::ifstream f(config_dir_ + "/cast.json");
    if (f) { json j = json::parse(f, nullptr, false);
        if (!j.is_discarded()) sender_id_ = j.value("sender_id", ""); }
    if (sender_id_.empty()) {
        sender_id_ = rand_hex(16);   // 128-bit
        // persist (merge with any existing screens)
        json j = json::object();
        std::ifstream in(config_dir_ + "/cast.json");
        if (in) { json e = json::parse(in, nullptr, false); if (!e.is_discarded()) j = e; }
        j["sender_id"] = sender_id_;
        std::ofstream out(config_dir_ + "/cast.json"); out << j.dump(2);
    }
}

void Cast::load_pairings(std::vector<Device>& out) const {
    std::ifstream f(config_dir_ + "/cast.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return;
    for (auto& s : j.value("screens", json::array())) {
        Device d; d.screen_id = s.value("screenId",""); d.name = s.value("name","");
        d.ip = s.value("ip",""); d.paired = true; d.kind = Kind::CastDevice;
        if (!d.screen_id.empty()) out.push_back(std::move(d));
    }
}
std::vector<Cast::Device> Cast::paired() const { std::vector<Device> v; load_pairings(v); return v; }

void Cast::save_pairing(const std::string& screen_id, const std::string& name) {
    json j = json::object();
    std::ifstream in(config_dir_ + "/cast.json");
    if (in) { json e = json::parse(in, nullptr, false); if (!e.is_discarded()) j = e; }
    if (!j.contains("sender_id")) j["sender_id"] = sender_id_;
    json screens = j.value("screens", json::array());
    bool found = false;
    for (auto& s : screens) if (s.value("screenId","") == screen_id) { s["name"] = name; found = true; }
    if (!found) screens.push_back({{"screenId",screen_id},{"name",name}});
    j["screens"] = screens;
    std::ofstream out(config_dir_ + "/cast.json"); out << j.dump(2);
}
void Cast::forget(const std::string& screen_id) {
    std::ifstream in(config_dir_ + "/cast.json");
    if (!in) return; json j = json::parse(in, nullptr, false); if (j.is_discarded()) return;
    json screens = json::array();
    for (auto& s : j.value("screens", json::array())) if (s.value("screenId","") != screen_id) screens.push_back(s);
    j["screens"] = screens;
    std::ofstream out(config_dir_ + "/cast.json"); out << j.dump(2);
}

// ---- discovery ----
std::vector<Cast::Device> Cast::discover(int timeout_ms) {
    std::vector<Device> devs;
    std::vector<Device> pairs; load_pairings(pairs);

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return devs;
    // Short per-recv timeout so we can re-send M-SEARCH periodically (UDP is lossy).
    timeval tv{ 0, 400000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int ttl = 4; setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    const char* ms =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\nMX: 2\r\n"
        "ST: urn:dial-multiscreen-org:service:dial:1\r\n\r\n";
    sockaddr_in mc{}; mc.sin_family=AF_INET; mc.sin_port=htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);

    std::vector<std::pair<std::string,std::string>> found;   // ip -> LOCATION (deduped)
    char buf[2048];
    long waited = 0, resend_at = 0;
    while (waited < timeout_ms) {
        if (waited >= resend_at) {   // (re)broadcast the query a few times over the window
            sendto(sock, ms, std::strlen(ms), 0, (sockaddr*)&mc, sizeof mc);
            resend_at += 900;
        }
        sockaddr_in from{}; socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) { waited += 400; continue; }   // timed out this slice
        buf[n] = 0; std::string txt(buf, n);
        std::string ip = inet_ntoa(from.sin_addr);
        std::string loc = find_header(txt, "LOCATION");
        if (loc.empty()) continue;
        bool dup=false; for (auto& f2:found) if (f2.first==ip) dup=true;
        if (!dup) found.push_back({ip, loc});
    }
    ::close(sock);

    HttpClient http;
    for (auto& [ip, loc] : found) {
        auto r = http.get(loc);
        Device d; d.ip = ip;
        d.app_url = r.header("Application-URL");
        while (!d.app_url.empty() && (d.app_url.back()=='\r'||d.app_url.back()==' ')) d.app_url.pop_back();
        d.name = xml_tag(r.body, "friendlyName");
        if (d.name.empty()) d.name = ip;
        if (!d.app_url.empty()) {
            std::string yt = d.app_url; if (yt.back()!='/') yt += '/'; yt += "YouTube";
            auto yr = http.get(yt);
            if (yr.status == 200) { d.kind = Kind::DialYouTube; d.screen_id = xml_tag(yr.body, "screenId"); }
            else d.kind = Kind::CastDevice;   // YouTube not DIAL-exposed -> needs a code
        }
        // Merge a previously-paired screenId (match by name, then ip).
        if (d.screen_id.empty())
            for (auto& p : pairs) if ((!p.name.empty() && p.name==d.name) || (!p.ip.empty() && p.ip==ip)) {
                d.screen_id = p.screen_id; d.paired = true; break; }
        devs.push_back(std::move(d));
    }
    // Append paired screens we didn't see on the LAN this round (offline/other subnet).
    for (auto& p : pairs) {
        bool seen=false; for (auto& d:devs) if (d.screen_id==p.screen_id) seen=true;
        if (!seen) devs.push_back(p);
    }
    return devs;
}

// ---- pairing (TV code) ----
std::string Cast::pair_with_code(const std::string& code, const std::string& name) {
    std::string digits; for (char c : code) if (std::isdigit((unsigned char)c)) digits += c;
    if (digits.empty()) return "";
    HttpClient http;
    auto r = http.post(std::string(LOUNGE) + "/pairing/get_screen",
                       form({{"pairing_code", digits}}),
                       {"Content-Type: application/x-www-form-urlencoded"});
    if (!r.ok()) return "";
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) return "";
    std::string sid = j.value("screen", json::object()).value("screenId", "");
    std::string nm = name.empty() ? j.value("screen", json::object()).value("name", "TV") : name;
    if (!sid.empty()) save_pairing(sid, nm);
    return sid;
}

// ---- lounge play + commands ----
std::string Cast::lounge_token(const std::string& screen_id) {
    HttpClient http;
    auto r = http.post(std::string(LOUNGE) + "/pairing/get_lounge_token_batch",
                       form({{"screen_ids", screen_id}}),
                       {"Content-Type: application/x-www-form-urlencoded"});
    if (!r.ok()) return "";
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) return "";
    auto screens = j.value("screens", json::array());
    if (screens.empty()) return "";
    return screens[0].value("loungeToken", "");
}

bool Cast::bind(Session& s) {
    HttpClient http;
    s.rid++;
    auto r = http.post(std::string(LOUNGE) + "/bc/bind?" + bind_qs(s, false),
                       "count=0", {"Content-Type: application/x-www-form-urlencoded"});
    if (!r.ok()) return false;
    // Response is repeated "<len>\n<json-array-of-events>".
    const std::string& raw = r.body; size_t i = 0;
    while (i < raw.size()) {
        size_t nl = raw.find('\n', i); if (nl == std::string::npos) break;
        long len = std::strtol(raw.substr(i, nl - i).c_str(), nullptr, 10);
        if (len <= 0) break;
        size_t start = nl + 1; if (start + (size_t)len > raw.size()) len = raw.size() - start;
        json arr = json::parse(raw.substr(start, len), nullptr, false);
        if (arr.is_array()) for (auto& ev : arr)
            if (ev.is_array() && ev.size() >= 2 && ev[1].is_array() && !ev[1].empty() && ev[1][0].is_string()) {
                std::string t = ev[1][0];
                if (t == "c" && ev[1].size() >= 2) s.sid = ev[1][1].get<std::string>();
                else if (t == "S" && ev[1].size() >= 2) s.gsession = ev[1][1].get<std::string>();
            }
        i = start + len;
    }
    return !s.sid.empty() && !s.gsession.empty();
}

bool Cast::send_cmd(Session& s, const std::string& cmd_body) {
    HttpClient http;
    s.rid++;
    auto r = http.post(std::string(LOUNGE) + "/bc/bind?" + bind_qs(s, true),
                       cmd_body, {"Content-Type: application/x-www-form-urlencoded"});
    return r.ok();
}

Cast::Session Cast::play(const Device& dev, const std::string& video_id, int start_seconds) {
    Session s;
    std::string screen = dev.screen_id;
    // A DIAL-YouTube TV whose app isn't running yet has no screenId — launch it.
    if (screen.empty() && dev.kind == Kind::DialYouTube && !dev.app_url.empty()) {
        HttpClient http;
        std::string yt = dev.app_url; if (yt.back()!='/') yt += '/'; yt += "YouTube";
        http.post(yt, "pairingCode=" + rand_hex(16) + "&theme=cl",
                  {"Content-Type: application/x-www-form-urlencoded"});
        for (int i = 0; i < 10 && screen.empty(); ++i) {   // poll for the screenId
            auto yr = http.get(yt);
            if (yr.status == 200) screen = xml_tag(yr.body, "screenId");
            if (screen.empty()) usleep(400000);
        }
    }
    if (screen.empty()) return s;   // Cast device that still needs a code
    s.screen_id = screen;
    s.lounge_token = lounge_token(screen);
    if (s.lounge_token.empty()) return s;
    s.rid = 1000 + (int)(rng()() % 8000);   // random base; then monotonic
    if (!bind(s)) return s;
    std::vector<std::pair<std::string,std::string>> cmd = {
        {"count","1"},{"ofs",std::to_string(s.ofs)},{"req0__sc","setPlaylist"},{"req0_videoId",video_id},
        {"req0_currentTime",std::to_string(start_seconds)},{"req0_currentIndex","0"},
        {"req0_listId",""},{"req0_audioOnly","false"},
        {"req0_prioritizeMobileSenderPlaybackStateOnConnection","true"},
    };
    s.ofs++;
    s.ok = send_cmd(s, form(cmd));
    return s;
}

bool Cast::command(Session& s, const std::string& type, double arg) {
    if (!s.ok || s.sid.empty()) return false;
    std::vector<std::pair<std::string,std::string>> cmd = { {"count","1"},{"ofs",std::to_string(s.ofs)} };
    if (type == "seekTo") { cmd.push_back({"req0__sc","seekTo"}); cmd.push_back({"req0_newTime", std::to_string(arg)}); }
    else if (type == "setVolume") { cmd.push_back({"req0__sc","setVolume"}); cmd.push_back({"req0_volume", std::to_string((int)arg)}); }
    else cmd.push_back({"req0__sc", type});   // play | pause | next | previous | stopVideo
    s.ofs++;
    return send_cmd(s, form(cmd));
}

} // namespace yt
