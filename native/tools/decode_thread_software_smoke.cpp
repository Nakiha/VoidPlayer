#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/decode_thread.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

std::string default_media_path() {
    const std::string root = VIDEO_TEST_DIR;
    return root.empty() ? std::string{} : root + "/h264_9s_1920x1080.mp4";
}

const char* storage_kind_name(vr::FrameStorageKind kind) {
    switch (kind) {
    case vr::FrameStorageKind::CpuRgba:
        return "CpuRgba";
    case vr::FrameStorageKind::CpuNv12:
        return "CpuNv12";
    case vr::FrameStorageKind::CpuPlanarYuv:
        return "CpuPlanarYuv";
    case vr::FrameStorageKind::D3D11Nv12:
        return "D3D11Nv12";
    case vr::FrameStorageKind::D3D11Texture:
        return "D3D11Texture";
    case vr::FrameStorageKind::Empty:
    default:
        return "Empty";
    }
}

bool is_cpu_video_storage(vr::FrameStorageKind kind) {
    return kind == vr::FrameStorageKind::CpuPlanarYuv ||
           kind == vr::FrameStorageKind::CpuNv12 ||
           kind == vr::FrameStorageKind::CpuRgba;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc >= 2 ? argv[1] : default_media_path();
    if (path.empty()) {
        std::cerr << "usage: decode_thread_software_smoke <media-path>\n";
        return 2;
    }

    vr::SeekController seek_controller;
    vr::PacketQueue packet_queue(64);
    vr::TrackBuffer track_buffer(12, 2);
    vr::DemuxThread demux(path, packet_queue, seek_controller);

    if (!demux.open()) {
        std::cerr << "failed to open demux input: " << path << "\n";
        return 1;
    }

    const auto& stats = demux.stats();
    if (!stats.codec_params || stats.time_base.den == 0 || stats.video_stream_index < 0) {
        std::cerr << "demux stats missing video codec parameters\n";
        demux.stop();
        return 1;
    }

    vr::DecodeThread decoder(packet_queue, track_buffer, stats.codec_params, stats.time_base);
    if (!decoder.is_valid()) {
        std::cerr << "decode thread failed to initialize codec context\n";
        demux.stop();
        return 1;
    }
    if (!decoder.start()) {
        std::cerr << "decode thread failed to start\n";
        demux.stop();
        return 1;
    }
    if (!demux.start_thread()) {
        std::cerr << "demux thread failed to start\n";
        decoder.stop();
        demux.stop();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (track_buffer.total_count() < 3 &&
           track_buffer.state() != vr::TrackState::Error &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const size_t queued = track_buffer.total_count();
    const auto state = track_buffer.state();
    auto first = track_buffer.peek(0);

    decoder.stop();
    demux.stop();

    if (state == vr::TrackState::Error) {
        std::cerr << "decode thread entered error state\n";
        return 1;
    }
    if (queued < 3 || !first.has_value()) {
        std::cerr << "expected at least 3 decoded frames, got " << queued << "\n";
        return 1;
    }
    if (first->width <= 0 || first->height <= 0) {
        std::cerr << "decoded frame has invalid dimensions\n";
        return 1;
    }
    const auto kind = first->storage_kind();
    if (!is_cpu_video_storage(kind)) {
        std::cerr << "decoded frame used unexpected storage kind "
                  << storage_kind_name(kind) << "\n";
        return 1;
    }

    std::cout << "decode thread queued " << queued
              << " frames, first=" << first->width << "x" << first->height
              << " storage=" << storage_kind_name(kind)
              << " pts_us=" << first->pts_us << "\n";
    return 0;
}
