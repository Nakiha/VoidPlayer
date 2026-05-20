#include "preview_frame_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct Metrics {
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
    uint64_t hash = 14695981039346656037ull;
};

Metrics measure(const VPMacOSDecodedFrame& frame) {
    Metrics metrics;
    if (!frame.bgra || frame.width <= 0 || frame.height <= 0) {
        return metrics;
    }

    double luma_sum = 0.0;
    int non_black = 0;
    const int pixel_count = frame.width * frame.height;
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.bgra + static_cast<size_t>(y) * frame.width * 4;
        for (int x = 0; x < frame.width; ++x) {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            luma_sum += 0.2126 * static_cast<double>(r) +
                        0.7152 * static_cast<double>(g) +
                        0.0722 * static_cast<double>(b);
            if (r > 4 || g > 4 || b > 4) {
                ++non_black;
            }
            metrics.hash ^= r;
            metrics.hash *= 1099511628211ull;
            metrics.hash ^= g;
            metrics.hash *= 1099511628211ull;
            metrics.hash ^= b;
            metrics.hash *= 1099511628211ull;
        }
    }
    metrics.avg_luma = luma_sum / static_cast<double>(std::max(1, pixel_count));
    metrics.non_black_ratio =
        static_cast<double>(non_black) / static_cast<double>(std::max(1, pixel_count));
    return metrics;
}

bool decode_frame(const std::string& path, int64_t pts_us, VPMacOSDecodedFrame& frame) {
    char error[1024] = {};
    const int ret =
        VPMacOSDecodeVideoFrameBGRA(path.c_str(), pts_us, &frame, error, sizeof(error));
    if (ret != 0) {
        std::cerr << "decode failed at " << pts_us << " us: " << error
                  << " (" << ret << ")\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::string path = std::string(VIDEO_TEST_DIR) + "/h264_9s_1920x1080.mp4";
    VPMacOSDecodedFrame first = {};
    VPMacOSDecodedFrame seek = {};

    if (!decode_frame(path, 0, first) || !decode_frame(path, 2500000, seek)) {
        VPMacOSDecodedFrameFree(&first);
        VPMacOSDecodedFrameFree(&seek);
        return EXIT_FAILURE;
    }

    const Metrics first_metrics = measure(first);
    const Metrics seek_metrics = measure(seek);
    const bool geometry_ok = first.width == 1920 && first.height == 1080 &&
                             seek.width == first.width && seek.height == first.height;
    const bool duration_ok = std::llabs(first.duration_us - 9999956) < 10000 &&
                             std::llabs(seek.duration_us - first.duration_us) < 10000;
    const bool frames_ok = first_metrics.non_black_ratio > 0.5 &&
                           seek_metrics.non_black_ratio > 0.5 &&
                           first_metrics.hash != seek_metrics.hash;
    const bool seek_pts_ok = seek.pts_us >= 2000000 && seek.pts_us <= 3000000;

    std::cout << "first=" << first.width << "x" << first.height
              << " pts=" << first.pts_us
              << " duration=" << first.duration_us
              << " hash=" << first_metrics.hash
              << " luma=" << first_metrics.avg_luma << "\n";
    std::cout << "seek=" << seek.width << "x" << seek.height
              << " pts=" << seek.pts_us
              << " duration=" << seek.duration_us
              << " hash=" << seek_metrics.hash
              << " luma=" << seek_metrics.avg_luma << "\n";

    VPMacOSDecodedFrameFree(&first);
    VPMacOSDecodedFrameFree(&seek);
    return geometry_ok && duration_ok && frames_ok && seek_pts_ok ? EXIT_SUCCESS
                                                                  : EXIT_FAILURE;
}
