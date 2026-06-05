#include "renderer/render/renderer_presentation_controller.h"

#include "renderer/metrics/presentation_metrics_store.h"
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/render/swap_chain_present_policy.h"

#ifdef _WIN32
#include "windows/d3d11/frame_capture_service.h"
#include "windows/d3d11/frame_presenter.h"
#include "windows/d3d11/headless_output.h"
#include "windows/d3d11/render_backend.h"
#endif

#include <atomic>
#include <chrono>
#include <utility>

namespace vr {

namespace {

uint64_t presentation_elapsed_us_since(
    std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

} // namespace

RendererPresentationController::RendererPresentationController() {
#ifdef _WIN32
    frame_capture_ = std::make_unique<FrameCaptureService>();
#endif
}

RendererPresentationController::~RendererPresentationController() = default;

PresentationBackend* RendererPresentationController::backend() {
    return backend_.get();
}

const PresentationBackend* RendererPresentationController::backend() const {
    return backend_.get();
}

bool RendererPresentationController::has_backend() const {
    return backend_ != nullptr;
}

PresentationBackendKind RendererPresentationController::backend_kind() const {
    return backend_ ? backend_->kind() : PresentationBackendKind::Unknown;
}

void RendererPresentationController::set_backend(std::unique_ptr<PresentationBackend> backend) {
    backend_ = std::move(backend);
}

std::unique_ptr<PresentationBackend> RendererPresentationController::release_backend() {
    return std::move(backend_);
}

void RendererPresentationController::shutdown_backend() {
    if (backend_) {
        backend_->shutdown();
        backend_.reset();
    }
}

std::recursive_mutex& RendererPresentationController::device_mutex() const {
    return device_mutex_;
}

void RendererPresentationController::set_frame_callback(RendererFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(callback);
}

void RendererPresentationController::clear_frame_callback() {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = {};
}

RendererFrameCallback RendererPresentationController::frame_callback_snapshot() const {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return frame_callback_;
}

void RendererPresentationController::set_frame_failure_callback(
    std::function<void(const char*)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_failure_callback_ = std::move(callback);
}

std::function<void(const char*)>
RendererPresentationController::frame_failure_callback_snapshot() const {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    return frame_failure_callback_;
}

std::string RendererPresentationController::backend_last_error() const {
    if (!backend_) {
        return "presentation backend is not available";
    }
    const char* error = backend_->last_error();
    return error && error[0] != '\0' ? error : "presentation backend draw failed";
}

PresentationBackendStats RendererPresentationController::backend_stats() const {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ ? backend_->presentation_stats() : PresentationBackendStats{};
}

bool RendererPresentationController::copy_last_frame_info(
    PresentationBackendFrameInfo* out) const {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ && backend_->copy_last_frame_info(out);
}

bool RendererPresentationController::poll_device_removed(const char* operation) const {
    return backend_ && backend_->poll_device_removed(operation);
}

bool RendererPresentationController::device_lost() const {
    return backend_ && backend_->device_lost();
}

long RendererPresentationController::device_removed_reason() const {
    return backend_ ? backend_->device_removed_reason() : 0;
}

void RendererPresentationController::reset_track(size_t slot) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->reset_track(slot);
    }
}

void RendererPresentationController::move_track(size_t from, size_t to) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->move_track(from, to);
    }
}

bool RendererPresentationController::update_headless_output(
    void* output,
    int width,
    int height,
    int max_track_slots) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ &&
           backend_->update_headless_output(output, width, height, max_track_slots);
}

bool RendererPresentationController::update_headless_output_ring(
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int width,
    int height,
    int max_track_slots) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ &&
           backend_->update_headless_output_ring(pixel_buffers,
                                                 pixel_buffer_count,
                                                 displayed_pixel_buffer,
                                                 protected_pixel_buffer,
                                                 width,
                                                 height,
                                                 max_track_slots);
}

void RendererPresentationController::mark_headless_output_displayed(
    void* pixel_buffer) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->mark_headless_output_displayed(pixel_buffer);
    }
}

void RendererPresentationController::protect_headless_output(void* pixel_buffer) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->protect_headless_output(pixel_buffer);
    }
}

void RendererPresentationController::release_headless_output(void* pixel_buffer) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->release_headless_output(pixel_buffer);
    }
}

void RendererPresentationController::clear_headless_output() {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->clear_headless_output();
    }
}

void RendererPresentationController::wait_gpu_idle(
    const char* label,
    PresentationMetricsStore& metrics) {
    const auto start = std::chrono::steady_clock::now();
    if (backend_) {
        backend_->wait_idle(label);
    }
    metrics.render_wait_us.fetch_add(
        presentation_elapsed_us_since(start), std::memory_order_relaxed);
    metrics.render_wait_count.fetch_add(1, std::memory_order_relaxed);
}

