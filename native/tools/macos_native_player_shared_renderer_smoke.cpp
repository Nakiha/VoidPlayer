#include "macos/player/native_player_bridge.h"
#include "tools/test_video_assets.h"

#include <CoreVideo/CoreVideo.h>

#include <atomic>
#include <chrono>
#include <cstring>
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
};

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
                if (pixel[0] > 4 || pixel[1] > 4 || pixel[2] > 4) {
                    ++non_black;
                }
                ++pixels;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return pixels == 0 ? 0.0 : static_cast<double>(non_black) / static_cast<double>(pixels);
}

void count_frame_available(void* user_data) {
    if (!user_data) {
        return;
    }
    auto* count = static_cast<std::atomic<int>*>(user_data);
    count->fetch_add(1, std::memory_order_relaxed);
}

bool wait_for_presented_frame(VPMacOSNativePlayer* player,
                              CVPixelBufferRef target,
                              VPMacOSNativeFrameInfo& info,
                              double& non_black,
                              std::chrono::milliseconds timeout) {
    char error[1024] = {};
    const int timeout_ms = static_cast<int>(timeout.count());
    if (VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
            player, timeout_ms, &info, error, sizeof(error)) != 0) {
        std::cerr << "timed out waiting for shared renderer frame";
        if (error[0] != '\0') {
            std::cerr << ": " << error;
        }
        std::cerr << "\n";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        non_black = non_black_ratio_pixel_buffer(target);
        if (non_black > 0.5) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << "shared renderer frame stayed black after refresh\n";
    return false;
}

bool wait_for_playback_presented_frame(VPMacOSNativePlayer* player,
                                       CVPixelBufferRef target,
                                       int64_t min_pts_us,
                                       VPMacOSNativeFrameInfo& info,
                                       double& non_black,
                                       std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(player, &info) == 0 &&
            info.pts_us > min_pts_us) {
            non_black = non_black_ratio_pixel_buffer(target);
            if (non_black > 0.5) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << "shared renderer playback frame did not advance past "
              << min_pts_us << "us or stayed black\n";
    return false;
}

bool request_refresh_expect_success(VPMacOSNativePlayer* player,
                                    VPMacOSNativeFrameInfo& info,
                                    std::chrono::milliseconds timeout) {
    info = {};
    char error[1024] = {};
    const int ret = VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
        player, static_cast<int>(timeout.count()), &info, error, sizeof(error));
    if (ret != 0) {
        std::cerr << "renderer-owned refresh failed unexpectedly: "
                  << error << "\n";
        return false;
    }
    return true;
}

bool copy_presentation_state(VPMacOSNativePlayer* player,
                             VPMacOSNativeRendererOwnedPresentationState& state) {
    state = {};
    return VPMacOSNativePlayerCopyRendererOwnedPresentationState(player, &state) == 0;
}

bool copy_track_diagnostics(VPMacOSNativePlayer* player,
                            std::vector<VPMacOSNativeTrackDiagnosticInfo>& tracks) {
    size_t count = 0;
    if (VPMacOSNativePlayerCopyTrackDiagnostics(player, nullptr, 0, &count) != 0) {
        return false;
    }
    tracks.assign(count, {});
    size_t copied = 0;
    if (!tracks.empty() &&
        VPMacOSNativePlayerCopyTrackDiagnostics(
            player, tracks.data(), tracks.size(), &copied) != 0) {
        return false;
    }
    tracks.resize(std::min(copied, tracks.size()));
    return true;
}

} // namespace

