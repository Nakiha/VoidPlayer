#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"

#include <iostream>
#include <string>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

std::string default_media_path() {
    const std::string root = VIDEO_TEST_DIR;
    if (root.empty()) {
        return {};
    }
    return root + "/h264_9s_1920x1080.mp4";
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc >= 2 ? argv[1] : default_media_path();
    if (path.empty()) {
        std::cerr << "usage: macos_media_smoke <media-path>\n";
        return 2;
    }

    vr::SeekController seek_controller;
    vr::PacketQueue video_queue;
    vr::DemuxThread demux(path, video_queue, seek_controller);
    if (!demux.open()) {
        std::cerr << "failed to open media: " << path << "\n";
        return 1;
    }

    const auto& stats = demux.stats();
    if (stats.video_stream_index < 0 || stats.width <= 0 || stats.height <= 0) {
        std::cerr << "missing video metadata for: " << path << "\n";
        return 1;
    }
    if (stats.duration_us <= 0) {
        std::cerr << "missing duration metadata for: " << path << "\n";
        return 1;
    }

    std::cout << "opened " << path << "\n"
              << "codec=" << stats.codec_name << "\n"
              << "size=" << stats.width << "x" << stats.height << "\n"
              << "duration_us=" << stats.duration_us << "\n";
    return 0;
}
