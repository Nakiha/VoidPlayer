#pragma once

#include "renderer/render/presentation_backend.h"

#include <CoreFoundation/CoreFoundation.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct VPWgpuMetalRenderer;

namespace vp_macos {

class WgpuMetalPresentationBackend final : public vr::PresentationBackend {
  friend void wgpu_async_draw_completed(void* user_data, int32_t result);

public:
  WgpuMetalPresentationBackend();
  ~WgpuMetalPresentationBackend() override;

  WgpuMetalPresentationBackend(const WgpuMetalPresentationBackend&) = delete;
  WgpuMetalPresentationBackend& operator=(const WgpuMetalPresentationBackend&) = delete;

  vr::PresentationBackendKind kind() const override {
    return vr::PresentationBackendKind::WgpuMetal;
  }
  const char* name() const override { return "wgpu-metal"; }
  bool initialize(const vr::PresentationBackendConfig& config) override;
  void shutdown() override;
  bool headless() const override { return headless_; }
  bool completes_draw_asynchronously() const override { return true; }
  bool update_headless_output(void* output,
                              int width,
                              int height,
                              int max_track_slots) override;
  bool update_headless_output_ring(const void* const* pixel_buffers,
                                   size_t pixel_buffer_count,
                                   void* displayed_pixel_buffer,
                                   void* protected_pixel_buffer,
                                   int width,
                                   int height,
                                   int max_track_slots) override;
  void mark_headless_output_displayed(void* pixel_buffer) override;
  void protect_headless_output(void* pixel_buffer) override;
  void release_headless_output(void* pixel_buffer) override;
  void clear_headless_output() override;
  vr::PresentationBackendStats presentation_stats() const override;
  vr::PresentationBackendDiagnostics diagnostics() const override;
  bool copy_last_frame_info(vr::PresentationBackendFrameInfo* out) const override;
  bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) override;
  bool capture_front_buffer_region(int x,
                                   int y,
                                   int width,
                                   int height,
                                   std::vector<uint8_t>& bgra,
                                   int& region_width,
                                   int& region_height) override;
  const char* last_error() const override { return last_error_.c_str(); }
  bool draw_frame(const vr::RendererDrawSnapshot& snapshot,
                  const vr::PresentationBackendDrawHooks& hooks) override;

private:
  enum class TargetState {
    Available,
    InFlight,
    Completed,
    Displayed,
    Protected,
  };
  struct TargetSlot {
    void* pixel_buffer = nullptr;
    void* cached_texture_ref = nullptr;
    int32_t cached_width = 0;
    int32_t cached_height = 0;
    uint64_t cached_pixel_format = 0;
    TargetState state = TargetState::Available;
  };
  struct SourceTextureCacheEntry {
    void* pixel_buffer = nullptr;
    CFTypeRef texture_ref = nullptr;
    uint64_t pixel_format = 0;
    int32_t width = 0;
    int32_t height = 0;
    size_t plane = 0;
    uint64_t last_used = 0;
  };
  struct SourceMetricsResult {
    bool viewport_composite = false;
    bool cache_hit = false;
  };
  struct AsyncState {
    std::mutex mutex;
    std::condition_variable cv;
    WgpuMetalPresentationBackend* backend = nullptr;
    size_t active_callbacks = 0;
    bool shutdown = false;
  };
  struct AsyncDrawPending {
    ~AsyncDrawPending();

    std::shared_ptr<AsyncState> state;
    vr::PresentationBackendDrawHooks hooks;
    vr::PresentationBackendFrameInfo frame_info{};
    std::chrono::steady_clock::time_point draw_start;
    std::chrono::steady_clock::time_point render_call_start;
    uint64_t target_pixel_buffer_address = 0;
    uint64_t package_copy_us = 0;
    int32_t package_storage = 0;
    bool source_upload = true;
    bool target_ring_acquired = false;
    bool overlay_expected = false;
    uint64_t overlay_fill_rect_count = 0;
    uint64_t overlay_line_rect_count = 0;
    void* destination_texture_ref = nullptr;
    std::array<void*, 4> source_y_texture_refs{};
    std::array<void*, 4> source_uv_texture_refs{};
  };

