// Hardware video-decode detection — plain sysfs/device-tree reads, no deps.
// Shared by the app (to decide whether the Video Decode toggle shows) and by
// ytc_setup (to pick the right lib bundle to offer). Mirrors tools/gpu_probe.sh;
// the survey behind the heuristics lives in docs/HWDEC_SURVEY.md.
#pragma once
#include <dirent.h>
#include <fstream>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#endif

namespace hwdetect {

struct Info {
    std::string soc;       // "qcom", "rockchip", "allwinner", ... ("" unknown)
    std::string gpu;       // "adreno", "mali-blob", "panfrost", ... ("" unknown)
    std::string decoder;   // STATEFUL v4l2 H264 decoder name ("" none) — the only
                           // kind ffmpeg's v4l2_m2m can drive
    std::string stateless; // stateless (Request API) decoder present but unusable
                           // by mainline ffmpeg (rk356x Hantro/rkvdec, cedrus, ...)
    bool has_decoder() const { return !decoder.empty(); }
};

#ifdef __linux__
// A decoder only counts if its OUTPUT (bitstream) queue accepts full-frame
// H264 — the STATEFUL v4l2 protocol ffmpeg's v4l2_m2m implements. Stateless
// decoders advertise H264_SLICE instead (V4L2 Request API): same sysfs look,
// entirely different API, silently ignored by ffmpeg ("no valid device").
// Verified: Venus (RP5) = stateful, works; rk3568-vpu-dec (X55) = stateless.
inline bool stateful_h264(const std::string& devnode) {
    int fd = open(devnode.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    bool ok = false;
    const int types[] = {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_OUTPUT};
    for (int t : types) {
        for (unsigned i = 0; !ok; ++i) {
            v4l2_fmtdesc f{};
            f.type = (v4l2_buf_type)t;
            f.index = i;
            if (ioctl(fd, VIDIOC_ENUM_FMT, &f) != 0) break;
            if (f.pixelformat == V4L2_PIX_FMT_H264) ok = true;
        }
        if (ok) break;
    }
    close(fd);
    return ok;
}
#else
inline bool stateful_h264(const std::string&) { return false; }
#endif

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

    // A v4l2 decoder node whose bitstream queue takes full-frame H264 (the
    // stateful protocol) = hardware decode works with a v4l2m2m ffmpeg.
    // Encoders and RGA/scaler nodes don't count; stateless (Request API)
    // decoders are recorded separately but can't be used by mainline ffmpeg.
    if (DIR* d = opendir("/sys/class/video4linux")) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("video", 0) != 0) continue;
            std::string name = read_file("/sys/class/video4linux/" + n + "/name");
            while (!name.empty() && (name.back() == '\n' || name.back() == ' '))
                name.pop_back();
            bool dec = name.find("dec") != std::string::npos &&
                       name.find("enc") == std::string::npos;
            if (!dec) continue;
            if (stateful_h264("/dev/" + n)) { out.decoder = name; break; }
            if (out.stateless.empty()) out.stateless = name;
        }
        closedir(d);
    }
    return out;
}

} // namespace hwdetect
