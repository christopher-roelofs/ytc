// Remux a downloaded video-only stream + audio-only stream into a single .mp4,
// copying packets (no re-encode) via libavformat. Used by the offline-download
// feature to combine adaptive streams the way the player would present them.
#pragma once
#include <string>

namespace ytn {

// True if the build was compiled with libavformat (remuxing available).
bool remux_available();

// True if the libavcodec loaded into THIS process provides the v4l2m2m H264
// decoder — i.e. the optional hardware-decode ffmpeg bundle is on the library
// path. Combined with hwdetect::detect() this gates the Video Decode toggle.
bool hwdec_v4l2_available();

// Combine video_path + audio_path into out_path (.mp4), stream-copy. Returns false
// on any failure (and leaves no valid output). If audio_path is empty, only the
// video stream is written. Blocking; call from a worker thread.
bool remux_to_mp4(const std::string& video_path, const std::string& audio_path,
                  const std::string& out_path);

} // namespace ytn
