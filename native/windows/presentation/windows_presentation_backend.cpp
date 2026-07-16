#include "windows/presentation/windows_presentation_backend.h"

#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/render/presentation_snapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

#include <spdlog/spdlog.h>

namespace vr {
namespace {

DXGI_FORMAT target_format_for(ColorOutputTarget output_target) {
  return output_target == ColorOutputTarget::kWindowsLinearScRGB
      ? DXGI_FORMAT_R16G16B16A16_FLOAT
      : DXGI_FORMAT_B8G8R8A8_UNORM;
}

const char* target_format_name(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "rgba16f" : "bgra8";
}

class ScopedD3D11ContextLock final {
 public:
  explicit ScopedD3D11ContextLock(ID3D10Multithread* multithread)
      : multithread_(multithread) {
    if (multithread_) {
      multithread_->Enter();
    }
  }
  ~ScopedD3D11ContextLock() {
    if (multithread_) {
      multithread_->Leave();
    }
  }

 private:
  ID3D10Multithread* multithread_ = nullptr;
};

}  // namespace

WindowsD3D11PresentationBackend::~WindowsD3D11PresentationBackend() {
  shutdown();
}

bool WindowsD3D11PresentationBackend::initialize(
    const PresentationBackendConfig& config) {
  shutdown();
  if (!config.offscreen || !config.output || config.width <= 0 ||
      config.height <= 0) {
    set_error("Windows D3D11 backend requires an offscreen target ring");
    return false;
  }
  output_target_ = config.output_target;
  sdr_white_level_nits_ = config.sdr_white_level_nits;
  width_ = config.width;
  height_ = config.height;
  max_track_slots_ = std::clamp(config.max_track_slots, 1, 4);

  const auto* install =
      static_cast<const WindowsD3D11TargetRingInstall*>(config.output);
  if (install->width != config.width || install->height != config.height) {
    set_error("Windows D3D11 target ring does not match renderer dimensions");
    return false;
  }
  if (!install_ring(*install)) {
    return false;
  }
  device_ = target_ring_.device();
  if (!device_) {
    set_error("Windows D3D11 target ring has no device");
    return false;
  }
  device_->GetImmediateContext(&context_);
  if (!context_) {
    set_error("Windows D3D11 target device has no immediate context");
    return false;
  }
  if (SUCCEEDED(device_.As(&multithread_)) && multithread_) {
    multithread_->SetMultithreadProtected(TRUE);
  }
  D3D11_QUERY_DESC query_desc = {};
  query_desc.Query = D3D11_QUERY_EVENT;
  const HRESULT query_result =
      device_->CreateQuery(&query_desc, &completion_query_);
  if (FAILED(query_result) || !completion_query_) {
    record_device_error(query_result, "CreateQuery");
    set_error("Windows D3D11 backend could not create a completion query");
    return false;
  }
  if (!viewport_renderer_.initialize(device_.Get(), context_.Get())) {
    set_error("Windows D3D11 viewport initialization failed: " +
              viewport_renderer_.last_error());
    return false;
  }
  initialized_ = true;
  set_error("");
  return true;
}

void WindowsD3D11PresentationBackend::shutdown() {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  if (context_) {
    context_->Flush();
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  viewport_renderer_.shutdown();
  target_ring_.clear();
  last_completed_target_.Reset();
  capture_staging_.Reset();
  completion_query_.Reset();
  multithread_.Reset();
  context_.Reset();
  device_.Reset();
  last_frame_info_ = {};
  last_frame_info_available_ = false;
  initialized_ = false;
  device_lost_ = false;
  device_removed_reason_ = S_OK;
  width_ = 0;
  height_ = 0;
  max_track_slots_ = 0;
  capture_staging_width_ = 0;
  capture_staging_height_ = 0;
  capture_staging_format_ = DXGI_FORMAT_UNKNOWN;
  capture_staging_allocation_count_ = 0;
  capture_staging_reuse_count_ = 0;
  capture_staging_max_bytes_ = 0;
  overlay_last_expected_ = false;
  overlay_last_applied_ = false;
  overlay_last_fill_rect_count_ = 0;
  overlay_last_line_rect_count_ = 0;
  overlay_expected_count_ = 0;
  overlay_applied_count_ = 0;
  overlay_missed_count_ = 0;
  overlay_gpu_failure_count_ = 0;
  overlay_diagnostic_count_ = 0;
}

bool WindowsD3D11PresentationBackend::poll_device_removed(
    const char* operation) {
  if (!device_) {
    return device_lost();
  }
  const HRESULT reason = device_->GetDeviceRemovedReason();
  if (FAILED(reason)) {
    record_device_error(reason, operation ? operation : "poll");
  }
  return device_lost();
}

bool WindowsD3D11PresentationBackend::device_lost() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return device_lost_;
}

long WindowsD3D11PresentationBackend::device_removed_reason() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return static_cast<long>(device_removed_reason_);
}

