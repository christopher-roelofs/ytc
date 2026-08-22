#include "stream.h"
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <curl/curl.h>
#include <cstring>
#include <cstdlib>
#include <string>

namespace ytn {

// Max bytes per underlying HTTP range request. googlevideo iOS URLs 403 on
// open-ended or very large ranges; ~1 MiB chunks are accepted.
static const int64_t kChunk = 1 << 20;

// ---- base64url (no padding) to carry an arbitrary URL+UA inside a URI ----
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static std::string b64encode(const std::string& in) {
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out += B64[(val >> bits) & 0x3F]; bits -= 6; }
    }
    if (bits > -6) out += B64[((val << 8) >> (bits + 8)) & 0x3F];
    return out;
}
static std::string b64decode(const std::string& in) {
    int T[256]; for (int i = 0; i < 256; ++i) T[i] = -1;
    for (int i = 0; i < 64; ++i) T[(unsigned char)B64[i]] = i;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c]; bits += 6;
        if (bits >= 0) { out += char((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}

struct Stream {
    CURL* curl = nullptr;
    std::string url;
    std::string ua;
    int64_t pos = 0;
    int64_t size = 0;       // total content length (from clen= or a probe)
};

// Parse clen=<n> from the googlevideo query; 0 if absent.
static int64_t parse_clen(const std::string& url) {
    auto p = url.find("clen=");
    if (p == std::string::npos) return 0;
    p += 5;
    int64_t v = 0;
    while (p < url.size() && url[p] >= '0' && url[p] <= '9') { v = v*10 + (url[p]-'0'); ++p; }
    return v;
}

struct WriteCtx { char* buf; uint64_t cap; uint64_t got; };
static size_t write_cb(char* ptr, size_t sz, size_t nm, void* u) {
    auto* w = static_cast<WriteCtx*>(u);
    size_t n = sz * nm;
    uint64_t room = w->cap - w->got;
    if (n > room) n = room;             // never overflow mpv's buffer
    std::memcpy(w->buf + w->got, ptr, n);
    w->got += n;
    return sz * nm;                     // report all consumed (range == cap)
}

static int64_t read_fn(void* cookie, char* buf, uint64_t nbytes) {
    auto* s = static_cast<Stream*>(cookie);
    if (s->size > 0 && s->pos >= s->size) return 0;   // EOF
    int64_t want = (int64_t)nbytes;
    if (want > kChunk) want = kChunk;
    if (s->size > 0 && s->pos + want > s->size) want = s->size - s->pos;
    if (want <= 0) return 0;

    char range[64];
    std::snprintf(range, sizeof range, "%lld-%lld",
                  (long long)s->pos, (long long)(s->pos + want - 1));
    WriteCtx w{buf, (uint64_t)want, 0};
    curl_easy_setopt(s->curl, CURLOPT_URL, s->url.c_str());
    curl_easy_setopt(s->curl, CURLOPT_RANGE, range);
    curl_easy_setopt(s->curl, CURLOPT_USERAGENT, s->ua.c_str());
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, &w);
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(s->curl);
    if (rc != CURLE_OK) return -1;
    long code = 0;
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);
    if (code >= 400) return -1;
    s->pos += w.got;
    return (int64_t)w.got;
}

static int64_t seek_fn(void* cookie, int64_t offset) {
    auto* s = static_cast<Stream*>(cookie);
    if (offset < 0) return MPV_ERROR_GENERIC;
    s->pos = offset;
    return offset;
}
static int64_t size_fn(void* cookie) {
    auto* s = static_cast<Stream*>(cookie);
    return s->size > 0 ? s->size : MPV_ERROR_UNSUPPORTED;
}
static void close_fn(void* cookie) {
    auto* s = static_cast<Stream*>(cookie);
    if (s->curl) curl_easy_cleanup(s->curl);
    delete s;
}

static int open_fn(void*, char* uri, mpv_stream_cb_info* info) {
    // uri is "ytn://<base64url(ua \n real_url)>"
    std::string u(uri);
    auto pos = u.find("://");
    std::string payload = (pos == std::string::npos) ? u : u.substr(pos + 3);
    std::string decoded = b64decode(payload);
    auto nl = decoded.find('\n');
    if (nl == std::string::npos) return MPV_ERROR_LOADING_FAILED;

    auto* s = new Stream();
    s->ua = decoded.substr(0, nl);
    s->url = decoded.substr(nl + 1);
    s->size = parse_clen(s->url);
    s->curl = curl_easy_init();
    if (!s->curl) { delete s; return MPV_ERROR_LOADING_FAILED; }

    info->cookie = s;
    info->read_fn = read_fn;
    info->seek_fn = seek_fn;
    info->size_fn = size_fn;
    info->close_fn = close_fn;
    return 0;
}

void register_stream(mpv_handle* mpv) {
    mpv_stream_cb_add_ro(mpv, "ytn", nullptr, open_fn);
}

std::string wrap_url(const std::string& url, const std::string& user_agent) {
    return "ytn://" + b64encode(user_agent + "\n" + url);
}

} // namespace ytn