bool RendererPresentationController::draw_renderer_managed_headless_and_publish(
    const RendererDrawSnapshot& snapshot,
    const char* source,
    PresentationMetricsStore& metrics,
    RendererPresentationOverlayHooks overlay_hooks,
    const std::function<bool()>& should_abort,
    RendererFrameCallback& callback) {
#ifdef _WIN32
    callback = {};
    if (should_abort && should_abort()) {
        return false;
    }
    auto* output = d3d_headless_output();
    auto* resources = d3d_resources();
    if (!output || !resources) {
        return false;
    }
    {
        std::lock_guard<std::mutex> tex_lock(output->texture_mutex());
        auto* rtv = output->begin_frame_locked();
        if (!rtv) {
            return false;
        }
        resources->cached_rtv = rtv;
    }
    if (!draw_frame(snapshot, source, metrics, std::move(overlay_hooks))) {
        return false;
    }
    const auto publish_start = std::chrono::steady_clock::now();
    output->wait_gpu_idle(source);
    {
        std::lock_guard<std::mutex> tex_lock(output->texture_mutex());
        auto published_callback = output->publish_frame_locked();
        callback = published_callback
            ? RendererFrameCallback(
                  [published_callback = std::move(published_callback)](
                      const PresentationBackendFrameInfo*) mutable {
                      published_callback();
                  })
            : RendererFrameCallback();
    }
    if (should_abort && should_abort()) {
        callback = {};
    }
    metrics.note_present_publish(presentation_elapsed_us_since(publish_start));
    return true;
#else
    (void)snapshot;
    (void)source;
    (void)metrics;
    (void)overlay_hooks;
    (void)should_abort;
    callback = {};
    return false;
#endif
}

bool RendererPresentationController::draw_frame(
    const RendererDrawSnapshot& snapshot,
    const char* source,
    PresentationMetricsStore& metrics,
    RendererPresentationOverlayHooks overlay_hooks,
    PresentationBackendAsyncDrawCompleted async_completion) {
    if (!backend_) {
        return false;
    }
    PresentationBackendDrawHooks hooks;
    hooks.draw_source = source;
    hooks.wait_gpu_idle = [this, &metrics](const char* label) {
        wait_gpu_idle(label, metrics);
    };
    hooks.record_frame_copy_us = [&metrics](uint64_t elapsed_us) {
        metrics.frame_copy_us.fetch_add(elapsed_us, std::memory_order_relaxed);
        metrics.frame_copy_count.fetch_add(1, std::memory_order_relaxed);
    };
    hooks.draw_overlay = std::move(overlay_hooks.draw_overlay);
    hooks.composite_bgra_overlay =
        std::move(overlay_hooks.composite_bgra_overlay);
    hooks.async_draw_completed = std::move(async_completion);
    return backend_->draw_frame(snapshot, hooks);
}

RendererPresentationDrawResult RendererPresentationController::execute_draw(
    RendererPresentationDrawRequest request) {
    RendererPresentationDrawResult result;
    const auto backend_start = std::chrono::steady_clock::now();
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    const bool async_backend =
        backend_ && backend_->completes_draw_asynchronously();
    auto async_completion =
        async_backend ? std::move(request.async_completion)
                      : PresentationBackendAsyncDrawCompleted();

    if (request.headless) {
        if (backend_ && backend_->renderer_manages_headless_publish() &&
            request.should_abort_headless_publish) {
            result.drew = draw_renderer_managed_headless_and_publish(
                request.snapshot,
                request.source,
                request.metrics,
                std::move(request.overlay_hooks),
                request.should_abort_headless_publish,
                result.frame_callback);
        } else {
            result.drew = draw_frame(request.snapshot,
                                     request.source,
                                     request.metrics,
                                     std::move(request.overlay_hooks),
                                     std::move(async_completion));
            result.async_draw_submitted = async_backend && result.drew;
            if (result.drew && !result.async_draw_submitted) {
                result.frame_callback = frame_callback_snapshot();
            }
        }
        result.device_lost =
            backend_ && request.poll_device_removed_label &&
            backend_->poll_device_removed(request.poll_device_removed_label);
    } else if (backend_) {
        result.drew = draw_frame(request.snapshot,
                                 request.source,
                                 request.metrics,
                                 std::move(request.overlay_hooks),
                                 std::move(async_completion));
        result.async_draw_submitted = async_backend && result.drew;
        if (should_present_swap_chain_after_draw(
                result.drew && !result.async_draw_submitted,
                request.publish_swap_chain_after_sync_draw &&
                    backend_->supports_swap_chain_present())) {
            const auto present_start = std::chrono::steady_clock::now();
            const bool presented = backend_->present_swap_chain(0);
            request.metrics.present_publish_us.fetch_add(
                presentation_elapsed_us_since(present_start),
                std::memory_order_relaxed);
            request.metrics.present_publish_count.fetch_add(
                1, std::memory_order_relaxed);
            result.device_lost = !presented && backend_->device_lost();
        } else {
            if (!result.async_draw_submitted &&
                request.wait_idle_after_sync_draw_label) {
                backend_->wait_idle(request.wait_idle_after_sync_draw_label);
            }
            if (request.poll_device_removed_label) {
                result.device_lost =
                    backend_->poll_device_removed(request.poll_device_removed_label);
            } else if (request.check_device_lost_after_draw) {
                result.device_lost = backend_->device_lost();
            }
        }
    }

    if (!result.drew) {
        result.failure_error = backend_last_error();
    }
    result.backend_us = presentation_elapsed_us_since(backend_start);
    return result;
}