void WindowsD3D11PresentationBackend::wait_idle(const char* label) {
  (void)wait_for_gpu(label ? label : "wait_idle");
}

bool WindowsD3D11PresentationBackend::update_offscreen_target_ring(
    const void* const* textures,
    size_t texture_count,
    void* displayed_texture,
    void* protected_texture,
    int width,
    int height,
    int max_track_slots) {
  if (!textures || texture_count == 0 || !textures[0]) {
    set_error("Windows D3D11 target ring is empty");
    return false;
  }
  D3D11_TEXTURE2D_DESC first_desc = {};
  static_cast<ID3D11Texture2D*>(const_cast<void*>(textures[0]))
      ->GetDesc(&first_desc);
  ColorOutputTarget next_output_target;
  if (first_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    next_output_target = ColorOutputTarget::kWindowsLinearScRGB;
  } else if (first_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
    next_output_target = ColorOutputTarget::kSDRToneMappedBT709;
  } else {
    set_error("Windows D3D11 target ring format is unsupported");
    return false;
  }
  WindowsD3D11TargetRingInstall install;
  install.textures = textures;
  install.texture_count = texture_count;
  install.displayed_texture = displayed_texture;
  install.protected_texture = protected_texture;
  install.width = width;
  install.height = height;
  install.format = first_desc.Format;
  ColorOutputTarget previous_output_target;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    previous_output_target = output_target_;
    output_target_ = next_output_target;
  }
  if (!install_ring(install)) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    output_target_ = previous_output_target;
    return false;
  }
  auto next_device = target_ring_.device();
  if (!device_ || next_device.Get() != device_.Get()) {
    set_error("Windows D3D11 target-ring device cannot change in place");
    target_ring_.clear();
    std::lock_guard<std::mutex> lock(state_mutex_);
    output_target_ = previous_output_target;
    return false;
  }
  width_ = width;
  height_ = height;
  max_track_slots_ = std::clamp(max_track_slots, 1, 4);
  spdlog::info(
      "[WindowsPresentation] target ring reconfigured format={} output={}",
      target_format_name(first_desc.Format),
      next_output_target == ColorOutputTarget::kWindowsLinearScRGB
          ? "linear-scrgb"
          : "sdr-bt709");
  return true;
}

void WindowsD3D11PresentationBackend::mark_offscreen_target_displayed(
    void* texture) {
  target_ring_.mark_displayed(static_cast<ID3D11Texture2D*>(texture));
}

void WindowsD3D11PresentationBackend::protect_offscreen_target(void* texture) {
  target_ring_.protect(static_cast<ID3D11Texture2D*>(texture));
}

void WindowsD3D11PresentationBackend::release_offscreen_target(void* texture) {
  target_ring_.release(static_cast<ID3D11Texture2D*>(texture));
}

void WindowsD3D11PresentationBackend::clear_offscreen_target() {
  target_ring_.clear();
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_completed_target_.Reset();
  last_frame_info_available_ = false;
}

