#pragma once

#include "renderer/render/presentation_backend.h"
#include "windows/presentation/windows_d3d11_target_ring.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <string>

namespace vr {

class WindowsD3D11PresentationBackend final : public PresentationBackend {
 public:
  ~WindowsD3D11PresentationBackend() override;

  PresentationBackendKind kind() const override {
    return PresentationBackendKind::NativeD3D11;
  }
  const char* name() const override { return "windows-native-d3d11"; }
  bool initialize(const PresentationBackendConfig& config) override;
  void shutdown() override;
  bool offscreen() const override { return true; }
  bool poll_device_removed(const char* operation) override;
  bool device_lost() const override;
  long device_removed_reason() const override;
  void wait_idle(const char* label) override;
  bool update_offscreen_target_ring(const void* const* textures,
                                    size_t texture_count,
                                    void* displayed_texture,
                                    void* protected_texture,
                                    int width,
                                    int height,
                                    int max_track_slots) override;
  void mark_offscreen_target_displayed(void* texture) override;
  void protect_offscreen_target(void* texture) override;
  void release_offscreen_target(void* texture) override;
  void clear_offscreen_target() override;
  void* native_render_device() const override { return device_.Get(); }
  PresentationBackendStats presentation_stats() const override;
  PresentationBackendDiagnostics diagnostics() const override;
  bool copy_last_frame_info(PresentationBackendFrameInfo* out) const override;
  bool capture_front_buffer(std::vector<uint8_t>& bgra,
                            int& width,
                            int& height) override;
  const char* last_error() const override;
  bool draw_frame(const RendererDrawSnapshot& snapshot,
                  const PresentationBackendDrawHooks& hooks) override;

 private:
  bool install_ring(const WindowsD3D11TargetRingInstall& install);
  bool wait_for_gpu(const char* label);
  void set_error(std::string error);
  void record_draw_failure();
  void record_device_error(HRESULT result, const char* operation);

  mutable std::mutex state_mutex_;
  WindowsD3D11TargetRing target_ring_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Query> completion_query_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> last_completed_target_;
  PresentationBackendFrameInfo last_frame_info_{};
  bool last_frame_info_available_ = false;
  bool initialized_ = false;
  bool device_lost_ = false;
  HRESULT device_removed_reason_ = S_OK;
  ColorOutputTarget output_target_ = ColorOutputTarget::kSDRToneMappedBT709;
  double sdr_white_level_nits_ = 80.0;
  int width_ = 0;
  int height_ = 0;
  int max_track_slots_ = 0;
  std::string last_error_;
  uint64_t draw_count_ = 0;
  uint64_t draw_failure_count_ = 0;
  uint64_t consecutive_draw_failures_ = 0;
};

std::unique_ptr<PresentationBackend> create_windows_presentation_backend();

}  // namespace vr
