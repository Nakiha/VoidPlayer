#pragma once

#include "metal_presentation_backend_bridge.h"
#include "renderer/render/presentation_backend.h"

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

namespace vp_macos {

class MetalPresentationBackend final : public vr::PresentationBackend {
public:
  MetalPresentationBackend() = default;
  ~MetalPresentationBackend() override;

  MetalPresentationBackend(const MetalPresentationBackend&) = delete;
  MetalPresentationBackend& operator=(const MetalPresentationBackend&) = delete;

  vr::PresentationBackendKind kind() const override { return vr::PresentationBackendKind::Metal; }
  const char* name() const override { return "metal-cvpixelbuffer"; }
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
  bool copy_last_frame_info(vr::PresentationBackendFrameInfo* out) const override;
  bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) override;
  const char* last_error() const override;
  bool draw_frame(const vr::RendererDrawSnapshot& snapshot,
                  const vr::PresentationBackendDrawHooks& hooks) override;

  bool available() const;
  int width() const { return width_; }
  int height() const { return height_; }
  VPMacOSMetalUploader* uploader() const { return uploader_; }
  void set_draw_target(void* pixel_buffer,
                       int32_t width,
                       int32_t height,
                       int32_t max_track_slots);
  void set_draw_target_ring(const void* const* pixel_buffers,
                            size_t pixel_buffer_count,
                            void* displayed_pixel_buffer,
                            void* protected_pixel_buffer,
                            int32_t width,
                            int32_t height,
                            int32_t max_track_slots);
  void clear_draw_target();
  bool contains_draw_target(void* pixel_buffer) const;
  void mark_displayed_target(void* pixel_buffer);
  void protect_target(void* pixel_buffer);
  void release_target(void* pixel_buffer);
  bool copy_last_draw_frame_info(VPMacOSNativeFrameInfo* out) const;
  struct DrawTicket {
    uint64_t sequence = 0;
    uint64_t target_pixel_buffer_address = 0;
    uint64_t source_signature = 0;
    uint64_t overlay_generation = 0;
    const char* path = "unknown";
  };

  DrawTicket make_draw_ticket(uint64_t target_pixel_buffer_address,
                              uint64_t source_signature,
                              uint64_t overlay_generation,
                              const char* path);
  void complete_async_draw_result(const DrawTicket& ticket,
                                  const VPMacOSNativeFrameInfo& frame_info,
                                  bool success,
                                  const char* error,
                                  int64_t total_us,
                                  bool overlay_expected,
                                  bool overlay_applied,
                                  bool gpu_attempted,
                                  bool gpu_succeeded,
                                  bool cpu_attempted,
                                  size_t fill_rect_count,
                                  size_t line_rect_count);
  void complete_ring_draw_target(uint64_t target_pixel_buffer_address,
                                 bool success);
  void finish_async_draw();

private:
  std::string target_ring_state_summary_locked() const;
  void set_last_error(std::string error);
  void mark_draw_failure(std::string error);
  void mark_draw_success(const VPMacOSNativeFrameInfo& frame_info);
  void record_overlay_result(bool expected,
                             bool applied,
                             bool gpu_attempted,
                             bool gpu_succeeded,
                             bool cpu_attempted,
                             size_t fill_rect_count,
                             size_t line_rect_count);
  void invalidate_source_cache();
  bool try_begin_async_draw(const char* source);
  void* try_acquire_ring_draw_target(const char* source);
  bool shutting_down_async() const;

  VPMacOSMetalUploader* uploader_ = nullptr;
  void* draw_target_pixel_buffer_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int draw_target_width_ = 0;
  int draw_target_height_ = 0;
  int draw_target_max_track_slots_ = VPMacOSNativeMaxTracks;
  bool last_draw_frame_info_available_ = false;
  VPMacOSNativeFrameInfo last_draw_frame_info_ = {};
  std::string last_error_;
  uint64_t draw_failure_count_ = 0;
  uint64_t consecutive_draw_failures_ = 0;
  uint64_t staging_allocation_count_ = 0;
  uint64_t staging_reuse_count_ = 0;
  uint64_t draw_profiler_count_ = 0;
  size_t staging_max_bytes_ = 0;
  bool overlay_last_expected_ = false;
  bool overlay_last_applied_ = false;
  uint64_t overlay_last_fill_rect_count_ = 0;
  uint64_t overlay_last_line_rect_count_ = 0;
  uint64_t overlay_expected_count_ = 0;
  uint64_t overlay_applied_count_ = 0;
  uint64_t overlay_missed_count_ = 0;
  uint64_t overlay_gpu_success_count_ = 0;
  uint64_t overlay_gpu_failure_count_ = 0;
  uint64_t overlay_cpu_fallback_count_ = 0;
  uint64_t metal_command_failure_count_ = 0;
  uint64_t metal_buffer_exhaustion_count_ = 0;
  uint64_t metal_command_completion_sample_count_ = 0;
  uint64_t metal_command_completion_p95_us_ = 0;
  uint64_t video_source_update_count_ = 0;
  uint64_t viewport_composite_count_ = 0;
  uint64_t source_frame_cache_hit_count_ = 0;
  uint64_t source_frame_cache_miss_count_ = 0;
  std::vector<uint64_t> metal_command_completion_samples_us_;
  std::vector<uint8_t> staging_buffer_;
  bool cached_package_valid_ = false;
  uint64_t cached_package_source_signature_ = 0;
  uint64_t last_source_signature_ = 0;
  VPMacOSNativePresentFramePackageInfo cached_package_ = {};
  bool last_draw_succeeded_ = false;
  bool headless_ = true;
  mutable std::mutex async_mutex_;
  std::condition_variable async_cv_;
  uint64_t in_flight_draws_ = 0;
  uint64_t next_draw_ticket_sequence_ = 0;
  bool async_shutdown_ = false;
  enum class TargetState {
    Available,
    InFlight,
    Completed,
    Displayed,
    Protected,
  };
  struct TargetSlot {
    void* pixel_buffer = nullptr;
    TargetState state = TargetState::Available;
  };
  std::vector<TargetSlot> target_ring_;
  bool target_ring_enabled_ = false;
  uint64_t displayed_target_address_ = 0;
  uint64_t protected_target_address_ = 0;
};

std::unique_ptr<vr::PresentationBackend> create_metal_presentation_backend();

}  // namespace vp_macos