bool WindowsD3D11PresentationBackend::update_sdr_white_level(double nits) {
  if (!std::isfinite(nits) || nits <= 0.0 || nits > 10000.0) {
    set_error("Windows D3D11 SDR white level is invalid");
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  sdr_white_level_nits_ = nits;
  return true;
}

PresentationBackendStats
WindowsD3D11PresentationBackend::presentation_stats() const {
  PresentationBackendStats stats;
  const auto ring = target_ring_.diagnostics();
  std::lock_guard<std::mutex> lock(state_mutex_);
  stats.backend_available = initialized_ && !device_lost_ ? 1 : 0;
  stats.target_installed = ring.target_count >=
          WindowsD3D11TargetRing::kMinTargetCount
      ? 1
      : 0;
  stats.last_draw_succeeded = last_frame_info_available_ ? 1 : 0;
  stats.draw_failure_count = draw_failure_count_;
  stats.consecutive_draw_failures = consecutive_draw_failures_;
  stats.last_successful_frame_pts_us = last_frame_info_.pts_us;
  stats.staging_allocation_count = capture_staging_allocation_count_;
  stats.staging_reuse_count = capture_staging_reuse_count_;
  stats.staging_max_bytes = capture_staging_max_bytes_;
  stats.viewport_composite_count = draw_count_;
  const auto viewport = viewport_renderer_.stats();
  stats.video_source_update_count = viewport.video_source_update_count;
  stats.source_frame_cache_hit_count =
      viewport.source_frame_cache_hit_count;
  stats.source_frame_cache_miss_count =
      viewport.source_frame_cache_miss_count;
  stats.overlay_last_expected = overlay_last_expected_ ? 1 : 0;
  stats.overlay_last_applied = overlay_last_applied_ ? 1 : 0;
  stats.overlay_last_fill_rect_count = overlay_last_fill_rect_count_;
  stats.overlay_last_line_rect_count = overlay_last_line_rect_count_;
  stats.overlay_expected_count = overlay_expected_count_;
  stats.overlay_applied_count = overlay_applied_count_;
  stats.overlay_missed_count = overlay_missed_count_;
  stats.overlay_gpu_success_count = overlay_applied_count_;
  stats.overlay_gpu_failure_count = overlay_gpu_failure_count_;
  stats.overlay_source_cache_hit_count =
      viewport.overlay_source_cache_hit_count;
  stats.overlay_source_cache_miss_count =
      viewport.overlay_source_cache_miss_count;
  stats.overlay_gpu_upload_count = viewport.overlay_gpu_upload_count;
  stats.overlay_gpu_buffer_reuse_count =
      viewport.overlay_gpu_buffer_reuse_count;
  stats.overlay_gpu_upload_bytes = viewport.overlay_gpu_upload_bytes;
  stats.overlay_last_source_generation =
      viewport.overlay_last_source_generation;
  stats.overlay_last_lookup_us = viewport.overlay_last_lookup_us;
  stats.overlay_last_upload_us = viewport.overlay_last_upload_us;
  stats.overlay_last_cpu_submit_us = viewport.overlay_last_cpu_submit_us;
  stats.overlay_max_cpu_submit_us = viewport.overlay_max_cpu_submit_us;
  return stats;
}

PresentationBackendDiagnostics
WindowsD3D11PresentationBackend::diagnostics() const {
  PresentationBackendDiagnostics diagnostics;
  const auto ring = target_ring_.diagnostics();
  bool initialized = false;
  ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    initialized = initialized_;
    output_target = output_target_;
  }
  diagnostics.backend = name();
  diagnostics.target_format = target_format_name(ring.format);
  diagnostics.render_target_format = diagnostics.target_format;
  diagnostics.render_color_space =
      output_target == ColorOutputTarget::kWindowsLinearScRGB
      ? "linear-scrgb"
      : "sdr-bt709";
  diagnostics.fallback_reason = initialized ? "none" : "backend-unavailable";
  diagnostics.width = ring.width;
  diagnostics.height = ring.height;
  diagnostics.buffer_count = static_cast<int32_t>(ring.target_count);
  diagnostics.offscreen = true;
  return diagnostics;
}