int main() {
    setenv("VOIDPLAYER_MACOS_SHARED_RENDERER", "1", 1);

    const std::string path = vp_tools::h264_smoke_video_path(VIDEO_TEST_DIR);
    if (path.empty()) {
        std::cerr << "missing h264 smoke video\n";
        return 2;
    }

    constexpr int target_width = 640;
    constexpr int target_height = 360;
    std::unique_ptr<VPMacOSNativePlayer, PlayerDeleter> player(VPMacOSNativePlayerCreate());
    std::unique_ptr<VPMacOSMetalPresentationBackend, MetalBackendDeleter> backend(
        VPMacOSMetalPresentationBackendCreate(target_width, target_height));
    PixelBufferHolder target = make_bgra_pixel_buffer(target_width, target_height);
    if (!player || !backend || !target.buffer) {
        std::cerr << "failed to create shared renderer smoke fixtures\n";
        return 1;
    }

    std::atomic<int> callbacks{0};
    VPMacOSNativePlayerSetFrameAvailableCallback(
        player.get(), count_frame_available, &callbacks);
    if (VPMacOSNativePlayerSetMetalPresentationTarget(
            player.get(), backend.get(), target.buffer, target_width, target_height, 2) != 0) {
        std::cerr << "failed to install shared renderer Metal target\n";
        return 1;
    }
    VPMacOSNativeRendererOwnedPresentationState state = {};
    if (!copy_presentation_state(player.get(), state) ||
        state.renderer_initialized != 0 ||
        state.target_installed == 0 ||
        state.backend_available != 0 ||
        state.target_generation == 0 ||
        state.target_width != target_width ||
        state.target_height != target_height) {
        std::cerr << "shared renderer bridge did not expose pre-open target state\n";
        return 1;
    }

    char error[1024] = {};
    if (VPMacOSNativePlayerOpen(player.get(), path.c_str(), error, sizeof(error)) != 0) {
        std::cerr << "shared renderer open failed: " << error << "\n";
        return 1;
    }
    if (!copy_presentation_state(player.get(), state) ||
        state.renderer_initialized == 0 ||
        state.target_installed == 0 ||
        VPMacOSNativePlayerWidth(player.get()) <= 0 ||
        VPMacOSNativePlayerHeight(player.get()) <= 0 ||
        VPMacOSNativePlayerDurationUs(player.get()) <= 0) {
        std::cerr << "shared renderer bridge did not expose valid metadata\n";
        return 1;
    }
    if (VPMacOSNativePlayerHasAudio(player.get()) != 0 &&
        (VPMacOSNativePlayerAudioSampleRate(player.get()) <= 0 ||
         VPMacOSNativePlayerAudioChannels(player.get()) <= 0 ||
         VPMacOSNativePlayerActiveAudioTrack(player.get()) != 0)) {
        std::cerr << "shared renderer bridge reported invalid audio metadata\n";
        return 1;
    }

    VPMacOSNativeFrameInfo first = {};
    double first_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(), target.buffer, first, first_non_black, std::chrono::seconds(3))) {
        return 1;
    }
    if (!copy_presentation_state(player.get(), state) ||
        state.renderer_initialized == 0 ||
        state.target_installed == 0 ||
        state.backend_available == 0 ||
        state.last_draw_succeeded == 0 ||
        state.draw_failure_count != 0 ||
        state.consecutive_draw_failures != 0 ||
        state.last_successful_frame_pts_us != first.pts_us ||
        state.upload_storage_kind == VPMacOSNativePresentPackageStorageUnavailable ||
        state.last_draw_error[0] != '\0') {
        std::cerr << "shared renderer bridge did not expose successful presentation state\n";
        return 1;
    }
    if (callbacks.load(std::memory_order_relaxed) <= 0) {
        std::cerr << "shared renderer bridge did not publish frame callbacks\n";
        return 1;
    }

    PixelBufferHolder invalid_target =
        make_bgra_pixel_buffer(target_width / 2, target_height / 2);
    if (!invalid_target.buffer) {
        std::cerr << "failed to create invalid presentation target fixture\n";
        return 1;
    }
    const uint64_t failure_count_before = state.draw_failure_count;
    if (VPMacOSNativePlayerSetMetalPresentationTarget(
            player.get(), backend.get(), invalid_target.buffer, target_width, target_height, 2) == 0) {
        std::cerr << "invalid-size target install unexpectedly succeeded\n";
        return 1;
    }
    if (!copy_presentation_state(player.get(), state) ||
        state.renderer_initialized == 0 ||
        state.target_installed == 0 ||
        state.backend_available == 0 ||
        state.draw_failure_count <= failure_count_before ||
        state.consecutive_draw_failures == 0 ||
        std::strstr(state.last_draw_error, "dimensions") == nullptr) {
        std::cerr << "shared renderer bridge did not expose invalid target rejection state: "
                  << "renderer_initialized=" << state.renderer_initialized
                  << " target_installed=" << state.target_installed
                  << " backend_available=" << state.backend_available
                  << " last_draw_succeeded=" << state.last_draw_succeeded
                  << " draw_failure_count=" << state.draw_failure_count
                  << " failure_count_before=" << failure_count_before
                  << " consecutive_draw_failures=" << state.consecutive_draw_failures
                  << " error=" << state.last_draw_error << "\n";
        return 1;
    }
    VPMacOSNativeFrameInfo recovered = {};
    double recovered_non_black = 0.0;
    if (!wait_for_presented_frame(player.get(),
                                  target.buffer,
                                  recovered,
                                  recovered_non_black,
                                  std::chrono::seconds(2)) ||
        !copy_presentation_state(player.get(), state) ||
        state.last_draw_succeeded == 0 ||
        state.consecutive_draw_failures != 0 ||
        state.last_draw_error[0] != '\0') {
        std::cerr << "shared renderer bridge did not preserve target after invalid install\n";
        return 1;
    }

    VPMacOSNativeTrackInfo second_track = {};
    if (VPMacOSNativePlayerAddTrack(
            player.get(), path.c_str(), 1, 1, &second_track, error, sizeof(error)) != 0 ||
        second_track.file_id != 1 ||
        second_track.slot != 1 ||
        second_track.width <= 0 ||
        second_track.height <= 0) {
        std::cerr << "shared renderer bridge add-track failed or reported wrong id: "
                  << error << "\n";
        return 1;
    }
    VPMacOSNativePlayerSetTrackOffset(player.get(), 1, 125'000);
    if (VPMacOSNativePlayerTrackOffsetUs(player.get(), 1) != 125'000) {
        std::cerr << "shared renderer bridge did not retain track offset\n";
        return 1;
    }
    std::vector<VPMacOSNativeTrackDiagnosticInfo> track_diagnostics;
    if (!copy_track_diagnostics(player.get(), track_diagnostics) ||
        track_diagnostics.size() < 2 ||
        track_diagnostics[0].file_id != 0 ||
        track_diagnostics[0].slot != 0 ||
        track_diagnostics[1].file_id != 1 ||
        track_diagnostics[1].slot != 1 ||
        track_diagnostics[1].offset_us != 125'000 ||
        track_diagnostics[1].decode_mode[0] == '\0') {
        std::cerr << "shared renderer bridge per-track diagnostics were invalid\n";
        return 1;
    }
    VPMacOSNativePlayerRemoveTrack(player.get(), 1);
    if (VPMacOSNativePlayerTrackOffsetUs(player.get(), 1) != 0) {
        std::cerr << "shared renderer bridge retained offset for removed track\n";
        return 1;
    }

    VPMacOSNativeLayoutState requested_layout = {};
    requested_layout.mode = 0;
    requested_layout.zoom_ratio = 1.25f;
    requested_layout.order[0] = 0;
    VPMacOSNativePlayerApplyLayout(player.get(), &requested_layout);
    VPMacOSNativeLayoutState layout_snapshot = {};
    if (VPMacOSNativePlayerCopyLayout(player.get(), &layout_snapshot) != 0 ||
        layout_snapshot.zoom_ratio != 1.25f) {
        std::cerr << "shared renderer bridge did not retain layout state\n";
        return 1;
    }

    VPMacOSNativePlayerPlay(player.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    requested_layout.view_offset_x = 12.0f;
    VPMacOSNativePlayerApplyLayout(player.get(), &requested_layout);
    VPMacOSNativeFrameInfo layout_refresh = {};
    if (!request_refresh_expect_success(
            player.get(), layout_refresh, std::chrono::milliseconds(500))) {
        std::cerr << "playing refresh should composite the cached source frame\n";
        return 1;
    }
    VPMacOSNativePlayerPerfStats perf_after_layout_refresh = {};
    if (VPMacOSNativePlayerCopyPerfStats(player.get(), &perf_after_layout_refresh) != 0) {
        std::cerr << "shared renderer bridge could not copy perf stats\n";
        return 1;
    }
    const uint64_t source_cache_samples =
        perf_after_layout_refresh.source_frame_cache_hit_count +
        perf_after_layout_refresh.source_frame_cache_miss_count;
    if (perf_after_layout_refresh.viewport_composite_count == 0 ||
        perf_after_layout_refresh.video_source_update_count == 0 ||
        source_cache_samples == 0) {
        std::cerr << "shared renderer bridge did not expose source-cache/composite stats"
                  << "\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    VPMacOSNativeFrameInfo playing = {};
    double playing_non_black = 0.0;
    if (!wait_for_playback_presented_frame(player.get(),
                                           target.buffer,
                                           first.pts_us,
                                           playing,
                                           playing_non_black,
                                           std::chrono::seconds(2)) ||
        playing.pts_us <= first.pts_us) {
        std::cerr << "shared renderer bridge did not advance during playback: first="
                  << first.pts_us << " playing=" << playing.pts_us << "\n";
        return 1;
    }

    VPMacOSNativePlayerPause(player.get());
    VPMacOSNativePlayerSeek(player.get(), 2'000'000);
    VPMacOSNativeFrameInfo seeked = {};
    double seeked_non_black = 0.0;
    if (!wait_for_presented_frame(
            player.get(), target.buffer, seeked, seeked_non_black, std::chrono::seconds(3)) ||
        seeked.pts_us < 1'500'000) {
        std::cerr << "shared renderer bridge seek did not reach requested region: "
                  << seeked.pts_us << "\n";
        return 1;
    }

    std::cout << "macOS shared renderer bridge smoke passed; first="
              << first.pts_us << " playing=" << playing.pts_us
              << " seeked=" << seeked.pts_us
              << " non_black=" << first_non_black << "\n";
    return 0;
}
