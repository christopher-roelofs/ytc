#include "remux.h"

#ifdef YTC_HAVE_AVFORMAT
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}
#include <cstdio>
#include <cstdlib>
#define RXLOG(...) do { if (std::getenv("YTC_DEBUG")) std::fprintf(stderr, "[remux] " __VA_ARGS__); } while (0)

namespace ytn {

bool remux_available() { return true; }

bool hwdec_v4l2_available() {
    return avcodec_find_decoder_by_name("h264_v4l2m2m") != nullptr;
}

namespace {
struct Src {
    AVFormatContext* fmt = nullptr;
    int in_index = -1;      // source stream index of the wanted media type
    int out_index = -1;     // destination stream index
    AVPacket* pkt = nullptr;
    bool eof = false;
    // Advance to the next packet belonging to in_index (skips others). Sets eof.
    void advance() {
        if (!fmt) { eof = true; return; }
        while (true) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) { eof = true; return; }
            if (pkt->stream_index == in_index) return;
            av_packet_unref(pkt);
        }
    }
    // Current packet's DTS in the common AV_TIME_BASE, for cross-stream ordering.
    int64_t dts_us() const {
        int64_t d = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (d == AV_NOPTS_VALUE) return 0;
        return av_rescale_q(d, fmt->streams[in_index]->time_base, AV_TIME_BASE_Q);
    }
};

bool open_src(Src& s, const std::string& path, AVMediaType type, AVFormatContext* ofmt) {
    if (path.empty()) return false;
    int e = avformat_open_input(&s.fmt, path.c_str(), nullptr, nullptr);
    if (e < 0) { RXLOG("open_input(%s) failed %d\n", path.c_str(), e); return false; }
    if ((e = avformat_find_stream_info(s.fmt, nullptr)) < 0) { RXLOG("find_stream_info failed %d\n", e); return false; }
    for (unsigned i = 0; i < s.fmt->nb_streams; ++i)
        if (s.fmt->streams[i]->codecpar->codec_type == type) { s.in_index = (int)i; break; }
    if (s.in_index < 0) { RXLOG("no stream of type %d in %s\n", (int)type, path.c_str()); return false; }
    AVStream* os = avformat_new_stream(ofmt, nullptr);
    if (!os) { RXLOG("new_stream failed\n"); return false; }
    if ((e = avcodec_parameters_copy(os->codecpar, s.fmt->streams[s.in_index]->codecpar)) < 0) {
        RXLOG("parameters_copy failed %d\n", e); return false; }
    os->codecpar->codec_tag = 0;
    s.out_index = os->index;
    s.pkt = av_packet_alloc();
    return s.pkt != nullptr;
}

void close_src(Src& s) {
    if (s.pkt) av_packet_free(&s.pkt);
    if (s.fmt) avformat_close_input(&s.fmt);
}

bool write_one(AVFormatContext* ofmt, Src& s) {
    Src& src = s;
    src.pkt->stream_index = src.out_index;
    av_packet_rescale_ts(src.pkt, src.fmt->streams[src.in_index]->time_base,
                         ofmt->streams[src.out_index]->time_base);
    src.pkt->pos = -1;
    int r = av_interleaved_write_frame(ofmt, src.pkt);
    av_packet_unref(src.pkt);
    return r >= 0;
}
} // namespace

bool remux_to_mp4(const std::string& video_path, const std::string& audio_path,
                  const std::string& out_path) {
    AVFormatContext* ofmt = nullptr;
    if (avformat_alloc_output_context2(&ofmt, nullptr, "mp4", out_path.c_str()) < 0 || !ofmt)
        return false;

    Src v{}, a{};
    bool have_audio = !audio_path.empty();
    bool ok = open_src(v, video_path, AVMEDIA_TYPE_VIDEO, ofmt);
    if (ok && have_audio) ok = open_src(a, audio_path, AVMEDIA_TYPE_AUDIO, ofmt);

    if (!ok) RXLOG("open_src failed (v=%p a=%p)\n", (void*)v.fmt, (void*)a.fmt);
    if (ok && !(ofmt->oformat->flags & AVFMT_NOFILE)) {
        int e = avio_open(&ofmt->pb, out_path.c_str(), AVIO_FLAG_WRITE);
        if (e < 0) { RXLOG("avio_open(%s) failed %d\n", out_path.c_str(), e); ok = false; }
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);   // moov up front -> instant seek/play
    if (ok) { int e = avformat_write_header(ofmt, &opts); if (e < 0) { RXLOG("write_header failed %d\n", e); ok = false; } }
    av_dict_free(&opts);

    if (ok) {
        v.advance();
        if (have_audio) a.advance();
        // Merge by DTS so the muxer never has to buffer a whole stream.
        while (ok && (!v.eof || (have_audio && !a.eof))) {
            bool take_video;
            if (v.eof) take_video = false;
            else if (!have_audio || a.eof) take_video = true;
            else take_video = v.dts_us() <= a.dts_us();
            Src& s = take_video ? v : a;
            if (!write_one(ofmt, s)) { ok = false; break; }
            s.advance();
        }
    }

    if (ok) av_write_trailer(ofmt);
    close_src(v);
    if (have_audio) close_src(a);
    if (ofmt) {
        if (!(ofmt->oformat->flags & AVFMT_NOFILE) && ofmt->pb) avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
    }
    if (!ok) std::remove(out_path.c_str());
    return ok;
}

} // namespace ytn

#else  // no libavformat at build time

namespace ytn {
bool remux_available() { return false; }
bool hwdec_v4l2_available() { return false; }
bool remux_to_mp4(const std::string&, const std::string&, const std::string&) { return false; }
} // namespace ytn

#endif