uint64_t WindowsD3D11PresentationBackend::last_draw_blocking_wait_us() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return last_draw_blocking_wait_us_;
}

bool WindowsD3D11PresentationBackend::copy_last_frame_info(
    PresentationBackendFrameInfo* out) const {
  if (!out) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!last_frame_info_available_) {
    return false;
  }
  *out = last_frame_info_;
  return true;
}

bool WindowsD3D11PresentationBackend::capture_front_buffer(
    std::vector<uint8_t>& bgra,
    int& width,
    int& height) {
  return capture_front_buffer_region(
      0, 0, std::numeric_limits<int>::max(),
      std::numeric_limits<int>::max(), bgra, width, height);
}

bool WindowsD3D11PresentationBackend::capture_front_buffer_region(
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& region_width,
    int& region_height) {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
  ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    source = last_completed_target_;
    output_target = output_target_;
  }
  bgra.clear();
  region_width = 0;
  region_height = 0;
  if (width <= 0 || height <= 0 || !source || !device_ || !context_ ||
      output_target == ColorOutputTarget::kWindowsLinearScRGB) {
    return false;
  }
  D3D11_TEXTURE2D_DESC source_desc = {};
  source->GetDesc(&source_desc);
  if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    set_error("Windows D3D11 capture requires an SDR BGRA8 target");
    return false;
  }
  const int full_width = static_cast<int>(source_desc.Width);
  const int full_height = static_cast<int>(source_desc.Height);
  const int left = std::clamp(x, 0, full_width);
  const int top = std::clamp(y, 0, full_height);
  const int right = static_cast<int>(std::clamp<int64_t>(
      static_cast<int64_t>(x) + width, left, full_width));
  const int bottom = static_cast<int>(std::clamp<int64_t>(
      static_cast<int64_t>(y) + height, top, full_height));
  region_width = right - left;
  region_height = bottom - top;
  if (region_width <= 0 || region_height <= 0) {
    region_width = 0;
    region_height = 0;
    return false;
  }
  if (!ensure_capture_staging(static_cast<UINT>(region_width),
                              static_cast<UINT>(region_height),
                              source_desc.Format)) {
    region_width = 0;
    region_height = 0;
    return false;
  }
  const D3D11_BOX source_box = {
      static_cast<UINT>(left), static_cast<UINT>(top), 0,
      static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT result = S_OK;
  const size_t region_stride = static_cast<size_t>(region_width) * 4u;
  {
    ScopedD3D11ContextLock context_lock(multithread_.Get());
    context_->CopySubresourceRegion(capture_staging_.Get(), 0, 0, 0, 0,
                                    source.Get(), 0, &source_box);
    if (!wait_for_gpu("capture-region")) {
      region_width = 0;
      region_height = 0;
      return false;
    }
    result = context_->Map(capture_staging_.Get(), 0, D3D11_MAP_READ, 0,
                           &mapped);
    if (FAILED(result)) {
      record_device_error(result, "capture region Map");
      region_width = 0;
      region_height = 0;
      return false;
    }
    bgra.resize(region_stride * static_cast<size_t>(region_height));
    for (int row = 0; row < region_height; ++row) {
      std::memcpy(bgra.data() + static_cast<size_t>(row) * region_stride,
                  static_cast<const uint8_t*>(mapped.pData) +
                      static_cast<size_t>(row) * mapped.RowPitch,
                  region_stride);
    }
    context_->Unmap(capture_staging_.Get(), 0);
  }
  return true;
}

