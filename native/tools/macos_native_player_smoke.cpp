#include "native_player_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
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

struct PlayerDeleter {
    void operator()(VPMacOSNativePlayer* player) const {
        VPMacOSNativePlayerDestroy(player);
    }
};

double non_black_ratio(const VPMacOSNativeFrame& frame) {
    if (!frame.bgra || frame.bgra_size < 4) {
        return 0.0;
    }
    size_t non_black = 0;
    const size_t pixels = frame.bgra_size / 4;
    for (size_t offset = 0; offset + 3 < frame.bgra_size; offset += 4) {
        const uint8_t b = frame.bgra[offset + 0];
        const uint8_t g = frame.bgra[offset + 1];
        const uint8_t r = frame.bgra[offset + 2];
        if (r > 4 || g > 4 || b > 4) {
            ++non_black;
        }
    }
    return pixels == 0 ? 0.0 : static_cast<double>(non_black) / static_cast<double>(pixels);
}

double non_black_ratio_bgra(const std::vector<uint8_t>& bgra, int width, int height, int stride) {
    if (width <= 0 || height <= 0 || stride < width * 4) {
        return 0.0;
    }
    size_t non_black = 0;
    size_t pixels = 0;
    for (int y = 0; y < height; ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < width; ++x) {
            const size_t offset = row + static_cast<size_t>(x) * 4u;
            if (offset + 3 >= bgra.size()) {
                return 0.0;
            }
            const uint8_t b = bgra[offset + 0];
            const uint8_t g = bgra[offset + 1];
            const uint8_t r = bgra[offset + 2];
            if (r > 4 || g > 4 || b > 4) {
                ++non_black;
            }
            ++pixels;
        }
    }
    return pixels == 0 ? 0.0 : static_cast<double>(non_black) / static_cast<double>(pixels);
}