  bool available() const;
  bool available_locked() const;
  void set_last_error(std::string error);
  void mark_draw_failure(std::string error);
  void mark_draw_success(const vr::PresentationBackendFrameInfo& frame_info,
                         int32_t package_storage,
                         bool source_upload = true);
  bool target_installed_locked() const;
  void* acquire_draw_target_locked();
  void release_target_texture_cache_locked();
  void release_target_texture_cache_for_slot(TargetSlot& slot);
  void release_source_texture_cache_locked();
  void* cached_target_texture_ref(void* pixel_buffer,
                                  uint64_t metal_pixel_format,
                                  int32_t width,
                                  int32_t height,
                                  std::string& error);
  void* cached_source_texture_ref(void* pixel_buffer,
                                  uint64_t metal_pixel_format,
                                  int32_t width,
                                  int32_t height,
                                  size_t plane,
                                  std::string& error);
  void complete_ring_draw_target(uint64_t target_pixel_buffer_address,
                                 bool success);
  void* capture_target_locked() const;
  void* current_draw_target_locked() const;
  SourceMetricsResult record_source_metrics(
      const vr::RendererDrawSnapshot& snapshot,
      const vr::PresentationBackendDrawHooks& hooks,
      int32_t target_width,
      int32_t target_height,
      int32_t track_slots);
  void record_wgpu_command_result(uint64_t elapsed_us, bool success);
  void record_present_package_timing(uint64_t copy_us,
                                     uint64_t gpu_wait_us,
                                     uint64_t total_us);
  void complete_async_draw(std::unique_ptr<AsyncDrawPending> pending,
                           bool success);

  std::shared_ptr<AsyncState> async_state_;
  void* metal_device_ = nullptr;
  void* texture_cache_ = nullptr;
  VPWgpuMetalRenderer* wgpu_renderer_ = nullptr;
  void* draw_target_pixel_buffer_ = nullptr;
  void* single_target_texture_ref_ = nullptr;
  int32_t single_target_texture_width_ = 0;
  int32_t single_target_texture_height_ = 0;
  uint64_t single_target_texture_pixel_format_ = 0;
  int width_ = 0;
  int height_ = 0;
  int draw_target_width_ = 0;
  int draw_target_height_ = 0;
  int draw_target_max_track_slots_ = 1;
  int32_t draw_target_output_format_ = 0;
  int32_t draw_target_output_color_mode_ = 0;
  std::string draw_target_render_format_ = "unknown";
  std::string draw_target_color_space_ = "unknown";
  bool headless_ = true;
  bool target_ring_enabled_ = false;
  uint64_t displayed_target_address_ = 0;
  uint64_t protected_target_address_ = 0;
  std::string wgpu_adapter_description_ = "unknown";
  std::string wgpu_driver_type_ = "unknown";
  std::string wgpu_backend_name_ = "unknown";
  std::string wgpu_device_type_ = "unknown";
  uint32_t wgpu_adapter_vendor_id_ = 0;
  uint32_t wgpu_adapter_device_id_ = 0;
  bool wgpu_supports_16bit_norm_ = false;
  bool retained_source_available_ = false;
  std::vector<TargetSlot> target_ring_;
  std::vector<SourceTextureCacheEntry> source_texture_cache_;
  uint64_t source_texture_cache_clock_ = 0;
  mutable std::mutex mutex_;

  std::string last_error_;
  bool last_draw_succeeded_ = false;
  uint64_t draw_failure_count_ = 0;
  uint64_t consecutive_draw_failures_ = 0;
  uint64_t cvpixelbuffer_upload_count_ = 0;
  uint64_t present_package_upload_count_ = 0;
  uint64_t last_present_package_copy_us_ = 0;
  uint64_t last_present_package_gpu_wait_us_ = 0;
  uint64_t last_present_package_total_us_ = 0;
  uint64_t staging_allocation_count_ = 0;
  uint64_t staging_reuse_count_ = 0;
  uint64_t target_ring_backpressure_count_ = 0;
  size_t staging_max_bytes_ = 0;
  int32_t last_present_package_storage_ = 0;
  bool overlay_last_expected_ = false;
  bool overlay_last_applied_ = false;
  uint64_t overlay_last_fill_rect_count_ = 0;
  uint64_t overlay_last_line_rect_count_ = 0;
  uint64_t overlay_expected_count_ = 0;
  uint64_t overlay_applied_count_ = 0;
  uint64_t overlay_missed_count_ = 0;
  uint64_t overlay_gpu_success_count_ = 0;
  uint64_t overlay_gpu_failure_count_ = 0;
  uint64_t video_source_update_count_ = 0;
  uint64_t viewport_composite_count_ = 0;
  uint64_t source_frame_cache_hit_count_ = 0;
  uint64_t source_frame_cache_miss_count_ = 0;
  uint64_t last_source_signature_ = 0;
  uint64_t metal_command_failure_count_ = 0;
  uint64_t metal_command_completion_sample_count_ = 0;
  uint64_t metal_command_completion_p95_us_ = 0;
  std::vector<uint64_t> metal_command_completion_samples_us_;
  std::vector<uint8_t> staging_buffer_;
  bool last_frame_info_available_ = false;
  vr::PresentationBackendFrameInfo last_frame_info_{};
  bool retained_source_frame_info_available_ = false;
  vr::PresentationBackendFrameInfo retained_source_frame_info_{};
};

std::unique_ptr<vr::PresentationBackend> create_wgpu_metal_presentation_backend();

}  // namespace vp_macos
