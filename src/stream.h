// Custom libmpv stream protocol ("ytc://") that fetches googlevideo media via
// libcurl using SMALL BOUNDED range requests. YouTube's iOS-issued URLs (used
// when a video is only available via the IOS client) reject the open-ended
// `Range: bytes=N-` requests ffmpeg makes by default (403), and also reject a
// single full-length range — they require chunked delivery. Routing playback
// through this protocol makes those URLs work and gives us one place to control
// range size, retries, and (future) URL refresh.
#pragma once
#include <string>

struct mpv_handle;

namespace ytn {

// Register the "ytc" protocol on this mpv handle (call once, after init).
void register_stream(mpv_handle* mpv);

// Wrap a real https URL + user-agent into a ytc:// URI for loadfile/audio-add.
std::string wrap_url(const std::string& url, const std::string& user_agent);

} // namespace ytn