bool WindowsD3D11PresentationBackend::ensure_capture_staging(
    UINT width,
    UINT height,
    DXGI_FORMAT format) {
  if (capture_staging_ && capture_staging_format_ == format &&
      capture_staging_width_ >= width && capture_staging_height_ >= height) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++capture_staging_reuse_count_;
    return true;
  }
  const auto grow_dimension = [](UINT current, UINT required) {
    if (current >= required) {
      return current;
    }
    if (current == 0) {
      return required;
    }
    const uint64_t grown = static_cast<uint64_t>(current) +
        std::max<uint64_t>(1, static_cast<uint64_t>(current) / 2);
    return static_cast<UINT>(std::min<uint64_t>(
        D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        std::max<uint64_t>(required, grown)));
  };
  const UINT current_width =
      capture_staging_format_ == format ? capture_staging_width_ : 0;
  const UINT current_height =
      capture_staging_format_ == format ? capture_staging_height_ : 0;
  const UINT next_width = grow_dimension(current_width, width);
  const UINT next_height = grow_dimension(current_height, height);
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = next_width;
  desc.Height = next_height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  const HRESULT result = device_->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(result) || !staging) {
    record_device_error(result, "capture region CreateTexture2D");
    set_error("Windows D3D11 region capture staging allocation failed");
    return false;
  }
  capture_staging_ = std::move(staging);
  capture_staging_width_ = next_width;
  capture_staging_height_ = next_height;
  capture_staging_format_ = format;
  const uint64_t bytes_per_pixel =
      format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
  const uint64_t staging_bytes = static_cast<uint64_t>(next_width) *
      static_cast<uint64_t>(next_height) * bytes_per_pixel;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++capture_staging_allocation_count_;
    capture_staging_max_bytes_ =
        std::max(capture_staging_max_bytes_, staging_bytes);
    spdlog::info(
        "[WindowsCapture] staging allocation={} size={}x{} bytes={} "
        "requested={}x{}",
        capture_staging_allocation_count_, next_width, next_height,
        staging_bytes, width, height);
  }
  return true;
}

const char* WindowsD3D11PresentationBackend::last_error() const {
  thread_local std::string copy;
  std::lock_guard<std::mutex> lock(state_mutex_);
  copy = last_error_;
  return copy.c_str();
}

