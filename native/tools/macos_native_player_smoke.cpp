#include "native_player_bridge.h"
#include "tools/test_video_assets.h"

#include <CoreVideo/CoreVideo.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

std::string default_media_path() {
    return vp_tools::h264_smoke_video_path(VIDEO_TEST_DIR);
}

struct PlayerDeleter {
    void operator()(VPMacOSNativePlayer* player) const {
        VPMacOSNativePlayerDestroy(player);
    }
};

struct MetalBackendDeleter {
    void operator()(VPMacOSMetalPresentationBackend* backend) const {
        VPMacOSMetalPresentationBackendDestroy(backend);
    }
};

struct PixelBufferHolder {
    CVPixelBufferRef buffer = nullptr;

    ~PixelBufferHolder() {
        if (buffer) {
            CVPixelBufferRelease(buffer);
        }
    }

    PixelBufferHolder() = default;
    PixelBufferHolder(const PixelBufferHolder&) = delete;
    PixelBufferHolder& operator=(const PixelBufferHolder&) = delete;
    PixelBufferHolder(PixelBufferHolder&& other) noexcept : buffer(other.buffer) {
        other.buffer = nullptr;
    }
    PixelBufferHolder& operator=(PixelBufferHolder&& other) noexcept {
        if (this != &other) {
            if (buffer) {
                CVPixelBufferRelease(buffer);
            }
            buffer = other.buffer;
            other.buffer = nullptr;
        }
        return *this;
    }
};

double non_black_ratio_pixel_buffer(CVPixelBufferRef buffer) {
    if (!buffer ||
        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return 0.0;
    }
    const int width = static_cast<int>(CVPixelBufferGetWidth(buffer));
    const int height = static_cast<int>(CVPixelBufferGetHeight(buffer));
    const int stride = static_cast<int>(CVPixelBufferGetBytesPerRow(buffer));
    const auto* bgra = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    size_t non_black = 0;
    size_t pixels = 0;
    if (bgra && width > 0 && height > 0 && stride >= width * 4) {
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                const uint8_t b = pixel[0];
                const uint8_t g = pixel[1];
                const uint8_t r = pixel[2];
                if (r > 4 || g > 4 || b > 4) {
                    ++non_black;
                }
                ++pixels;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return pixels == 0 ? 0.0 : static_cast<double>(non_black) / static_cast<double>(pixels);
}

PixelBufferHolder make_bgra_pixel_buffer(int width, int height) {
    PixelBufferHolder holder;
    const void* keys[] = {kCVPixelBufferMetalCompatibilityKey};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_32BGRA,
        attrs,
        &holder.buffer);
    if (attrs) {
        CFRelease(attrs);
    }
    return holder;
}

