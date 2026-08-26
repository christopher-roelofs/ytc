#include "cast.h"
#include "http.h"
#include "../third_party/json.hpp"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <random>
#include <fstream>
#include <algorithm>
#include <curl/curl.h>
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
// Decode the XML entities that show up in DIAL friendlyNames (e.g. 50&quot; -> 50").
static std::string xml_unescape(std::string s) {
    struct { const char* e; const char* c; } map[] = {
        {"&quot;","\""},{"&apos;","'"},{"&#39;","'"},{"&#34;","\""},
        {"&lt;","<"},{"&gt;",">"},{"&amp;","&"}};   // &amp; last so it doesn't re-trigger
    for (auto& m : map) { std::string e = m.e; size_t p = 0;
        while ((p = s.find(e, p)) != std::string::npos) { s.replace(p, e.size(), m.c); p += std::strlen(m.c); } }
    return s;
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

// ---- Cast v2 protocol (raw TLS via libcurl CONNECT_ONLY) ----
namespace {
void pb_varint(std::string& o, uint64_t v){ do{uint8_t b=v&0x7f; v>>=7; if(v)b|=0x80; o+=(char)b;}while(v); }
void pb_str(std::string& o,int f,const std::string& s){ o+=(char)((f<<3)|2); pb_varint(o,s.size()); o+=s; }
void pb_var(std::string& o,int f,uint64_t v){ o+=(char)((f<<3)|0); pb_varint(o,v); }
std::string cast_frame(const std::string& src,const std::string& dst,const std::string& ns,const std::string& pl){
    std::string m; pb_var(m,1,0); pb_str(m,2,src); pb_str(m,3,dst); pb_str(m,4,ns); pb_var(m,5,0); pb_str(m,6,pl);
    uint32_t len=htonl((uint32_t)m.size()); std::string fr((char*)&len,4); fr+=m; return fr; }
uint64_t pb_getvar(const std::string& b,size_t& i){ uint64_t v=0;int sh=0; while(i<b.size()){uint8_t c=b[i++]; v|=(uint64_t)(c&0x7f)<<sh; if(!(c&0x80))break; sh+=7;} return v; }
struct PMsg{ std::string ns,payload,source; };
PMsg pb_parse(const std::string& b){ PMsg p; size_t i=0;
    while(i<b.size()){ uint64_t tag=pb_getvar(b,i); int f=tag>>3,wt=tag&7;
        if(wt==0) pb_getvar(b,i);
        else if(wt==2){ uint64_t l=pb_getvar(b,i); std::string s=b.substr(i,l); i+=l;
            if(f==2)p.source=s; else if(f==4)p.ns=s; else if(f==6)p.payload=s; }
        else break; }
    return p; }
// Blocking send over a CONNECT_ONLY TLS handle.
bool cc_send(CURL* c, const std::string& d){ size_t off=0;
    for(int guard=0; off<d.size() && guard<2000; ++guard){ size_t n=0; CURLcode rc=curl_easy_send(c,d.data()+off,d.size()-off,&n);
        if(rc==CURLE_OK) off+=n; else if(rc==CURLE_AGAIN) usleep(5000); else return false; }
    return off>=d.size(); }
// Read one length-prefixed frame (4-byte BE length + payload) with a deadline.
bool cc_recv_frame(CURL* c, curl_socket_t fd, std::string& out, int timeout_ms){
    auto readn=[&](char* buf, size_t need)->bool{ size_t got=0; int waited=0;
        while(got<need){ size_t n=0; CURLcode rc=curl_easy_recv(c,buf+got,need-got,&n);
            if(rc==CURLE_OK){ if(n==0){usleep(5000);waited+=5;} else got+=n; }
            else if(rc==CURLE_AGAIN){ fd_set r; FD_ZERO(&r); FD_SET(fd,&r); timeval tv{0,200000};
                select((int)fd+1,&r,nullptr,nullptr,&tv); waited+=200; }
            else return false;
            if(waited>timeout_ms) return false; }
        return true; };
    char lb[4]; if(!readn(lb,4)) return false;
    uint32_t len=ntohl(*(uint32_t*)lb); out.resize(len); return len==0 || readn(&out[0],len);
}
} // namespace

std::string Cast::receiver_screen_id(const std::string& ip) {
    CURL* c = curl_easy_init(); if(!c) return "";
    std::string url = "https://" + ip + ":8009";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
    if (curl_easy_perform(c) != CURLE_OK) { curl_easy_cleanup(c); return ""; }
    curl_socket_t fd = 0; curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &fd);

    const std::string CONN="urn:x-cast:com.google.cast.tp.connection", HEART="urn:x-cast:com.google.cast.tp.heartbeat";
    const std::string RECV="urn:x-cast:com.google.cast.receiver", MDX="urn:x-cast:com.google.youtube.mdx", SRC="sender-0";
    cc_send(c, cast_frame(SRC,"receiver-0",CONN,R"({"type":"CONNECT"})"));
    cc_send(c, cast_frame(SRC,"receiver-0",RECV,R"({"type":"LAUNCH","appId":"233637DE","requestId":1})"));

    std::string transport, screen;
    for (int it=0; it<40 && screen.empty(); ++it) {
        std::string raw; if(!cc_recv_frame(c, fd, raw, 8000)) break;
        PMsg p = pb_parse(raw); json j = json::parse(p.payload, nullptr, false);
        std::string type = j.is_discarded()? "" : j.value("type","");
        if (p.ns==HEART) { if(type=="PING") cc_send(c, cast_frame(SRC,p.source.empty()?"receiver-0":p.source,HEART,R"({"type":"PONG"})")); continue; }
        if (p.ns==RECV && type=="RECEIVER_STATUS" && transport.empty()) {
            for (auto& app : j.value("status",json::object()).value("applications",json::array()))
                if (app.value("appId","")=="233637DE") transport = app.value("transportId","");
            if (!transport.empty()) {
                cc_send(c, cast_frame(SRC,transport,CONN,R"({"type":"CONNECT"})"));
                cc_send(c, cast_frame(SRC,transport,MDX,R"({"type":"getMdxSessionStatus"})"));
            }
        }
        if (p.ns==MDX && type=="mdxSessionStatus") screen = j.value("data",json::object()).value("screenId","");
    }
    curl_easy_cleanup(c);
    return screen;
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
        d.name = xml_unescape(xml_tag(r.body, "friendlyName"));
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
    // An unpaired Cast device (the "· Chromecast" row): launch its YouTube web
    // receiver over Cast to wake it and get an ephemeral screenId. No code needed.
    if (screen.empty() && dev.kind == Kind::CastDevice && !dev.ip.empty())
        screen = receiver_screen_id(dev.ip);
    if (screen.empty()) return s;   // still nothing -> couldn't reach it
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

Cast::NowPlaying Cast::read_events(const Session& s, int aid_in, long timeout_s) {
    NowPlaying np; np.aid = aid_in;
    if (s.sid.empty()) return np;
    HttpClient http;
    // Backchannel GET: RID=rpc, TYPE=xmlhttp, AID=last-seen; the server long-polls.
    std::string qs = bind_qs(s, true);   // includes SID/gsessionid/AID(0); patch RID+extras below
    // bind_qs set RID to the numeric s.rid and AID=0 — rewrite for the event channel.
    auto set = [&](const std::string& key, const std::string& val) {
        size_t p = qs.find(key + "=");
        if (p == std::string::npos) { qs += "&" + key + "=" + val; return; }
        size_t e = qs.find('&', p); std::string rep = key + "=" + val;
        qs.replace(p, (e==std::string::npos?qs.size():e) - p, rep);
    };
    int aid = aid_in < 0 ? 0 : aid_in;
    set("RID", "rpc"); set("AID", std::to_string(aid)); set("CI", "0"); set("TYPE", "xmlhttp");
    auto r = http.get(std::string(LOUNGE) + "/bc/bind?" + qs,
                      {"Origin: https://www.youtube.com"}, timeout_s);
    if (std::getenv("YTC_CASTDBG"))
        std::fprintf(stderr, "[events] HTTP %ld, %zu bytes: %.200s\n", r.status, r.body.size(), r.body.c_str());
    // Streaming long-poll: data arrives, then the timeout trips (status -1). Parse the
    // body whenever it looks like the length-prefixed event stream, ok() or not.
    const std::string& raw = r.body;
    if (raw.empty() || !std::isdigit((unsigned char)raw[0])) return np;
    size_t i = 0;
    while (i < raw.size()) {
        size_t nl = raw.find('\n', i); if (nl == std::string::npos) break;
        long len = std::strtol(raw.substr(i, nl - i).c_str(), nullptr, 10);
        if (len <= 0) break;
        size_t start = nl + 1; if (start + (size_t)len > raw.size()) len = raw.size() - start;
        json arr = json::parse(raw.substr(start, len), nullptr, false);
        if (arr.is_array()) for (auto& ev : arr) {
            if (!ev.is_array() || ev.size() < 2) continue;
            if (ev[0].is_number_integer() && ev[0].get<int>() > np.aid) np.aid = ev[0].get<int>();
            auto& b = ev[1];
            if (!b.is_array() || b.empty() || !b[0].is_string()) continue;
            std::string t = b[0];
            if ((t == "nowPlaying" || t == "onStateChange") && b.size() >= 2 && b[1].is_object()) {
                auto& d = b[1];
                auto num = [&](const char* k)->double { auto it = d.find(k);
                    if (it == d.end()) return -1;
                    return it->is_string() ? std::atof(it->get<std::string>().c_str()) : it->get<double>(); };
                double ct = num("currentTime"), du = num("duration"), st = num("state");
                if (ct >= 0) { np.current_time = ct; np.valid = true; }
                if (du >= 0) np.duration = du;
                if (st >= -1 && d.contains("state")) np.state = (int)st;
            }
        }
        i = start + len;
    }
    return np;
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