bool wait_for_frame(VPMacOSNativePlayer* player,
                    VPMacOSNativeFrame& frame,
                    std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char error[1024] = {};
    while (std::chrono::steady_clock::now() < deadline) {
        VPMacOSNativeFrameFree(&frame);
        if (VPMacOSNativePlayerCopyCurrentFrameBGRA(player, &frame, error, sizeof(error)) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "timed out waiting for frame";
    if (error[0] != '\0') {
        std::cerr << ": " << error;
    }
    std::cerr << "\n";
    return false;
}

void count_frame_available(void* user_data) {
    if (!user_data) return;
    auto* count = static_cast<std::atomic<int>*>(user_data);
    count->fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc >= 2 ? argv[1] : default_media_path();
    if (path.empty()) {
        std::cerr << "usage: macos_native_player_smoke <media-path>\n";
        return 2;
    }

    std::unique_ptr<VPMacOSNativePlayer, PlayerDeleter> player(VPMacOSNativePlayerCreate());
    if (!player) {
        std::cerr << "failed to create player\n";
        return 1;
    }

    char error[1024] = {};
    if (VPMacOSNativePlayerOpen(player.get(), path.c_str(), error, sizeof(error)) != 0) {
        std::cerr << "open failed: " << error << "\n";
        return 1;
    }
    if (VPMacOSNativePlayerWidth(player.get()) <= 0 ||
        VPMacOSNativePlayerHeight(player.get()) <= 0 ||
        VPMacOSNativePlayerDurationUs(player.get()) <= 0) {
        std::cerr << "player reported invalid media metadata\n";
        return 1;
    }

    VPMacOSNativeFrame first = {};
    if (!wait_for_frame(player.get(), first, std::chrono::seconds(3))) {
        return 1;
    }
    if (first.width <= 0 || first.height <= 0 || non_black_ratio(first) <= 0.5) {
        std::cerr << "first frame is invalid or unexpectedly black\n";
        VPMacOSNativeFrameFree(&first);
        return 1;
    }
    const int64_t first_pts = first.pts_us;
    const int direct_stride = first.width * 4 + 64;
    std::vector<uint8_t> direct_bgra(
        static_cast<size_t>(direct_stride) * static_cast<size_t>(first.height), 0);
    VPMacOSNativeFrameInfo direct_info = {};
    if (VPMacOSNativePlayerCopyCurrentFrameBGRAInto(
            player.get(),
            direct_bgra.data(),
            direct_bgra.size(),
            first.width,
            first.height,
            direct_stride,
            &direct_info,
            error,
            sizeof(error)) != 0) {
        std::cerr << "direct BGRA copy failed: " << error << "\n";
        VPMacOSNativeFrameFree(&first);
        return 1;
    }
    if (direct_info.pts_us != first_pts ||
        non_black_ratio_bgra(direct_bgra, first.width, first.height, direct_stride) <= 0.5) {
        std::cerr << "direct BGRA copy returned invalid frame info or pixels\n";
        VPMacOSNativeFrameFree(&first);
        return 1;
    }

    std::atomic<int> frame_available_callbacks{0};
    VPMacOSNativePlayerSetFrameAvailableCallback(
        player.get(), count_frame_available, &frame_available_callbacks);
    VPMacOSNativePlayerPlay(player.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    VPMacOSNativeFrame playing = {};
    if (!wait_for_frame(player.get(), playing, std::chrono::seconds(2))) {
        VPMacOSNativeFrameFree(&first);
        return 1;
    }
    if (playing.pts_us <= first_pts) {
        std::cerr << "playback did not advance frames: first=" << first_pts
                  << " playing=" << playing.pts_us << "\n";
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        return 1;
    }
    if (frame_available_callbacks.load(std::memory_order_relaxed) <= 0) {
        std::cerr << "native frame-available callback was not invoked during playback\n";
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        return 1;
    }

    VPMacOSNativePlayerPause(player.get());
    VPMacOSNativePlayerSetFrameAvailableCallback(player.get(), nullptr, nullptr);
    VPMacOSNativePlayerSeek(player.get(), 2'000'000);
    VPMacOSNativeFrame seeked = {};
    if (!wait_for_frame(player.get(), seeked, std::chrono::seconds(3))) {
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        return 1;
    }
    if (seeked.pts_us < 1'500'000) {
        std::cerr << "seek did not reach the requested region: " << seeked.pts_us << "\n";
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        VPMacOSNativeFrameFree(&seeked);
        return 1;
    }

    VPMacOSNativePlayerSetLoopRange(player.get(), 1, 1'000'000, 1'250'000);
    VPMacOSNativePlayerSeek(player.get(), 1'000'000);
    VPMacOSNativePlayerPlay(player.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const int64_t loop_tick_pts = VPMacOSNativePlayerCurrentPtsUs(player.get());
    if (loop_tick_pts > 1'450'000) {
        std::cerr << "loop range was not enforced by native playback tick before frame copy: "
                  << loop_tick_pts << "\n";
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        VPMacOSNativeFrameFree(&seeked);
        return 1;
    }
    VPMacOSNativeFrame looped = {};
    if (!wait_for_frame(player.get(), looped, std::chrono::seconds(3))) {
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        VPMacOSNativeFrameFree(&seeked);
        return 1;
    }
    VPMacOSNativePlayerPause(player.get());
    VPMacOSNativePlayerSetLoopRange(player.get(), 0, 0, 0);
    if (looped.pts_us > 1'450'000) {
        std::cerr << "loop range did not seek back near start: " << looped.pts_us << "\n";
        VPMacOSNativeFrameFree(&first);
        VPMacOSNativeFrameFree(&playing);
        VPMacOSNativeFrameFree(&seeked);
        VPMacOSNativeFrameFree(&looped);
        return 1;
    }

    std::cout << "macOS native player frames: first=" << first_pts
              << " playing=" << playing.pts_us
              << " seeked=" << seeked.pts_us
              << " looped=" << looped.pts_us
              << " loop_tick=" << loop_tick_pts
              << " size=" << first.width << "x" << first.height
              << " non_black=" << non_black_ratio(first) << "\n";

    VPMacOSNativeFrameFree(&first);
    VPMacOSNativeFrameFree(&playing);
    VPMacOSNativeFrameFree(&seeked);
    VPMacOSNativeFrameFree(&looped);
    return 0;
}
