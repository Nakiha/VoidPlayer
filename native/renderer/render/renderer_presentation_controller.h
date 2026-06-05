#pragma once

#include "renderer/render/presentation_backend.h"
#include "renderer/renderer_api_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vr {

class FrameCaptureService;
class PresentationMetricsStore;
class D3D11Device;
class D3D11FramePresenter;
class D3D11HeadlessOutput;
class D3D11RenderBackend;
struct D3D11RenderResources;

struct RendererPresentationOverlayHooks {
    std::function<void(PresentationBackend&, const RendererDrawSnapshot&)> draw_overlay;
    std::function<bool(const RendererDrawSnapshot&, uint8_t*, int, int, size_t)>
        composite_bgra_overlay;
};

struct RendererPresentationDrawRequest {
    RendererPresentationDrawRequest(const RendererDrawSnapshot& draw_snapshot,
                                    PresentationMetricsStore& metrics_store)
        : snapshot(draw_snapshot),
          metrics(metrics_store) {}

    const RendererDrawSnapshot& snapshot;
    const char* source = nullptr;
    bool headless = false;
    bool publish_swap_chain_after_sync_draw = false;
    const char* wait_idle_after_sync_draw_label = nullptr;
    const char* poll_device_removed_label = nullptr;
    bool check_device_lost_after_draw = false;
    PresentationMetricsStore& metrics;
    RendererPresentationOverlayHooks overlay_hooks;
    std::function<bool()> should_abort_headless_publish;
    PresentationBackendAsyncDrawCompleted async_completion;
};

struct RendererPresentationDrawResult {
    bool drew = false;
    bool async_draw_submitted = false;
    bool device_lost = false;
    uint64_t backend_us = 0;
    RendererFrameCallback frame_callback;
    std::string failure_error;
};

struct RendererPresentationCompletionCallbacks {
    RendererFrameCallback frame_callback;
    std::function<void(const char*)> frame_failure_callback;
};

struct RendererPresentationAsyncCompletion {
    bool success = false;
    const char* error = nullptr;
    uint64_t backend_us = 0;
    const PresentationBackendFrameInfo* frame_info = nullptr;
    RendererPresentationCompletionCallbacks callbacks;
};

struct RendererPresentationSubmitRequest {
    RendererPresentationSubmitRequest(const RendererDrawSnapshot& draw_snapshot,
                                      PresentationMetricsStore& metrics_store)
        : snapshot(draw_snapshot),
          metrics(metrics_store) {}

    const RendererDrawSnapshot& snapshot;
    const char* source = nullptr;
    bool headless = false;
    bool publish_swap_chain_after_sync_draw = false;
    const char* wait_idle_after_sync_draw_label = nullptr;
    const char* poll_device_removed_label = nullptr;
    bool check_device_lost_after_draw = false;
    PresentationMetricsStore& metrics;
    RendererPresentationOverlayHooks overlay_hooks;
    std::function<bool()> should_abort_headless_publish;
    std::function<void(const RendererPresentationAsyncCompletion&)> async_completed;
};

struct RendererPresentationSubmitResult {
    RendererPresentationDrawResult draw;
    RendererPresentationCompletionCallbacks callbacks;
};

struct RendererPresentationSyncCompletion {
    RendererPresentationDrawResult draw;
    RendererPresentationCompletionCallbacks callbacks;
};

struct RendererPresentationSubmitDispatchHooks {
    std::function<void()> device_lost;
    std::function<void()> async_submitted;
    std::function<void(const RendererPresentationSyncCompletion&)> sync_completed;
};

struct RendererPresentationD3DMemorySnapshot {
    RendererGpuMemoryStats stats;
    std::array<uint64_t, kMaxTracks> presenter_copy_texture_bytes_by_slot{};
};

// Lock contract:
// - Owns the presentation backend, backend device mutex, backend texture
//   publication access, frame callback storage, and optional Windows capture
//   service.
// - May take backend texture locks only while holding device_mutex(), following
//   device -> texture order.
// - Does not take renderer state/lifecycle locks.
// - Does not invoke host callbacks while holding device/texture locks; callers
//   snapshot callbacks and invoke them after releasing renderer/backend locks.
class RendererPresentationController {
public:
    RendererPresentationController();
    ~RendererPresentationController();

    RendererPresentationController(const RendererPresentationController&) = delete;
    RendererPresentationController& operator=(const RendererPresentationController&) = delete;

