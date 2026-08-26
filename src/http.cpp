#include "http.h"
#include <curl/curl.h>
#include <stdexcept>
#include <cstdlib>
#include <cctype>
#include <fstream>

namespace {
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}
size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t n = size * nmemb;
    auto* out = static_cast<std::vector<std::string>*>(userdata);
    std::string line(ptr, n);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (!line.empty() && line.find(':') != std::string::npos) out->push_back(line);
    return n;
}
// Our curl is statically linked with a compiled-in CA path (/etc/ssl/certs/
// ca-certificates.crt, present on Debian/muOS) — but other CFWs put the bundle
// elsewhere (RockNIX: /etc/pki/tls/certs/ca-bundle.crt). Without a valid CA file,
// TLS verification fails and every HTTPS request errors out (visitor_id HTTP -1 ->
// no search results). Probe common locations once and pin CURLOPT_CAINFO to the
// first that exists so the port is portable across CFWs. Empty -> keep curl default.
struct GlobalInit {
    GlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~GlobalInit() { curl_global_cleanup(); }
};
GlobalInit g_init;
} // namespace

const char* http_ca_bundle() {
    static std::string cached = []() -> std::string {
        if (const char* e = std::getenv("CURL_CA_BUNDLE")) { std::ifstream f(e); if (f) return e; }
        if (const char* e = std::getenv("SSL_CERT_FILE"))   { std::ifstream f(e); if (f) return e; }
        const char* cands[] = {
            "/etc/ssl/certs/ca-certificates.crt",          // Debian/Ubuntu/muOS
            "/etc/pki/tls/certs/ca-bundle.crt",            // RockNIX/Fedora/RHEL
            "/etc/ssl/cert.pem",                           // Alpine/BSD/some
            "/etc/ssl/ca-bundle.pem",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            "/etc/ca-certificates/extracted/tls-ca-bundle.pem",
        };
        for (const char* p : cands) { std::ifstream f(p); if (f) return p; }
        return {};
    }();
    return cached.empty() ? nullptr : cached.c_str();
}

HttpClient::HttpClient() {
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
}

HttpClient::~HttpClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

HttpClient::Response HttpClient::perform(const std::string& url,
                                         const std::string* post_body,
                                         const std::vector<std::string>& headers, long timeout_s) {
    CURL* c = static_cast<CURL*>(curl_);
    curl_easy_reset(c);

    Response resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &resp.resp_headers);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip/deflate
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    if (const char* ca = http_ca_bundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

    if (post_body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body->data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)post_body->size());
    }

    CURLcode rc = curl_easy_perform(c);
    if (hdrs) curl_slist_free_all(hdrs);
    if (rc != CURLE_OK) {
        resp.status = -1;
        // A streaming long-poll (lounge backchannel) delivers data then trips the
        // timeout — keep what was received rather than clobbering it with the error.
        if (resp.body.empty()) resp.body = curl_easy_strerror(rc);
        return resp;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
    return resp;
}

std::string HttpClient::Response::header(const std::string& key) const {
    for (const auto& line : resp_headers) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        if (k.size() != key.size()) continue;
        bool eq = true;
        for (size_t i = 0; i < k.size(); ++i)
            if (std::tolower((unsigned char)k[i]) != std::tolower((unsigned char)key[i])) { eq = false; break; }
        if (!eq) continue;
        size_t v = colon + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        return line.substr(v);
    }
    return "";
}

namespace {
struct DlCtx { std::ofstream* f; long long chunk_bytes; };
size_t file_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* d = static_cast<DlCtx*>(userdata);
    size_t n = size * nmemb;
    d->f->write(ptr, n);
    if (!d->f->good()) return 0;   // short write aborts the transfer
    d->chunk_bytes += (long long)n;
    return n;
}
// Parse the total size from a "Content-Range: bytes A-B/TOTAL" response header.
struct HdrCtx { long long total; };
size_t range_hdr_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t n = size * nmemb;
    std::string line(ptr, n);
    auto slash = line.find('/');
    if (slash != std::string::npos &&
        (line.compare(0, 14, "Content-Range:") == 0 || line.compare(0, 14, "content-range:") == 0)) {
        long long t = std::strtoll(line.c_str() + slash + 1, nullptr, 10);
        if (t > 0) static_cast<HdrCtx*>(userdata)->total = t;
    }
    return n;
}
} // namespace

bool HttpClient::download(const std::string& url, const std::string& dest_path,
                          const std::vector<std::string>& headers,
                          ProgressFn progress, long timeout_s) {
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    CURL* c = static_cast<CURL*>(curl_);
    struct curl_slist* base_hdrs = nullptr;
    for (const auto& h : headers) base_hdrs = curl_slist_append(base_hdrs, h.c_str());
    // googlevideo throttles/closes open-ended GETs (audio streams truncate). Pull the
    // file in bounded range chunks, the same way the player's stream layer does.
    const long long kChunk = 4 * 1024 * 1024;
    long long off = 0, total = -1;
    bool ok = true;
    while (ok) {
        curl_easy_reset(c);
        DlCtx dc{&out, 0};
        HdrCtx hc{-1};
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &dc);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, range_hdr_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &hc);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
        if (const char* ca = http_ca_bundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
        if (base_hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, base_hdrs);
        char range[64];
        std::snprintf(range, sizeof range, "%lld-%lld", off, off + kChunk - 1);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
        CURLcode rc = curl_easy_perform(c);
        long status = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        if (rc != CURLE_OK || status < 200 || status >= 300) { ok = false; break; }
        if (total < 0) total = hc.total;                 // learned from Content-Range
        off += dc.chunk_bytes;
        if (progress && !progress(off, total < 0 ? 0 : total)) { ok = false; break; }  // cancelled
        if (status == 200) break;                        // server ignored Range -> whole file sent
        if (dc.chunk_bytes < kChunk) break;              // last (short) chunk
        if (total >= 0 && off >= total) break;           // done
    }
    if (base_hdrs) curl_slist_free_all(base_hdrs);
    out.close();
    if (!ok) return false;
    return total < 0 ? (off > 0) : (off >= total);       // complete only if we got it all
}

HttpClient::Response HttpClient::get(const std::string& url,
                                     const std::vector<std::string>& headers, long timeout_s) {
    return perform(url, nullptr, headers, timeout_s);
}

HttpClient::Response HttpClient::post(const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& headers, long timeout_s) {
    return perform(url, &body, headers, timeout_s);
}