bool WindowsD3D11PresentationBackend::draw_frame(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks) {
  (void)hooks;
  bool available = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    available = initialized_ && context_ && !device_lost_;
    last_draw_blocking_wait_us_ = 0;
  }
  if (!available) {
    set_error("Windows D3D11 backend is unavailable");
    return false;
  }
  auto target = target_ring_.acquire_draw_target();
  if (!target) {
    const auto ring = target_ring_.diagnostics();
    if (ring.backpressure_count <= 8 ||
        ring.backpressure_count % 120 == 0) {
      spdlog::warn(
          "[WindowsInteraction] target ring backpressure={} targets={} "
          "available={} in_flight={} completed={} releases={} "
          "release_misses={}",
          ring.backpressure_count, ring.target_count, ring.available_count,
          ring.in_flight_count, ring.completed_count, ring.release_count,
          ring.release_miss_count);
    }
    set_error("Windows D3D11 target ring is busy");
    record_draw_failure();
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  const HRESULT rtv_result =
      device_->CreateRenderTargetView(target.Get(), nullptr, &rtv);
  if (FAILED(rtv_result) || !rtv) {
    target_ring_.complete_draw_target(target.Get(), false);
    record_device_error(rtv_result, "CreateRenderTargetView");
    set_error("Windows D3D11 target RTV creation failed");
    record_draw_failure();
    return false;
  }
  const auto presentation = build_presentation_snapshot(
      snapshot.decision,
      snapshot.layout,
      snapshot.track_geometry,
      snapshot.target_width,
      snapshot.target_height,
      snapshot.background_color);
  ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
  double sdr_white_level_nits = 80.0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    output_target = output_target_;
    sdr_white_level_nits = sdr_white_level_nits_;
  }
  const auto viewport_before = viewport_renderer_.stats();
  bool viewport_drawn = false;
  bool overlay_expected = false;
  bool overlay_applied = false;
  uint64_t overlay_fill_rect_count = 0;
  uint64_t overlay_line_rect_count = 0;
  {
    ScopedD3D11ContextLock context_lock(multithread_.Get());
    viewport_drawn = viewport_renderer_.draw(snapshot,
                                             presentation,
                                             rtv.Get(),
                                             output_target,
                                             sdr_white_level_nits);
    if (viewport_drawn && !hooks.suppress_analysis_overlay &&
        hooks.build_overlay_primitives) {
      const auto package = hooks.build_overlay_primitives(snapshot);
      overlay_expected = package && !package->empty();
      if (package && package->overlay_track_count > 0) {
        ++overlay_diagnostic_count_;
        if (overlay_diagnostic_count_ <= 12 ||
            overlay_diagnostic_count_ % 120 == 0) {
          spdlog::info(
              "[WindowsAnalysisOverlay] package tracks={} matched={} draw_tracks={} "
              "missing_slot={} missing_presented={} missing_index={} missing_frame={} "
              "generation={}",
              package->overlay_track_count, package->matched_track_count,
              package->tracks.size(), package->missing_track_slot_count,
              package->missing_presented_frame_count,
              package->missing_frame_index_count,
              package->overlay_frame_missing_count, package->cache_generation);
        }
      }
      if (overlay_expected) {
        overlay_applied = viewport_renderer_.draw_overlay(
            *package, presentation, rtv.Get(), output_target,
            sdr_white_level_nits);
        const auto overlay_stats = viewport_renderer_.stats();
        overlay_fill_rect_count = overlay_stats.overlay_last_fill_rect_count;
        overlay_line_rect_count = overlay_stats.overlay_last_line_rect_count;
        if (!overlay_applied) {
          spdlog::error("[WindowsAnalysisOverlay] D3D11 draw failed: {}",
                        viewport_renderer_.last_error());
        }
      }
    }
    if (viewport_drawn && hooks.draw_overlay) {
      hooks.draw_overlay(*this, snapshot);
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    overlay_last_expected_ = overlay_expected;
    overlay_last_applied_ = overlay_applied;
    overlay_last_fill_rect_count_ = overlay_fill_rect_count;
    overlay_last_line_rect_count_ = overlay_line_rect_count;
    if (overlay_expected) {
      ++overlay_expected_count_;
      if (overlay_applied) {
        ++overlay_applied_count_;
      } else {
        ++overlay_missed_count_;
        ++overlay_gpu_failure_count_;
      }
    }
  }
  if (!viewport_drawn) {
    target_ring_.complete_draw_target(target.Get(), false);
    set_error("Windows D3D11 viewport draw failed: " +
              viewport_renderer_.last_error());
    record_draw_failure();
    return false;
  }
  const auto viewport_after = viewport_renderer_.stats();
  const bool projection_only_draw =
      viewport_after.source_frame_cache_hit_count >
          viewport_before.source_frame_cache_hit_count &&
      viewport_after.video_source_update_count ==
          viewport_before.video_source_update_count;
  // A projection-only interaction reuses the presentation-device source
  // cache. The runner compositor consumes this target on the same immediate
  // context, so D3D11 command ordering already places its sample after this
  // draw. The ring slot remains retained until the compositor's GPU query
  // completes; an intermediate CPU wait here only serializes interaction.
  uint64_t blocking_wait_us = 0;
  if (!projection_only_draw) {
    const auto wait_start = std::chrono::steady_clock::now();
    const bool wait_succeeded = wait_for_gpu("draw_frame");
    blocking_wait_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - wait_start)
            .count());
    if (!wait_succeeded) {
      target_ring_.complete_draw_target(target.Get(), false);
      record_draw_failure();
      return false;
    }
  }
  if (!target_ring_.complete_draw_target(target.Get(), true)) {
    set_error("Windows D3D11 target completion state mismatch");
    record_draw_failure();
    return false;
  }

  PresentationBackendFrameInfo frame_info;
  for (const auto& frame : presentation.frames) {
    if (!frame.present) {
      continue;
    }
    frame_info.width = frame.width;
    frame_info.height = frame.height;
    frame_info.pts_us = frame.pts_us;
    frame_info.dts_us = frame.dts_us;
    frame_info.duration_us = frame.duration_us;
    frame_info.analysis_frame_index = frame.analysis_frame_index;
    frame_info.frame_identity_mode = frame.frame_identity_mode;
    frame_info.source_packet_index = frame.source_packet_index;
    frame_info.source_packet_size = frame.source_packet_size;
    frame_info.source_packet_pos = frame.source_packet_pos;
    frame_info.source_packet_pts = frame.source_packet_pts;
    frame_info.source_packet_dts = frame.source_packet_dts;
    frame_info.color_range = frame.color_range;
    frame_info.color_matrix = frame.color_matrix;
    frame_info.color_transfer = frame.color_transfer;
    frame_info.color_primaries = frame.color_primaries;
    break;
  }
  frame_info.target_pixel_buffer_address =
      reinterpret_cast<uint64_t>(target.Get());
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_completed_target_ = target;
    last_frame_info_ = frame_info;
    last_frame_info_available_ = true;
    last_draw_blocking_wait_us_ = blocking_wait_us;
    ++draw_count_;
    consecutive_draw_failures_ = 0;
    last_error_.clear();
  }
  return true;
}

