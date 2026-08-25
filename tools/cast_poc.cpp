// Google Cast v2 POC — cast a media URL to a Chromecast/Shield's Default Media
// Receiver. No SDK: raw TLS (OpenSSL) + hand-rolled CastMessage protobuf framing.
//
// Build: g++ -std=c++17 tools/cast_poc.cpp -o /tmp/cast_poc -lssl -lcrypto
// Run:   /tmp/cast_poc <cast-ip> <media-url> [content-type]
//        (content-type defaults to video/mp4; use application/x-mpegURL for HLS)
//
// Protocol: connect TLS:8009 -> CONNECT + LAUNCH(CC1AD845) on receiver-0, wait for
// the launched app's transportId, CONNECT to it, then LOAD the media. Responds to
// heartbeat PINGs so the session stays up while it confirms playback.
#include "../third_party/json.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
using json = nlohmann::json;

// ---- protobuf (just what CastMessage needs) ----
static void put_varint(std::string& o, uint64_t v) {
    do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; o += (char)b; } while (v);
}
static void put_str_field(std::string& o, int field, const std::string& s) {
    o += (char)((field << 3) | 2);            // wire type 2 (length-delimited)
    put_varint(o, s.size()); o += s;
}
static void put_varint_field(std::string& o, int field, uint64_t v) {
    o += (char)((field << 3) | 0);            // wire type 0 (varint)
    put_varint(o, v);
}
// CastMessage { 1:proto_ver=0, 2:source, 3:dest, 4:namespace, 5:payload_type=0, 6:payload }
static std::string cast_message(const std::string& src, const std::string& dst,
                                const std::string& ns, const std::string& payload) {
    std::string m;
    put_varint_field(m, 1, 0);
    put_str_field(m, 2, src);
    put_str_field(m, 3, dst);
    put_str_field(m, 4, ns);
    put_varint_field(m, 5, 0);
    put_str_field(m, 6, payload);
    return m;
}
// Minimal field walk: pull namespace (4), payload_utf8 (6), source (2) out of a msg.
static uint64_t get_varint(const std::string& b, size_t& i) {
    uint64_t v = 0; int sh = 0;
    while (i < b.size()) { uint8_t c = b[i++]; v |= (uint64_t)(c & 0x7f) << sh; if (!(c & 0x80)) break; sh += 7; }
    return v;
}
struct Parsed { std::string ns, payload, source; };
static Parsed parse_message(const std::string& b) {
    Parsed p; size_t i = 0;
    while (i < b.size()) {
        uint64_t tag = get_varint(b, i); int field = tag >> 3, wt = tag & 7;
        if (wt == 0) { get_varint(b, i); }
        else if (wt == 2) { uint64_t len = get_varint(b, i); std::string s = b.substr(i, len); i += len;
            if (field == 2) p.source = s; else if (field == 4) p.ns = s; else if (field == 6) p.payload = s; }
        else break;
    }
    return p;
}

// ---- TLS transport ----
static SSL* g_ssl = nullptr;
static void die(const char* m) { std::fprintf(stderr, "FATAL: %s\n", m); exit(1); }

static void send_msg(const std::string& src, const std::string& dst,
                     const std::string& ns, const std::string& payload) {
    std::string m = cast_message(src, dst, ns, payload);
    uint32_t len = htonl((uint32_t)m.size());
    std::string frame((char*)&len, 4); frame += m;
    size_t off = 0;
    while (off < frame.size()) {
        int n = SSL_write(g_ssl, frame.data() + off, frame.size() - off);
        if (n <= 0) die("SSL_write");
        off += n;
    }
    // Log outgoing control payloads (not the giant LOAD media dump).
    std::fprintf(stderr, ">> [%s] %s\n", ns.c_str(),
                 payload.size() < 200 ? payload.c_str() : "(LOAD ...)");
}

