#pragma once

#include <flutter_windows.h>

#if !defined(FLUTTER_WINDOWS_SURFACE_EXPORT_API) || \
    FLUTTER_WINDOWS_SURFACE_EXPORT_API < 2
#error "VoidPlayer Windows compositor requires the locked Flutter surface-export engine"
#endif

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct WindowsNativeCompositorDiagnostics {
  bool initialized = false;
  bool flutter_export_enabled = false;
  bool last_composite_succeeded = false;
  uint64_t flutter_publish_count = 0;
  uint64_t composite_count = 0;
  uint64_t acquire_failure_count = 0;
  uint64_t keyed_mutex_failure_count = 0;
  uint64_t present_failure_count = 0;
  uint64_t video_target_generation = 0;
  uint64_t video_publish_count = 0;
  uint64_t video_present_count = 0;
  uint64_t last_flutter_frame_generation = 0;
  int64_t sdr_white_level_milli_nits = 80000;
  bool scrgb_output_enabled = false;
  std::string output_mode = "native-compositor-sdr";
  std::string swap_chain_format = "bgra8";
  std::string swap_chain_color_space = "RGB_FULL_G22_NONE_P709";
  std::string flutter_source_format = "bgra8-premultiplied-srgb";
  std::string video_target_format = "unavailable";
  std::string output_fallback_reason = "none";
  std::string last_error;
};

struct WindowsNativeCompositorOutputConfig {
  bool linear_scrgb = false;
  double sdr_white_level_nits = 80.0;
};

// Passive final compositor: it samples the latest runner-owned native viewport
// and Flutter's already-published premultiplied-alpha surface into one DComp
// visual. It neither owns an input window nor requests Flutter frames.
class WindowsNativeCompositor final {
 public:
  WindowsNativeCompositor(HWND top_level_window,
                          FlutterDesktopViewRef flutter_view);
  ~WindowsNativeCompositor();

  WindowsNativeCompositor(const WindowsNativeCompositor&) = delete;
  WindowsNativeCompositor& operator=(const WindowsNativeCompositor&) = delete;

  bool Start();
  void Stop();
  void NotifyResize();
  bool ConfigureOutput(const WindowsNativeCompositorOutputConfig& config,
                       std::string& error);
  bool CreateVideoTargetRing(uint32_t width,
                             uint32_t height,
                             DXGI_FORMAT format,
                             size_t target_count,
                             std::vector<void*>& textures);
  void ClearVideoTargetRing();
  bool PresentVideoTarget(ID3D11Texture2D* texture, uint32_t timeout_ms = 250);
  void SetVideoViewportRect(int left,
                            int top,
                            int width,
                            int height,
                            int surface_width,
                            int surface_height);
  ID3D11Device* device() const { return device_.Get(); }
  WindowsNativeCompositorDiagnostics diagnostics() const;

 private:
  static void OnFlutterSurfacePublished(FlutterDesktopViewRef view,
                                        uint64_t frame_generation,
                                        void* user_data);
  void SignalComposite(uint64_t flutter_generation = 0);
  void Run();
  bool InitializeOnThread();
  void ShutdownOnThread();
  bool ResizeSwapChain();
  bool ApplyPendingOutputConfiguration();
  bool ApplyOutputConfiguration(
      const WindowsNativeCompositorOutputConfig& config,
      std::string& error);
  bool ResizeSwapChainBuffers(uint32_t width, uint32_t height,
                              DXGI_FORMAT format,
                              DXGI_COLOR_SPACE_TYPE color_space,
                              std::string& error);
  bool CompositeLatest();
  bool CreatePipeline();
  bool WaitForGpu();
  void CompleteVideoPresentation(uint64_t serial, bool success);
  void SetError(std::string error);

  HWND top_level_window_ = nullptr;
  FlutterDesktopViewRef flutter_view_ = nullptr;
  HANDLE wake_event_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE ready_event_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> initialization_succeeded_{false};
  std::atomic<bool> resize_pending_{false};

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread_;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  Microsoft::WRL::ComPtr<ID3D11Query> completion_query_;
  // The keyed-mutex Flutter export cannot be reacquired until Flutter renders
  // into that ring slot again. Copy each newly published generation once, then
  // composite native video frames against this runner-owned immutable cache.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> flutter_cache_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flutter_cache_srv_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> video_target_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> video_targets_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> retired_video_targets_;
  uint64_t video_publish_serial_ = 0;
  uint64_t video_completed_serial_ = 0;
  bool last_video_presentation_succeeded_ = false;
  uint64_t latest_flutter_published_generation_ = 0;
  uint64_t cached_flutter_generation_ = 0;
  uint64_t cached_flutter_ring_generation_ = 0;
  uint64_t flutter_cache_refresh_count_ = 0;
  int video_viewport_left_ = 0;
  int video_viewport_top_ = 0;
  int video_viewport_width_ = 0;
  int video_viewport_height_ = 0;
  int video_viewport_surface_width_ = 0;
  int video_viewport_surface_height_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  DXGI_FORMAT output_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  DXGI_COLOR_SPACE_TYPE output_color_space_ =
      DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  DXGI_FORMAT video_target_format_ = DXGI_FORMAT_UNKNOWN;
  WindowsNativeCompositorOutputConfig requested_output_config_;
  WindowsNativeCompositorOutputConfig applied_output_config_;
  uint64_t requested_output_generation_ = 0;
  uint64_t completed_output_generation_ = 0;
  bool completed_output_succeeded_ = true;

  mutable std::mutex state_mutex_;
  std::condition_variable state_condition_;
  WindowsNativeCompositorDiagnostics diagnostics_;
};
