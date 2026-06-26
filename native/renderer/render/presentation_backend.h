#pragma once

#include "renderer/render/presentation_backend_types.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace vr {

class PresentationBackend;
struct SourceCacheTrackDescriptor;
struct SharedSourceCacheBundleSnapshot;
struct AnalysisOverlayPrimitivePackage;
struct SharedFp16TextureSnapshot;
struct PresentationExternalD3D12Surface;
struct WindowsSourceProjection;

using PresentationBackendAsyncDrawCompleted =
    std::function<void(bool, const char*, uint64_t, const PresentationBackendFrameInfo*)>;

struct PresentationBackendDrawHooks {
    const char* draw_source = nullptr;
    bool suppress_analysis_overlay = false;
    std::function<void(const char*)> wait_gpu_idle;
    std::function<void(uint64_t)> record_frame_copy_us;
    std::function<void(PresentationBackend&, const RendererDrawSnapshot&)> draw_overlay;
    std::function<bool(const RendererDrawSnapshot&, uint8_t*, int, int, size_t)> composite_bgra_overlay;
    std::function<std::shared_ptr<const AnalysisOverlayPrimitivePackage>(
        const RendererDrawSnapshot&)> build_overlay_primitives;
    PresentationBackendAsyncDrawCompleted async_draw_completed;
};

// Async draw contract:
// - Backends that return completes_draw_asynchronously() must invoke
//   async_draw_completed at most once for each accepted draw.
// - shutdown() must drain, cancel, or otherwise make pending async completions
//   unable to outlive renderer-owned completion state captured by callers.
class PresentationBackend {
public:
    virtual ~PresentationBackend() = default;

    virtual PresentationBackendKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual bool initialize(const PresentationBackendConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual bool headless() const = 0;
    virtual bool renderer_manages_headless_publish() const { return false; }
    virtual bool begin_renderer_managed_headless_frame() { return true; }
    virtual std::function<void()> publish_renderer_managed_headless_frame(const char*) {
        return {};
    }
    virtual bool resize_renderer_managed_headless_output(int, int) { return false; }
    virtual bool prewarm_renderer_managed_headless_output(int, int) { return false; }
    virtual void cleanup_renderer_managed_headless_pending_buffers() {}
    virtual bool set_renderer_managed_headless_frame_callback(std::function<void()>) {
        return false;
    }
    virtual bool completes_draw_asynchronously() const { return false; }
    virtual bool supports_swap_chain_present() const { return false; }
    virtual bool poll_device_removed(const char*) { return false; }
    virtual bool device_lost() const { return false; }
    virtual long device_removed_reason() const { return 0; }
    virtual void wait_idle(const char*) {}
    virtual bool present_swap_chain(int) { return false; }
    virtual void reset_track(size_t) {}
    virtual void move_track(size_t, size_t) {}
    virtual bool update_headless_output(void*, int, int, int) { return false; }
    virtual bool update_headless_output_ring(const void* const*,
                                             size_t,
                                             void*,
                                             void*,
                                             int,
                                             int,
                                             int) { return false; }
    virtual void mark_headless_output_displayed(void*) {}
    virtual void protect_headless_output(void*) {}
    virtual void release_headless_output(void*) {}
    virtual void clear_headless_output() {}
    virtual bool update_sdr_white_level(double) { return false; }
    virtual void* native_render_device() const { return nullptr; }
#ifdef _WIN32
    virtual bool acquire_shared_fp16_texture(SharedFp16TextureSnapshot&) {
        return false;
    }
    virtual void release_shared_fp16_texture(int, uint64_t) {}
    virtual void set_shared_fp16_frame_callback(std::function<void()>) {}
    virtual bool update_external_flutter_surface(
        const PresentationExternalD3D12Surface&) {
        return false;
    }
    virtual void clear_external_flutter_surface() {}
#endif
    virtual PresentationBackendStats presentation_stats() const { return {}; }
    virtual PresentationBackendDiagnostics diagnostics() const { return {}; }
    virtual bool copy_last_frame_info(PresentationBackendFrameInfo*) const { return false; }
    virtual bool capture_front_buffer(std::vector<uint8_t>&, int&, int&) { return false; }
    virtual bool capture_front_buffer_region(int,
                                             int,
                                             int,
                                             int,
                                             std::vector<uint8_t>&,
                                             int&,
                                             int&) { return false; }
#ifdef _WIN32
    virtual bool configure_source_cache(
        const std::vector<SourceCacheTrackDescriptor>&) { return false; }
    virtual void clear_source_cache(const char*) {}
    virtual bool update_source_projection(const WindowsSourceProjection&) {
        return false;
    }
    virtual void clear_source_projection() {}
    virtual bool acquire_source_cache_bundle(
        SharedSourceCacheBundleSnapshot&) { return false; }
    virtual void release_source_cache_bundle(int, uint64_t) {}
    virtual void set_source_cache_frame_callback(std::function<void()>) {}
#endif
    virtual const char* last_error() const { return ""; }
    virtual bool draw_frame(const RendererDrawSnapshot& snapshot,
                            const PresentationBackendDrawHooks& hooks) = 0;
};

} // namespace vr
