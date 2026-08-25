// CLI for the cast module (test harness):
//   yt_cast discover
//   yt_cast pair "<tv code>" ["name"]
//   yt_cast play <videoId> [name-substring] [start-seconds]
#include "cast.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static std::string cfg_dir() {
    const char* e = std::getenv("YTC_CONFIG");
    std::string p = e ? e : "config/clients.json";
    auto s = p.find_last_of('/');
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

int main(int argc, char** argv) {
    yt::Cast cast(cfg_dir());
    std::string cmd = argc > 1 ? argv[1] : "discover";

    if (cmd == "discover") {
        auto devs = cast.discover();
        std::printf("%zu device(s):\n", devs.size());
        for (auto& d : devs) {
            const char* k = d.kind == yt::Cast::Kind::DialYouTube ? "DIAL-YouTube"
                          : d.kind == yt::Cast::Kind::CastDevice ? "Cast" : "?";
            const char* st = d.needs_code() ? "NEEDS CODE" : (d.paired ? "paired" : "ready");
            std::printf("  %-26s ip=%-15s %-12s %-10s screen=%s\n",
                        d.name.c_str(), d.ip.c_str(), k, st,
                        d.screen_id.empty() ? "-" : d.screen_id.substr(0, 16).c_str());
        }
        return 0;
    }
    if (cmd == "pair") {
        std::string code = argc > 2 ? argv[2] : "";
        std::string name = argc > 3 ? argv[3] : "";
        std::string sid = cast.pair_with_code(code, name);
        if (sid.empty()) { std::printf("pair failed\n"); return 1; }
        std::printf("paired screenId=%s\n", sid.c_str());
        return 0;
    }
    if (cmd == "play") {
        std::string vid  = argc > 2 ? argv[2] : "";
        std::string want = argc > 3 ? argv[3] : "";
        int start = argc > 4 ? std::atoi(argv[4]) : 0;
        auto devs = cast.discover();
        yt::Cast::Device* pick = nullptr;
        for (auto& d : devs) {
            if (d.needs_code()) continue;
            if (want.empty() || d.name.find(want) != std::string::npos) { pick = &d; break; }
        }
        if (!pick) { std::printf("no ready device (pair one first with: yt_cast pair \"<code>\")\n"); return 1; }
        std::printf("casting %s to '%s' at %ds...\n", vid.c_str(), pick->name.c_str(), start);
        auto s = cast.play(*pick, vid, start);
        std::printf(s.ok ? "OK — playing on the TV\n" : "play failed\n");
        return s.ok ? 0 : 1;
    }
    if (cmd == "chromecast") {   // code-free web-receiver cast to a Cast device (wakes it)
        std::string vid  = argc > 2 ? argv[2] : "";
        std::string want = argc > 3 ? argv[3] : "";
        auto devs = cast.discover();
        yt::Cast::Device* pick = nullptr;
        for (auto& d : devs)
            if (d.kind == yt::Cast::Kind::CastDevice && !d.ip.empty() &&
                (want.empty() || d.name.find(want) != std::string::npos)) { pick = &d; break; }
        if (!pick) { std::printf("no Cast device\n"); return 1; }
        yt::Cast::Device cc = *pick; cc.screen_id.clear();   // force the receiver path
        std::printf("chromecast %s -> %s (launching receiver...)\n", vid.c_str(), cc.name.c_str());
        auto s = cast.play(cc, vid, 0);
        std::printf(s.ok ? "OK — playing (web receiver)\n" : "failed\n");
        if (s.ok) { sleep(6); std::printf("stopVideo -> %d\n", cast.command(s, "stopVideo")); }
        return s.ok ? 0 : 1;
    }
    if (cmd == "test") {   // play, then exercise the remote commands (watch the TV)
        std::string vid  = argc > 2 ? argv[2] : "";
        std::string want = argc > 3 ? argv[3] : "";
        auto devs = cast.discover();
        yt::Cast::Device* pick = nullptr;
        for (auto& d : devs) if (!d.needs_code() && (want.empty() || d.name.find(want)!=std::string::npos)) { pick=&d; break; }
        if (!pick) { std::printf("no ready device\n"); return 1; }
        std::printf("play -> %s\n", pick->name.c_str());
        auto s = cast.play(*pick, vid, 0);
        if (!s.ok) { std::printf("play failed\n"); return 1; }
        sleep(4); std::printf("pause -> %d\n",   cast.command(s, "pause"));
        sleep(3); std::printf("play -> %d\n",    cast.command(s, "play"));
        sleep(3); std::printf("seekTo 60 -> %d\n", cast.command(s, "seekTo", 60));
        sleep(3); std::printf("vol 30 -> %d\n",  cast.command(s, "setVolume", 30));
        sleep(2); std::printf("vol 80 -> %d\n",  cast.command(s, "setVolume", 80));
        return 0;
    }
    std::fprintf(stderr, "usage: %s discover | pair \"<code>\" [name] | play <videoId> [name] [start] | test <videoId> [name]\n", argv[0]);
    return 2;
}
