// CLI milestone 1: resolve a videoId to stream URLs via Innertube, print the
// format ladder, and (optionally) verify the chosen URLs serve bytes.
//   ./yt_resolve <videoId> [max_height]
#include "innertube.h"
#include "http.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>

static const char* config_path() {
    const char* env = std::getenv("YTC_CONFIG");
    return env ? env : "config/clients.json";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <videoId> [max_height]\n", argv[0]);
        return 2;
    }
    std::string video_id = argv[1];
    int max_height = argc > 2 ? std::atoi(argv[2]) : 0;

    try {
        auto t0 = std::chrono::steady_clock::now();
        yt::Innertube it(config_path());
        yt::VideoInfo info = it.resolve(video_id);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!info.ok()) {
            std::printf("FAILED [%s]: %s / %s (%.0f ms)\n",
                        info.resolved_client.c_str(), info.status.c_str(),
                        info.status_reason.c_str(), ms);
            return 1;
        }

        std::printf("OK via %s in %.0f ms\n", info.resolved_client.c_str(), ms);
        std::printf("  title : %s\n", info.title.c_str());
        std::printf("  author: %s\n", info.author.c_str());
        std::printf("  length: %lds\n\n", info.length_seconds);

        std::printf("  %-6s %-9s %-6s %-10s %-9s %s\n",
                    "itag", "quality", "fps", "kbps", "kind", "codec");
        for (const auto& f : info.formats) {
            const char* kind = f.has_video && f.has_audio ? "prog"
                             : f.has_video ? "video" : "audio";
            std::printf("  %-6d %-9s %-6d %-10ld %-9s %s\n",
                        f.itag,
                        f.quality_label.empty() ? f.audio_quality.c_str()
                                                : f.quality_label.c_str(),
                        f.fps, f.bitrate / 1000, kind, f.codec.c_str());
        }

        yt::VideoPrefs vprefs;
        vprefs.max_height = max_height;
        const yt::Format* v = info.best_video(vprefs);
        const yt::Format* a = info.best_audio();
        std::printf("\n  chosen video: itag %d %s %s (cap=%d)\n",
                    v ? v->itag : -1, v ? v->quality_label.c_str() : "-",
                    v ? yt::codec_name(v->codec_family) : "", max_height);
        std::printf("  chosen audio: itag %d %ld kbps\n",
                    a ? a->itag : -1, a ? a->bitrate / 1000 : 0);

        // Verify the chosen video URL actually serves bytes (Range 0-1MB).
        if (v && !v->url.empty()) {
            HttpClient probe;
            auto r = probe.get(v->url, {"User-Agent: " + info.user_agent,
                                        "Range: bytes=0-1048575"});
            std::printf("  video URL probe: HTTP %ld, %zu bytes\n",
                        r.status, r.body.size());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
