// CLI: search YouTube via Innertube and print result rows.
//   ./yt_search "<query>" [max_results]
#include "innertube.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>

static const char* config_path() {
    const char* env = std::getenv("YTNATIVE_CONFIG");
    return env ? env : "config/clients.json";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s \"<query>\" [max_results]\n", argv[0]);
        return 2;
    }
    std::string query = argv[1];
    int max_results = argc > 2 ? std::atoi(argv[2]) : 20;

    try {
        auto t0 = std::chrono::steady_clock::now();
        yt::Innertube it(config_path());
        auto results = it.search(query, max_results);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        std::printf("\"%s\" -> %zu results in %.0f ms\n\n", query.c_str(),
                    results.size(), ms);
        int i = 1;
        for (const auto& r : results) {
            std::printf("%2d. %s\n", i++, r.title.c_str());
            std::printf("     %s  |  %s  |  %s  |  %s\n",
                        r.author.c_str(),
                        r.length_text.empty() ? "-" : r.length_text.c_str(),
                        r.view_count_text.empty() ? "-" : r.view_count_text.c_str(),
                        r.published_text.empty() ? "-" : r.published_text.c_str());
            std::printf("     id=%s  thumb=%.60s\n", r.video_id.c_str(),
                        r.thumbnail_url.c_str());
        }
        return results.empty() ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