static bool read_frame(std::string& out) {
    char lenb[4]; int got = 0;
    while (got < 4) { int n = SSL_read(g_ssl, lenb + got, 4 - got); if (n <= 0) return false; got += n; }
    uint32_t len = ntohl(*(uint32_t*)lenb);
    out.clear(); out.resize(len);
    size_t off = 0;
    while (off < len) { int n = SSL_read(g_ssl, &out[off], len - off); if (n <= 0) return false; off += n; }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <ip> <media-url> [content-type]\n", argv[0]); return 2; }
    std::string ip = argv[1], url = argv[2];
    std::string ctype = argc > 3 ? argv[3] : "video/mp4";

    // TCP connect to :8009
    addrinfo hints{}, *res;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ip.c_str(), "8009", &hints, &res)) die("getaddrinfo");
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) die("connect :8009");
    freeaddrinfo(res);
    timeval tv{8, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // TLS (Cast devices use self-signed certs -> no verification)
    SSL_library_init(); SSL_load_error_strings();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    g_ssl = SSL_new(ctx); SSL_set_fd(g_ssl, fd);
    if (SSL_connect(g_ssl) != 1) { ERR_print_errors_fp(stderr); die("SSL_connect"); }
    std::fprintf(stderr, "TLS connected to %s:8009 (%s)\n", ip.c_str(), SSL_get_cipher(g_ssl));

    const std::string NS_CONN = "urn:x-cast:com.google.cast.tp.connection";
    const std::string NS_HEART = "urn:x-cast:com.google.cast.tp.heartbeat";
    const std::string NS_RECV = "urn:x-cast:com.google.cast.receiver";
    const std::string NS_MEDIA = "urn:x-cast:com.google.cast.media";
    const std::string SRC = "sender-0";

    // 1) CONNECT to the platform receiver, then LAUNCH the Default Media Receiver.
    send_msg(SRC, "receiver-0", NS_CONN, R"({"type":"CONNECT"})");
    send_msg(SRC, "receiver-0", NS_RECV, R"({"type":"LAUNCH","appId":"CC1AD845","requestId":1})");

    std::string transport; bool loaded = false; int reqid = 2;
    // Pump the channel: answer PINGs, watch RECEIVER_STATUS for the app's transportId,
    // then CONNECT + LOAD, then read MEDIA_STATUS to confirm.
    for (int iter = 0; iter < 40; ++iter) {
        std::string raw;
        if (!read_frame(raw)) { std::fprintf(stderr, "(read timeout / closed)\n"); break; }
        Parsed p = parse_message(raw);
        json j = json::parse(p.payload, nullptr, false);
        std::string type = j.is_discarded() ? "" : j.value("type", "");
        if (p.ns == NS_HEART) {
            if (type == "PING") send_msg(SRC, p.source.empty() ? "receiver-0" : p.source, NS_HEART, R"({"type":"PONG"})");
            continue;
        }
        std::fprintf(stderr, "<< [%s] %s\n", p.ns.c_str(),
                     p.payload.size() < 400 ? p.payload.c_str() : "(status ...)");
        if (p.ns == NS_RECV && type == "RECEIVER_STATUS" && transport.empty()) {
            for (auto& app : j.value("status", json::object()).value("applications", json::array())) {
                if (app.value("appId", "") == "CC1AD845") {
                    transport = app.value("transportId", "");
                    std::fprintf(stderr, "== launched, transportId=%s (%s)\n",
                                 transport.c_str(), app.value("displayName","").c_str());
                }
            }
            if (!transport.empty() && !loaded) {
                send_msg(SRC, transport, NS_CONN, R"({"type":"CONNECT"})");
                json load = { {"type","LOAD"}, {"requestId", reqid++}, {"autoplay", true},
                    {"media", { {"contentId", url}, {"streamType","BUFFERED"}, {"contentType", ctype} }} };
                send_msg(SRC, transport, NS_MEDIA, load.dump());
                loaded = true;
            }
        }
        if (p.ns == NS_MEDIA && type == "MEDIA_STATUS") {
            for (auto& st : j.value("status", json::array())) {
                std::string ps = st.value("playerState", "");
                std::fprintf(stderr, "== playerState=%s\n", ps.c_str());
                if (ps == "PLAYING" || ps == "BUFFERING") {
                    std::fprintf(stderr, "\nSUCCESS: media loaded and %s on the receiver.\n", ps.c_str());
                    // Let it settle a moment, then exit (playback continues on the device).
                    for (int k = 0; k < 3; ++k) { std::string r2; if (!read_frame(r2)) break;
                        Parsed pp = parse_message(r2);
                        if (pp.ns == NS_HEART) send_msg(SRC, "receiver-0", NS_HEART, R"({"type":"PONG"})"); }
                    return 0;
                }
            }
        }
    }
    std::fprintf(stderr, "%s\n", loaded ? "loaded (no PLAYING confirmation yet)" : "did not load");
    return loaded ? 0 : 1;
}