    PresentationBackend* backend();
    const PresentationBackend* backend() const;
    bool has_backend() const;
    PresentationBackendKind backend_kind() const;
    void set_backend(std::unique_ptr<PresentationBackend> backend);
    std::unique_ptr<PresentationBackend> release_backend();
    void shutdown_backend();

    std::recursive_mutex& device_mutex() const;

    void set_frame_callback(RendererFrameCallback callback);
    void clear_frame_callback();
    RendererFrameCallback frame_callback_snapshot() const;

    void set_frame_failure_callback(std::function<void(const char*)> callback);
    std::function<void(const char*)> frame_failure_callback_snapshot() const;

    std::string backend_last_error() const;
    PresentationBackendStats backend_stats() const;
    bool copy_last_frame_info(PresentationBackendFrameInfo* out) const;
    bool poll_device_removed(const char* operation) const;
    bool device_lost() const;
    long device_removed_reason() const;
    void reset_track(size_t slot);
    void move_track(size_t from, size_t to);
    bool update_headless_output(void* output,
                                int width,
                                int height,
                                int max_track_slots);
    bool update_headless_output_ring(const void* const* pixel_buffers,
                                     size_t pixel_buffer_count,
                                     void* displayed_pixel_buffer,
                                     void* protected_pixel_buffer,
                                     int width,
                                     int height,
                                     int max_track_slots);
    void mark_headless_output_displayed(void* pixel_buffer);
    void protect_headless_output(void* pixel_buffer);
    void release_headless_output(void* pixel_buffer);
    void clear_headless_output();

    // Caller must hold device_mutex() when coordinating with surrounding
    // presentation work.
    void wait_gpu_idle(const char* label, PresentationMetricsStore& metrics);
    // Caller must hold device_mutex() when coordinating with surrounding
    // presentation work.
    bool draw_renderer_managed_headless_and_publish(
        const RendererDrawSnapshot& snapshot,
        const char* source,
        PresentationMetricsStore& metrics,
        RendererPresentationOverlayHooks overlay_hooks,
        const std::function<bool()>& should_abort,
        RendererFrameCallback& callback);
    bool draw_frame(const RendererDrawSnapshot& snapshot,
                    const char* source,
                    PresentationMetricsStore& metrics,
                    RendererPresentationOverlayHooks overlay_hooks = {},
                    PresentationBackendAsyncDrawCompleted async_completion = {});
    RendererPresentationDrawResult execute_draw(
        RendererPresentationDrawRequest request);
    RendererPresentationSubmitResult submit_draw(
        RendererPresentationSubmitRequest request);
    bool submit_and_dispatch(RendererPresentationSubmitRequest request,
                             RendererPresentationSubmitDispatchHooks hooks);
    bool capture_backend_front_buffer(std::vector<uint8_t>& bgra,
                                      int& width,
                                      int& height);
    RendererPresentationD3DMemorySnapshot d3d_memory_snapshot() const;
    bool resize_renderer_managed_headless_output(int width,
                                                 int height,
                                                 PresentationMetricsStore& metrics);
    void cleanup_renderer_managed_headless_pending_buffers();

#ifdef _WIN32
    bool set_d3d_headless_frame_callback(RendererFrameCallback callback);
    bool acquire_d3d_shared_texture(SharedTextureSnapshot& snapshot,
                                    PresentationMetricsStore& metrics) const;
    void release_d3d_shared_texture(int buffer_index,
                                    uint64_t buffer_generation) const;
    bool capture_d3d_headless_front_buffer(std::vector<uint8_t>& bgra,
                                           int& width,
                                           int& height) const;
    D3D11RenderBackend* d3d_backend() const;
    D3D11Device* d3d_device() const;
    D3D11FramePresenter* d3d_frame_presenter() const;
    D3D11HeadlessOutput* d3d_headless_output() const;
    D3D11RenderResources* d3d_resources() const;
#endif

private:
    std::unique_ptr<PresentationBackend> backend_;
    mutable std::recursive_mutex device_mutex_;
    mutable std::mutex callback_mutex_;
    RendererFrameCallback frame_callback_;
    std::function<void(const char*)> frame_failure_callback_;
#ifdef _WIN32
    std::unique_ptr<FrameCaptureService> frame_capture_;
#endif
};

} // namespace vr
