// Minimal libcurl wrapper: blocking GET/POST returning body as std::string.
// Keeps a persistent easy handle per HttpClient so the session (DNS cache,
// TLS session, keep-alive) is reused across the visitor_id + player calls.
#pragma once
#include <string>
#include <vector>
#include <utility>

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    struct Response {
        long status = 0;
        std::string body;
        bool ok() const { return status >= 200 && status < 300; }
    };

    // headers: list of "Key: Value" strings.
    Response get(const std::string& url,
                 const std::vector<std::string>& headers = {});
    Response post(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers = {});

private:
    void* curl_ = nullptr; // CURL*
    Response perform(const std::string& url,
                     const std::string* post_body,
                     const std::vector<std::string>& headers);
};
