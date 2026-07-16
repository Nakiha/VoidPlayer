#include "renderer/render/renderer_presentation_controller.h"

#include "renderer/metrics/presentation_metrics_store.h"
#include "renderer/render/swap_chain_present_policy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <thread>
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

struct AsyncSubmissionGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool opened = false;
};

RendererPresentationController::RendererPresentationController() = default;

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

PresentationBackendDiagnostics
RendererPresentationController::backend_diagnostics() const {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ ? backend_->diagnostics() : PresentationBackendDiagnostics{};
}

bool RendererPresentationController::copy_last_frame_info(
    PresentationBackendFrameInfo* out) const {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ && backend_->copy_last_frame_info(out);
}

void* RendererPresentationController::native_render_device() const {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ ? backend_->native_render_device() : nullptr;
}

void* RendererPresentationController::native_render_command_queue() const {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ ? backend_->native_render_command_queue() : nullptr;
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

bool RendererPresentationController::update_offscreen_target(
    void* output,
    int width,
    int height,
    int max_track_slots) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ &&
           backend_->update_offscreen_target(output, width, height, max_track_slots);
}

bool RendererPresentationController::update_offscreen_target_ring(
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int width,
    int height,
    int max_track_slots) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    return backend_ &&
           backend_->update_offscreen_target_ring(pixel_buffers,
                                                 pixel_buffer_count,
                                                 displayed_pixel_buffer,
                                                 protected_pixel_buffer,
                                                 width,
                                                 height,
                                                 max_track_slots);
}

void RendererPresentationController::mark_offscreen_target_displayed(
    void* pixel_buffer) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->mark_offscreen_target_displayed(pixel_buffer);
    }
}

void RendererPresentationController::protect_offscreen_target(void* pixel_buffer) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->protect_offscreen_target(pixel_buffer);
    }
}

void RendererPresentationController::release_offscreen_target(void* pixel_buffer) {
    pending_offscreen_target_releases_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->release_offscreen_target(pixel_buffer);
    }
    pending_offscreen_target_releases_.fetch_sub(1, std::memory_order_acq_rel);
}

void RendererPresentationController::clear_offscreen_target() {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_) {
        backend_->clear_offscreen_target();
    }
}

bool RendererPresentationController::update_sdr_white_level(double nits) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex());
    return backend_ && backend_->update_sdr_white_level(nits);
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

bool RendererPresentationController::draw_renderer_managed_offscreen_and_publish(
    const RendererDrawSnapshot& snapshot,
    const char* source,
    PresentationMetricsStore& metrics,
    RendererPresentationOverlayHooks overlay_hooks,
    const std::function<bool()>& should_abort,
    RendererFrameCallback& callback) {
    callback = {};
    if (should_abort && should_abort()) {
        return false;
    }
    if (!backend_ || !backend_->begin_renderer_managed_offscreen_frame()) {
        return false;
    }
    if (!draw_frame(snapshot, source, metrics, std::move(overlay_hooks))) {
        return false;
    }
    const auto publish_start = std::chrono::steady_clock::now();
    auto published_callback =
        backend_->publish_renderer_managed_offscreen_frame(source);
    callback = published_callback
        ? RendererFrameCallback(
              [published_callback = std::move(published_callback)](
                  const PresentationBackendFrameInfo*) mutable {
                  published_callback();
              })
        : RendererFrameCallback();
    if (should_abort && should_abort()) {
        callback = {};
    }
    metrics.note_present_publish(presentation_elapsed_us_since(publish_start));
    return true;
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
    hooks.build_overlay_primitives =
        std::move(overlay_hooks.build_overlay_primitives);
    hooks.async_draw_completed = std::move(async_completion);
    return backend_->draw_frame(snapshot, hooks);
}