bool WindowsD3D11PresentationBackend::install_ring(
    const WindowsD3D11TargetRingInstall& install) {
  ColorOutputTarget output_target = ColorOutputTarget::kSDRToneMappedBT709;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    output_target = output_target_;
  }
  if (install.format != target_format_for(output_target)) {
    set_error("Windows D3D11 target-ring format does not match output mode");
    return false;
  }
  std::string error;
  if (!target_ring_.install(install.textures,
                            install.texture_count,
                            install.displayed_texture,
                            install.protected_texture,
                            install.width,
                            install.height,
                            install.format,
                            error)) {
    set_error(std::move(error));
    return false;
  }
  return true;
}

bool WindowsD3D11PresentationBackend::wait_for_gpu(const char* label) {
  if (!context_ || !completion_query_) {
    set_error("Windows D3D11 completion query is unavailable");
    return false;
  }
  context_->End(completion_query_.Get());
  context_->Flush();
  const auto start = std::chrono::steady_clock::now();
  HRESULT result = S_FALSE;
  while ((result = context_->GetData(
              completion_query_.Get(), nullptr, 0, 0)) == S_FALSE) {
    if (std::chrono::steady_clock::now() - start >=
        std::chrono::milliseconds(100)) {
      set_error(std::string("Windows D3D11 GPU wait timed out: ") +
                (label ? label : "unknown"));
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  if (FAILED(result)) {
    record_device_error(result, label ? label : "GetData");
    set_error("Windows D3D11 GPU completion query failed");
    return false;
  }
  return true;
}

void WindowsD3D11PresentationBackend::set_error(std::string error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_error_ = std::move(error);
}

void WindowsD3D11PresentationBackend::record_draw_failure() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  ++draw_failure_count_;
  ++consecutive_draw_failures_;
}

void WindowsD3D11PresentationBackend::record_device_error(
    HRESULT result,
    const char* operation) {
  (void)operation;
  if (result != DXGI_ERROR_DEVICE_REMOVED &&
      result != DXGI_ERROR_DEVICE_RESET &&
      result != DXGI_ERROR_DEVICE_HUNG) {
    return;
  }
  const HRESULT queried = device_ ? device_->GetDeviceRemovedReason() : result;
  std::lock_guard<std::mutex> lock(state_mutex_);
  device_lost_ = true;
  device_removed_reason_ = FAILED(queried) ? queried : result;
}

std::unique_ptr<PresentationBackend> create_windows_presentation_backend() {
  return std::make_unique<WindowsD3D11PresentationBackend>();
}

}  // namespace vr
