// Remux a downloaded video-only stream + audio-only stream into a single .mp4,
// copying packets (no re-encode) via libavformat. Used by the offline-download
// feature to combine adaptive streams the way the player would present them.
#pragma once
#include <string>

namespace ytn {

// True if the build was compiled with libavformat (remuxing available).
bool remux_available();

// True if the libavcodec loaded into THIS process provides the named decoder
// (e.g. "h264_v4l2m2m") — i.e. the matching optional hardware-decode bundle is
// on the library path. The name comes from the manifest bundle's "check"; with
// hwdetect this gates the Video Decode toggle. Always false without libav.
bool avcodec_has_decoder(const std::string& name);

// Probe for a usable hardware-decode DEVICE without playing anything: walks the
// H264 decoder's compiled-in hw configs (VAAPI, VDPAU, VideoToolbox, D3D11,
// CUDA, ...) and tries to create each device context — for VAAPI that is a real
// vaInitialize on the render node. Returns the first device type name that
// initializes ("vaapi", "videotoolbox", ...) or "" if none. Fast (ms). The
// handheld ffmpeg builds have no hw device types, so this is "" there; v4l2m2m
// is a decoder, not a device, and is detected separately.
std::string probe_hw_device();

// Combine video_path + audio_path into out_path (.mp4), stream-copy. Returns false
// on any failure (and leaves no valid output). If audio_path is empty, only the
// video stream is written. Blocking; call from a worker thread.
bool remux_to_mp4(const std::string& video_path, const std::string& audio_path,
                  const std::string& out_path);

} // namespace ytn
