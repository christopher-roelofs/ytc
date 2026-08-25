// Resolve a videoId to a CASTABLE url (single muxed stream) for the cast POC.
// Prefers the HLS manifest (from an HLS client), else a progressive muxed format
// (has both audio+video in one URL). Prints:  <content-type>\t<url>
//
// Build: g++ -std=c++17 tools/cast_resolve.cpp src/innertube.cpp src/http.cpp \
//        -I src -o /tmp/cast_resolve -lcurl
#include "innertube.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* cfg() { const char* e = std::getenv("YTC_CONFIG"); return e ? e : "config/clients.json"; }

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <videoId>\n", argv[0]); return 2; }
    try {
        yt::Innertube it(cfg());
        yt::VideoInfo info = it.resolve(argv[1]);
        if (!info.ok()) { std::fprintf(stderr, "resolve failed: %s / %s\n",
                          info.status.c_str(), info.status_reason.c_str()); return 1; }
        std::fprintf(stderr, "resolved via %s: \"%s\"\n", info.resolved_client.c_str(), info.title.c_str());

        bool prefer_muxed = argc > 2 && std::string(argv[2]) == "muxed";
        // progressive muxed (audio+video in one file, e.g. itag 18/22) — the plainest
        // format for a Cast Default Media Receiver.
        const yt::Format* best = nullptr;
        for (const auto& f : info.formats)
            if (f.has_audio && f.has_video && !f.url.empty())
                if (!best || f.height > best->height) best = &f;
        if (best) std::fprintf(stderr, "progressive muxed available: itag %d %s\n",
                               best->itag, best->quality_label.c_str());
        else std::fprintf(stderr, "no progressive muxed format\n");
        if (prefer_muxed && best) { std::printf("video/mp4\t%s\n", best->url.c_str()); return 0; }
        // HLS manifest — single URL but split audio/video renditions (DMR may reject).
        if (info.hls_manifest_url && !info.hls_manifest_url->empty()) {
            std::printf("application/x-mpegURL\t%s\n", info.hls_manifest_url->c_str());
            return 0;
        }
        if (best) { std::printf("video/mp4\t%s\n", best->url.c_str()); return 0; }
        // 3) fallback: best video-only (plays, but silent) — signals we need HLS/muxed.
        const yt::Format* v = info.best_video();
        if (v && !v->url.empty()) {
            std::fprintf(stderr, "WARNING: only video-only (DASH) available; casting silent\n");
            std::printf("video/mp4\t%s\n", v->url.c_str());
            return 0;
        }
        std::fprintf(stderr, "no castable format found\n");
        return 1;
    } catch (const std::exception& e) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
}
