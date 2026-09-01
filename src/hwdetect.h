// Hardware video-decode detection — plain sysfs/device-tree reads, no deps.
// Shared by the app (to decide whether the Video Decode toggle shows) and by
// ytc_setup (to pick the right lib bundle to offer). Mirrors tools/gpu_probe.sh;
// the survey behind the heuristics lives in docs/HWDEC_SURVEY.md.
#pragma once
#include <dirent.h>
#include <fstream>
#include <string>

namespace hwdetect {

struct Info {
    std::string soc;       // "qcom", "rockchip", "allwinner", ... ("" unknown)
    std::string gpu;       // "adreno", "mali-blob", "panfrost", ... ("" unknown)
    std::string decoder;   // v4l2 decoder name, e.g. "qcom-venus-decoder" ("" none)
    bool has_decoder() const { return !decoder.empty(); }
};

inline std::string read_file(const std::string& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (auto& c : s) if (c == '\0') c = ',';   // device-tree strings are \0-separated
    return s;
}

inline Info detect() {
    Info out;
    std::string compat = read_file("/proc/device-tree/compatible");
    if (compat.find("qcom,") != std::string::npos)          out.soc = "qcom";
    else if (compat.find("rockchip,") != std::string::npos) out.soc = "rockchip";
    else if (compat.find("allwinner,") != std::string::npos ||
             compat.find("sun50i") != std::string::npos)    out.soc = "allwinner";
    else if (compat.find("amlogic,") != std::string::npos)  out.soc = "amlogic";

    std::ifstream kgsl("/dev/kgsl-3d0"), mali("/dev/mali0");
    if (kgsl)      out.gpu = "adreno";
    else if (mali) out.gpu = "mali-blob";
    else if (out.soc == "qcom") out.gpu = "adreno";   // mainline msm/freedreno

    // A v4l2 mem2mem DECODER node = hardware decode is possible with a
    // v4l2m2m-enabled ffmpeg. Encoders and RGA/scaler nodes don't count.
    if (DIR* d = opendir("/sys/class/video4linux")) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("video", 0) != 0) continue;
            std::string name = read_file("/sys/class/video4linux/" + n + "/name");
            while (!name.empty() && (name.back() == '\n' || name.back() == ' '))
                name.pop_back();
            bool dec = name.find("dec") != std::string::npos &&
                       name.find("enc") == std::string::npos;
            if (dec) { out.decoder = name; break; }
        }
        closedir(d);
    }
    return out;
}

} // namespace hwdetect
