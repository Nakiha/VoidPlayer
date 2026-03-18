#ifndef VOIDVIEW_NATIVE_MEDIA_PROBE_HPP
#define VOIDVIEW_NATIVE_MEDIA_PROBE_HPP

#include <string>
#include <cstdint>

namespace voidview {

/**
 * Media information structure
 */
struct MediaInfo {
    bool valid = false;
    std::string error_message;

    // Basic info
    int width = 0;
    int height = 0;
    int64_t duration_ms = 0;
    double fps = 0.0;
    std::string codec_name;
    std::string pixel_format;

    // Stream info
    int video_stream_index = -1;
    int audio_stream_index = -1;
    bool has_audio = false;

    // Format info
    std::string format_name;
    std::string format_long_name;
    int64_t bit_rate = 0;
    bool seekable = true;
};

/**
 * Probe media file for information
 *
 * This is a lightweight operation that only reads container metadata,
 * without initializing decoders.
 *
 * @param url Path to media file
 * @return MediaInfo structure with probed information
 */
MediaInfo probe_file(const std::string& url);

} // namespace voidview

#endif // VOIDVIEW_NATIVE_MEDIA_PROBE_HPP