RendererPresentationSubmitResult RendererPresentationController::submit_draw(
    RendererPresentationSubmitRequest request) {
    RendererPresentationSubmitResult result;
    result.callbacks.frame_callback = frame_callback_snapshot();
    result.callbacks.frame_failure_callback = frame_failure_callback_snapshot();

    RendererPresentationDrawRequest draw_request(request.snapshot, request.metrics);
    draw_request.source = request.source;
    draw_request.headless = request.headless;
    draw_request.publish_swap_chain_after_sync_draw =
        request.publish_swap_chain_after_sync_draw;
    draw_request.wait_idle_after_sync_draw_label =
        request.wait_idle_after_sync_draw_label;
    draw_request.poll_device_removed_label = request.poll_device_removed_label;
    draw_request.check_device_lost_after_draw = request.check_device_lost_after_draw;
    draw_request.overlay_hooks = std::move(request.overlay_hooks);
    draw_request.should_abort_headless_publish =
        std::move(request.should_abort_headless_publish);
    if (request.async_completed) {
        auto callbacks = result.callbacks;
        auto async_completed = std::move(request.async_completed);
        draw_request.async_completion = PresentationBackendAsyncDrawCompleted(
            [callbacks = std::move(callbacks),
             async_completed = std::move(async_completed)](
                bool success,
                const char* error,
                uint64_t completion_backend_us,
                const PresentationBackendFrameInfo* frame_info) mutable {
                RendererPresentationAsyncCompletion completion;
                completion.success = success;
                completion.error = error;
                completion.backend_us = completion_backend_us;
                completion.frame_info = frame_info;
                completion.callbacks = callbacks;
                async_completed(completion);
            });
    }

    result.draw = execute_draw(std::move(draw_request));
    return result;
}

bool RendererPresentationController::submit_and_dispatch(
    RendererPresentationSubmitRequest request,
    RendererPresentationSubmitDispatchHooks hooks) {
    auto submit_result = submit_draw(std::move(request));
    if (submit_result.draw.device_lost) {
        if (hooks.device_lost) {
            hooks.device_lost();
        }
        return false;
    }
    if (submit_result.draw.async_draw_submitted) {
        if (hooks.async_submitted) {
            hooks.async_submitted();
        }
        return submit_result.draw.drew;
    }

    const bool drew = submit_result.draw.drew;
    if (hooks.sync_completed) {
        RendererPresentationSyncCompletion completion{
            std::move(submit_result.draw),
            std::move(submit_result.callbacks),
        };
        hooks.sync_completed(completion);
    }
    return drew;
}

bool RendererPresentationController::capture_backend_front_buffer(
    std::vector<uint8_t>& bgra,
    int& width,
    int& height) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_ && backend_->capture_front_buffer(bgra, width, height)) {
        return true;
    }
    bgra.clear();
    width = 0;
    height = 0;
    return false;
}

