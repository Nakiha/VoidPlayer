#pragma once

#include "renderer/renderer.h"

#include <functional>
#include <memory>
#include <shared_mutex>

namespace vr {

// Windows platform facade over the shared renderer. It owns no HWND, Flutter
// object, or final-compositor policy; those stay in the runner.
class WindowsNativePlayer final {
 public:
  WindowsNativePlayer();
  ~WindowsNativePlayer();

  WindowsNativePlayer(const WindowsNativePlayer&) = delete;
  WindowsNativePlayer& operator=(const WindowsNativePlayer&) = delete;

  bool initialize(const RendererConfig& config);
  void shutdown();
  bool initialized() const;

  void play();
  void pause();
  void seek(int64_t pts_us, int64_t request_id = -1);
  void step_forward();
  void step_backward();
  void set_speed(double speed);
  void set_loop_range(bool enabled, int64_t start_us, int64_t end_us);
  void set_audible_track(int file_id);
  void set_track_offset(int file_id, int64_t offset_us);
  int64_t track_offset_us(int file_id) const;
  void set_background_color(float red, float green, float blue, float alpha);
  void apply_layout(const LayoutState& layout);
  void apply_interaction_layout(const LayoutState& layout);
  void resize(int width, int height);

  int add_track(const std::string& path, bool use_hardware_decode);
  void remove_track(int file_id);
  bool is_playing() const;
  int64_t current_pts_us() const;
  int64_t duration_us() const;
  LayoutState layout() const;
  std::vector<TrackInfo> tracks() const;
  std::vector<TrackPerfStats> track_perf_stats() const;
  RendererGpuMemoryStats gpu_memory_stats() const;
  PresentationBackendMetrics presentation_metrics() const;
  PresentationBackendStats presentation_stats() const;
  PresentationBackendDiagnostics presentation_diagnostics() const;
  std::string presentation_error() const;
  bool copy_last_frame_info(PresentationBackendFrameInfo* out) const;

  void set_frame_callback(RendererFrameCallback callback);
  void set_frame_failure_callback(std::function<void(const char*)> callback);
  void set_event_callback(RendererEventCallback callback);
  void mark_target_displayed(void* texture);
  void protect_target(void* texture);
  void release_target(void* texture);
  bool install_target_ring(const void* const* textures,
                           size_t texture_count,
                           void* displayed_texture,
                           void* protected_texture,
                           int width,
                           int height,
                           int max_track_slots);
  bool update_presentation_sdr_white_level(double nits);
  bool request_frame_refresh(const char* reason);
  RendererFrameRefreshResult request_interaction_frame();
  bool capture_front_buffer(std::vector<uint8_t>& bgra,
                            int& width,
                            int& height);
  bool capture_front_buffer_region(int x,
                                   int y,
                                   int width,
                                   int height,
                                   std::vector<uint8_t>& bgra,
                                   int& region_width,
                                   int& region_height);

 private:
  bool ready_locked() const;

  mutable std::shared_mutex mutex_;
  std::unique_ptr<Renderer> renderer_;
};

}  // namespace vr
