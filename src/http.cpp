#include "http.h"
#include <curl/curl.h>
#include <stdexcept>
#include <cstdlib>
#include <fstream>

namespace {
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
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
                                         const std::vector<std::string>& headers) {
    CURL* c = static_cast<CURL*>(curl_);
    curl_easy_reset(c);

    Response resp;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip/deflate
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
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
        resp.body = curl_easy_strerror(rc);
        return resp;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
    return resp;
}

HttpClient::Response HttpClient::get(const std::string& url,
                                     const std::vector<std::string>& headers) {
    return perform(url, nullptr, headers);
}

HttpClient::Response HttpClient::post(const std::string& url,
                                      const std::string& body,
                                      const std::vector<std::string>& headers) {
    return perform(url, &body, headers);
}