bool wait_for_presented_frame(VPMacOSNativePlayer* player,
                              VPMacOSMetalPresentationBackend* backend,
                              CVPixelBufferRef target,
                              int width,
                              int height,
                              VPMacOSNativeFrameInfo& info,
                              double& non_black,
                              std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char error[1024] = {};
    if (VPMacOSNativePlayerSetMetalPresentationTarget(
            player,
            backend,
            target,
            width,
            height,
            1) != 0) {
        std::cerr << "failed to install renderer-owned Metal presentation target\n";
        return false;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        if (VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(
                player,
                &info,
                error,
                sizeof(error)) == 0) {
            non_black = non_black_ratio_pixel_buffer(target);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "timed out waiting for presented frame";
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

bool wait_for_present_package_frame_count(VPMacOSNativePlayer* player,
                                          int width,
                                          int height,
                                          int max_track_slots,
                                          int expected_frame_count,
                                          std::chrono::milliseconds timeout) {
    const size_t max_bytes =
        VPMacOSNativePresentFramePackageMaxBytes(width, height, max_track_slots);
    if (max_bytes == 0) {
        std::cerr << "invalid present package max bytes\n";
        return false;
    }
    std::vector<uint8_t> buffer(max_bytes);
    VPMacOSNativePresentFramePackageInfo package = {};
    char error[1024] = {};
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (VPMacOSNativePlayerCopyPresentFramePackage(
                player,
                buffer.data(),
                buffer.size(),
                width,
                height,
                max_track_slots,
                &package,
                error,
                sizeof(error)) == 0 &&
            package.decision.frame_count >= expected_frame_count) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "timed out waiting for present package frame_count >= "
              << expected_frame_count << ", last="
              << package.decision.frame_count;
    if (error[0] != '\0') {
        std::cerr << ": " << error;
    }
    std::cerr << "\n";
    return false;
}

bool videotoolbox_disabled_by_env() {
    const char* value = std::getenv("VOIDPLAYER_DISABLE_VIDEOTOOLBOX");
    if (!value || value[0] == '\0') {
        return false;
    }
    return std::string(value) != "0" && std::string(value) != "false";
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
    const int target_width = VPMacOSNativePlayerWidth(player.get());
    const int target_height = VPMacOSNativePlayerHeight(player.get());
    std::unique_ptr<VPMacOSMetalPresentationBackend, MetalBackendDeleter> backend(
        VPMacOSMetalPresentationBackendCreate(target_width, target_height));
    PixelBufferHolder target = make_bgra_pixel_buffer(target_width, target_height);
    if (!backend || !target.buffer) {
        std::cerr << "failed to create Metal presentation target for native smoke\n";
        return 1;
    }
    VPMacOSNativePlayerSetTrackOffset(player.get(), 0, 250'000);
    if (VPMacOSNativePlayerTrackOffsetUs(player.get(), 0) != 250'000) {
        std::cerr << "track offset was not retained by macOS native player\n";
        return 1;
    }
    VPMacOSNativePlayerSetTrackOffset(player.get(), 0, 0);

    VPMacOSNativeFrameInfo first = {};
    double first_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(),
            backend.get(),
            target.buffer,
            target_width,
            target_height,
            first,
            first_non_black,
            std::chrono::seconds(3))) {
        return 1;
    }
    if (first.width <= 0 || first.height <= 0 || first_non_black <= 0.5) {
        std::cerr << "first presented frame is invalid or unexpectedly black\n";
        return 1;
    }
    if (videotoolbox_disabled_by_env()) {
        if (std::string(VPMacOSNativePlayerDecodeModeName(player.get())) !=
                "software-fallback" ||
            VPMacOSNativePlayerHardwareDecodeActive(player.get()) != 0) {
            std::cerr << "software fallback decode was not active; mode="
                      << VPMacOSNativePlayerDecodeModeName(player.get()) << "\n";
            return 1;
        }
    } else {
        const bool force_hwdownload =
            std::getenv("VOIDPLAYER_FORCE_VIDEOTOOLBOX_HWDOWNLOAD") != nullptr;
        const std::string expected_mode = force_hwdownload
            ? "videotoolbox-download-to-cpu"
            : "videotoolbox-renderer-owned";
        const int expected_downloads = force_hwdownload ? 1 : 0;
        if (std::string(VPMacOSNativePlayerDecodeModeName(player.get())) != expected_mode ||
            VPMacOSNativePlayerHardwareDecodeActive(player.get()) == 0 ||
            VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(player.get()) != expected_downloads) {
            std::cerr << "VideoToolbox decode mode mismatch; mode="
                      << VPMacOSNativePlayerDecodeModeName(player.get()) << "\n";
            return 1;
        }
    }
    VPMacOSNativeFrameInfo direct_info = {};
    double direct_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(),
            backend.get(),
            target.buffer,
            target_width,
            target_height,
            direct_info,
            direct_non_black,
            std::chrono::seconds(1))) {
        return 1;
    }
    if (direct_info.pts_us != first.pts_us || direct_non_black <= 0.5) {
        std::cerr << "Metal presentation copy returned invalid frame info or pixels\n";
        return 1;
    }

    VPMacOSNativeTrackInfo second_track = {};
    if (VPMacOSNativePlayerAddTrack(
            player.get(), path.c_str(), 1, &second_track, error, sizeof(error)) != 0) {
        std::cerr << "add second native track failed: " << error << "\n";
        return 1;
    }
    if (second_track.file_id != 1 ||
        second_track.slot != 1 ||
        second_track.width <= 0 ||
        second_track.height <= 0 ||
        second_track.duration_us <= 0) {
        std::cerr << "second native track reported invalid metadata\n";
        return 1;
    }
    VPMacOSNativePlayerSetTrackOffset(player.get(), 1, 125'000);
    if (VPMacOSNativePlayerTrackOffsetUs(player.get(), 1) != 125'000 ||
        VPMacOSNativePlayerDurationUs(player.get()) < second_track.duration_us) {
        std::cerr << "second native track offset or duration was not retained\n";
        return 1;
    }
    VPMacOSNativeLayoutState requested_layout = {};
    requested_layout.mode = 1;
    requested_layout.split_pos = 1.5f;
    requested_layout.zoom_ratio = 2.0f;
    requested_layout.view_offset_x = 32.0f;
    requested_layout.view_offset_y = -16.0f;
    requested_layout.pixel_size_mode = 1;
    requested_layout.order[0] = 0;
    requested_layout.order[1] = 1;
    VPMacOSNativePlayerApplyLayout(player.get(), &requested_layout);
    VPMacOSNativeLayoutState layout_snapshot = {};
    if (VPMacOSNativePlayerCopyLayout(player.get(), &layout_snapshot) != 0 ||
        layout_snapshot.mode != 1 ||
        layout_snapshot.split_pos != 1.0f ||
        layout_snapshot.zoom_ratio != 2.0f ||
        layout_snapshot.pixel_size_mode != 1 ||
        layout_snapshot.order[0] != 0 ||
        layout_snapshot.order[1] != 1) {
        std::cerr << "native layout state was not retained or clamped by macOS player\n";
        return 1;
    }
    VPMacOSNativePlayerRemoveTrack(player.get(), 1);
    VPMacOSNativeTrackInfo readded_track = {};
    if (VPMacOSNativePlayerAddTrack(
            player.get(), path.c_str(), 1, &readded_track, error, sizeof(error)) != 0) {
        std::cerr << "re-add second native track failed: " << error << "\n";
        return 1;
    }
    if (VPMacOSNativePlayerTrackOffsetUs(player.get(), 1) != 0) {
        std::cerr << "re-added native track retained stale offset\n";
        return 1;
    }
    if (!wait_for_present_package_frame_count(
            player.get(), target_width, target_height, 2, 2, std::chrono::seconds(2))) {
        std::cerr << "re-added native track did not publish as an un-offset present frame\n";
        return 1;
    }
    VPMacOSNativePlayerRemoveTrack(player.get(), 1);
    VPMacOSNativeLayoutState single_track_layout = {};
    VPMacOSNativePlayerApplyLayout(player.get(), &single_track_layout);

    std::atomic<int> frame_available_callbacks{0};
    VPMacOSNativePlayerSetFrameAvailableCallback(
        player.get(), count_frame_available, &frame_available_callbacks);
    VPMacOSNativePlayerPlay(player.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    VPMacOSNativeFrameInfo playing = {};
    double playing_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(),
            backend.get(),
            target.buffer,
            target_width,
            target_height,
            playing,
            playing_non_black,
            std::chrono::seconds(2))) {
        return 1;
    }
    if (playing.pts_us <= first.pts_us || playing_non_black <= 0.5) {
        std::cerr << "playback did not advance frames: first=" << first.pts_us
                  << " playing=" << playing.pts_us << "\n";
        return 1;
    }
    if (frame_available_callbacks.load(std::memory_order_relaxed) <= 0) {
        std::cerr << "native frame-available callback was not invoked during playback\n";
        return 1;
    }
    VPMacOSNativePresentationSchedulerStats scheduler_stats = {};
    if (std::string(VPMacOSNativePresentationSchedulerName()) !=
            "shared-presentation-scheduler/transitional-thread" ||
        VPMacOSNativePlayerCopyPresentationSchedulerStats(
            player.get(), &scheduler_stats) != 0 ||
        scheduler_stats.tick_count == 0 ||
        scheduler_stats.presentable_tick_count == 0 ||
        scheduler_stats.frame_notification_count == 0 ||
        scheduler_stats.last_present_frame_count <= 0 ||
        scheduler_stats.cached_present_decision_available == 0 ||
        scheduler_stats.deadline_sleep_count == 0 ||
        scheduler_stats.last_deadline_sleep_us < 0 ||
        scheduler_stats.last_deadline_sleep_us > 8000 ||
        scheduler_stats.last_selected_pts_us <= first.pts_us) {
        std::cerr << "native presentation scheduler stats were not updated during playback\n";
        return 1;
    }

    VPMacOSNativePlayerPause(player.get());
    VPMacOSNativePlayerSetFrameAvailableCallback(player.get(), nullptr, nullptr);
    VPMacOSNativePlayerSeek(player.get(), 2'000'000);
    VPMacOSNativeFrameInfo seeked = {};
    double seeked_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(),
            backend.get(),
            target.buffer,
            target_width,
            target_height,
            seeked,
            seeked_non_black,
            std::chrono::seconds(3))) {
        return 1;
    }
    if (seeked.pts_us < 1'500'000 || seeked_non_black <= 0.5) {
        std::cerr << "seek did not reach the requested region: " << seeked.pts_us << "\n";
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
        return 1;
    }
    VPMacOSNativeFrameInfo looped = {};
    double looped_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(),
            backend.get(),
            target.buffer,
            target_width,
            target_height,
            looped,
            looped_non_black,
            std::chrono::seconds(3))) {
        return 1;
    }
    VPMacOSNativePlayerPause(player.get());
    VPMacOSNativePlayerSetLoopRange(player.get(), 0, 0, 0);
    if (looped.pts_us > 1'450'000 || looped_non_black <= 0.5) {
        std::cerr << "loop range did not seek back near start: " << looped.pts_us << "\n";
        return 1;
    }
    if (VPMacOSNativePlayerTrackOffsetUs(player.get(), 1) != 0) {
        std::cerr << "removed native track still reports an offset\n";
        return 1;
    }

    std::cout << "macOS native player frames: first=" << first.pts_us
              << " playing=" << playing.pts_us
              << " seeked=" << seeked.pts_us
              << " looped=" << looped.pts_us
              << " loop_tick=" << loop_tick_pts
              << " size=" << first.width << "x" << first.height
              << " non_black=" << first_non_black << "\n";

    return 0;
}