RendererPresentationD3DMemorySnapshot
RendererPresentationController::d3d_memory_snapshot() const {
    RendererPresentationD3DMemorySnapshot result;
#ifdef _WIN32
    std::lock_guard<std::recursive_mutex> device_lock(device_mutex_);
    if (auto* presenter = d3d_frame_presenter()) {
        const auto presenter_stats = presenter->memory_stats();
        result.stats.presenter_texture_bytes =
            presenter_stats.total_estimated_bytes;
        result.stats.total_estimated_bytes +=
            result.stats.presenter_texture_bytes;
        for (size_t i = 0;
             i < kMaxTracks && i < presenter_stats.slots.size();
             ++i) {
            result.presenter_copy_texture_bytes_by_slot[i] =
                presenter_stats.slots[i].render_nv12_copy_texture_bytes;
        }
    }

    if (auto* output = d3d_headless_output()) {
        const auto headless_stats = output->memory_stats();
        result.stats.headless_output_bytes = headless_stats.estimated_bytes;
        result.stats.headless_width = headless_stats.width;
        result.stats.headless_height = headless_stats.height;
        result.stats.headless_buffer_count = headless_stats.buffer_count;
        result.stats.total_estimated_bytes +=
            result.stats.headless_output_bytes;
    }

    if (auto* resources = d3d_resources()) {
        const auto overlay_stats =
            snapshot_analysis_overlay_memory_stats(*resources);
        result.stats.analysis_overlay_bytes = overlay_stats.estimated_bytes;
        result.stats.analysis_overlay_width = overlay_stats.width;
        result.stats.analysis_overlay_height = overlay_stats.height;
        if (result.stats.analysis_overlay_bytes > 0) {
            result.stats.total_estimated_bytes +=
                result.stats.analysis_overlay_bytes;
        }
    }
#endif
    return result;
}

bool RendererPresentationController::resize_renderer_managed_headless_output(
    int width,
    int height,
    PresentationMetricsStore& metrics) {
#ifdef _WIN32
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    auto* output = d3d_headless_output();
    if (!output) {
        return false;
    }
    {
        std::lock_guard<std::mutex> tex_lock(output->texture_mutex());
        if (!output->resize_locked(width, height)) {
            return false;
        }
    }
    metrics.note_shared_texture_resize();
    return true;
#else
    (void)width;
    (void)height;
    (void)metrics;
    return false;
#endif
}

void RendererPresentationController::cleanup_renderer_managed_headless_pending_buffers() {
#ifdef _WIN32
    if (auto* output = d3d_headless_output()) {
        output->cleanup_expired_pending_buffers();
    }
#endif
}

#ifdef _WIN32
bool RendererPresentationController::set_d3d_headless_frame_callback(
    RendererFrameCallback callback) {
    auto* output = d3d_headless_output();
    if (!output) {
        return false;
    }
    output->set_frame_callback([callback = std::move(callback)]() {
        if (callback) {
            callback(nullptr);
        }
    });
    clear_frame_callback();
    return true;
}

bool RendererPresentationController::acquire_d3d_shared_texture(
    SharedTextureSnapshot& snapshot,
    PresentationMetricsStore& metrics) const {
    snapshot = {};
    auto* output = d3d_headless_output();
    if (!output) {
        metrics.note_texture_sharing_failure();
        return false;
    }

    std::lock_guard<std::mutex> lock(output->texture_mutex());
    D3D11HeadlessOutputTextureLease lease;
    if (!output->acquire_shared_texture_locked(lease)) {
        metrics.note_texture_sharing_failure();
        return false;
    }

    snapshot.type = SharedTextureHandleType::D3D11SharedHandle;
    snapshot.texture = lease.texture;
    snapshot.handle = lease.handle;
    snapshot.width = lease.width;
    snapshot.height = lease.height;
    snapshot.buffer_index = lease.buffer_index;
    snapshot.buffer_generation = lease.generation;
    return true;
}

void RendererPresentationController::release_d3d_shared_texture(
    int buffer_index,
    uint64_t buffer_generation) const {
    if (auto* output = d3d_headless_output()) {
        output->release_shared_texture(buffer_index, buffer_generation);
    }
}

bool RendererPresentationController::capture_d3d_headless_front_buffer(
    std::vector<uint8_t>& bgra,
    int& width,
    int& height) const {
    auto* output = d3d_headless_output();
    if (!frame_capture_ || !output) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return frame_capture_->capture_headless_front_buffer(
        *output, device_mutex_, bgra, width, height);
}

D3D11RenderBackend* RendererPresentationController::d3d_backend() const {
    if (!backend_ || backend_->kind() != PresentationBackendKind::D3D11) {
        return nullptr;
    }
    return static_cast<D3D11RenderBackend*>(backend_.get());
}

D3D11Device* RendererPresentationController::d3d_device() const {
    auto* backend = d3d_backend();
    return backend ? backend->device() : nullptr;
}

D3D11FramePresenter* RendererPresentationController::d3d_frame_presenter() const {
    auto* backend = d3d_backend();
    return backend ? backend->frame_presenter() : nullptr;
}

D3D11HeadlessOutput* RendererPresentationController::d3d_headless_output() const {
    auto* backend = d3d_backend();
    return backend ? backend->headless_output() : nullptr;
}

D3D11RenderResources* RendererPresentationController::d3d_resources() const {
    auto* backend = d3d_backend();
    return backend ? backend->resources() : nullptr;
}
#endif

} // namespace vr