RendererPresentationDrawResult RendererPresentationController::execute_draw(
    RendererPresentationDrawRequest request) {
    RendererPresentationDrawResult result;
    const auto backend_start = std::chrono::steady_clock::now();
    // Completed-target releases are tiny but latency-sensitive. Once a host
    // callback is waiting to return a ring slot, do not let a continuous stream
    // of 120Hz interaction/source draws repeatedly win the backend mutex.
    while (pending_offscreen_target_releases_.load(
               std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    const bool async_backend =
        backend_ && backend_->completes_draw_asynchronously();
    auto async_completion =
        async_backend ? std::move(request.async_completion)
                      : PresentationBackendAsyncDrawCompleted();

    if (request.offscreen) {
        if (backend_ && backend_->renderer_manages_offscreen_publish() &&
            request.should_abort_offscreen_publish) {
            result.drew = draw_renderer_managed_offscreen_and_publish(
                request.snapshot,
                request.source,
                request.metrics,
                std::move(request.overlay_hooks),
                request.should_abort_offscreen_publish,
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
    if (backend_) {
        if (result.drew && !result.async_draw_submitted) {
            result.backend_blocking_wait_us =
                backend_->last_draw_blocking_wait_us();
            result.frame_info_available =
                backend_->copy_last_frame_info(&result.frame_info);
        }
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
    draw_request.offscreen = request.offscreen;
    draw_request.publish_swap_chain_after_sync_draw =
        request.publish_swap_chain_after_sync_draw;
    draw_request.wait_idle_after_sync_draw_label =
        request.wait_idle_after_sync_draw_label;
    draw_request.poll_device_removed_label = request.poll_device_removed_label;
    draw_request.check_device_lost_after_draw = request.check_device_lost_after_draw;
    draw_request.overlay_hooks = std::move(request.overlay_hooks);
    draw_request.should_abort_offscreen_publish =
        std::move(request.should_abort_offscreen_publish);
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
    std::shared_ptr<AsyncSubmissionGate> async_gate;
    if (request.async_completed) {
        async_gate = std::make_shared<AsyncSubmissionGate>();
        auto async_completed = std::move(request.async_completed);
        request.async_completed =
            [async_gate, async_completed = std::move(async_completed)](
                const RendererPresentationAsyncCompletion& completion) mutable {
                {
                    std::unique_lock<std::mutex> lock(async_gate->mutex);
                    async_gate->cv.wait(lock, [&] { return async_gate->opened; });
                }
                async_completed(completion);
            };
    }
    const auto open_async_gate = [&]() {
        if (!async_gate) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(async_gate->mutex);
            async_gate->opened = true;
        }
        async_gate->cv.notify_all();
    };
    auto submit_result = submit_draw(std::move(request));
    if (submit_result.draw.async_draw_submitted) {
        if (hooks.async_submitted) {
            hooks.async_submitted();
        }
        open_async_gate();
        if (submit_result.draw.device_lost) {
            if (hooks.device_lost) {
                hooks.device_lost();
            }
            return false;
        }
        return submit_result.draw.drew;
    }
    open_async_gate();
    if (submit_result.draw.device_lost) {
        if (hooks.device_lost) {
            hooks.device_lost();
        }
        return false;
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

bool RendererPresentationController::capture_backend_front_buffer_region(
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& region_width,
    int& region_height) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    if (backend_ && backend_->capture_front_buffer_region(
                        x,
                        y,
                        width,
                        height,
                        bgra,
                        region_width,
                        region_height)) {
        return true;
    }
    bgra.clear();
    region_width = 0;
    region_height = 0;
    return false;
}

RendererPresentationMemorySnapshot
RendererPresentationController::memory_snapshot() const {
    return {};
}

bool RendererPresentationController::resize_renderer_managed_offscreen_target(
    int width,
    int height,
    PresentationMetricsStore& metrics) {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    if (!backend_ ||
        !backend_->resize_renderer_managed_offscreen_target(width, height)) {
        return false;
    }
    metrics.note_presentation_target_resize();
    return true;
}

bool RendererPresentationController::prewarm_renderer_managed_offscreen_target(
    int width,
    int height) {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    return backend_ &&
           backend_->prewarm_renderer_managed_offscreen_target(width, height);
}

void RendererPresentationController::cleanup_renderer_managed_offscreen_pending_buffers() {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    if (backend_) {
        backend_->cleanup_renderer_managed_offscreen_pending_buffers();
    }
}

bool RendererPresentationController::set_renderer_managed_offscreen_frame_callback(
    RendererFrameCallback callback) {
    if (!backend_ ||
        !backend_->set_renderer_managed_offscreen_frame_callback(
            [callback = std::move(callback)]() {
                if (callback) {
                    callback(nullptr);
                }
            })) {
        return false;
    }
    clear_frame_callback();
    return true;
}

bool RendererPresentationController::recover_device_loss(
    const char* reason,
    long removed_reason) {
    std::lock_guard<std::recursive_mutex> lock(device_mutex_);
    (void)reason;
    (void)removed_reason;
    return false;
}

} // namespace vr
