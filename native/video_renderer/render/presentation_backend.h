#pragma once

#include "video_renderer/render/backend_type.h"
#include "video_renderer/render/renderer_draw_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace vr {

using PresentationBackendKind = RenderBackendKind;
class PresentationBackend;

struct PresentationBackendConfig {
    void* hwnd = nullptr;
    void* adapter = nullptr;
    void* output = nullptr;
    int width = 0;
    int height = 0;
    int max_track_slots = 0;
    bool headless = false;
};

struct PresentationBackendDrawHooks {
    std::function<void(const char*)> wait_gpu_idle;
    std::function<void(uint64_t)> record_frame_copy_us;
    std::function<void(PresentationBackend&, const RendererDrawSnapshot&)> draw_overlay;
};

class PresentationBackend {
public:
    virtual ~PresentationBackend() = default;

    virtual PresentationBackendKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual bool initialize(const PresentationBackendConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual bool headless() const = 0;
    virtual bool renderer_manages_headless_publish() const { return false; }
    virtual bool supports_swap_chain_present() const { return false; }
    virtual bool poll_device_removed(const char*) { return false; }
    virtual bool device_lost() const { return false; }
    virtual long device_removed_reason() const { return 0; }
    virtual void wait_idle(const char*) {}
    virtual bool present_swap_chain(int) { return false; }
    virtual void reset_track(size_t) {}
    virtual void move_track(size_t, size_t) {}
    virtual bool draw_frame(const RendererDrawSnapshot& snapshot,
                            const PresentationBackendDrawHooks& hooks) = 0;
};

} // namespace vr
