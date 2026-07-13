#pragma once

#include <flutter_windows.h>

#if !defined(FLUTTER_WINDOWS_SURFACE_EXPORT_API) || \
    FLUTTER_WINDOWS_SURFACE_EXPORT_API < 2
#error "VoidPlayer Windows compositor requires the locked Flutter surface-export engine"
#endif

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct WindowsNativeCompositorDiagnostics {
  bool initialized = false;
  bool flutter_export_enabled = false;
  bool last_composite_succeeded = false;
  uint64_t flutter_publish_count = 0;
  uint64_t composite_count = 0;
  uint64_t acquire_failure_count = 0;
  uint64_t keyed_mutex_failure_count = 0;
  uint64_t present_failure_count = 0;
  uint64_t last_flutter_frame_generation = 0;
  std::string last_error;
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
  void PublishVideoTarget(ID3D11Texture2D* texture);
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
  bool CompositeLatest();
  bool CreatePipeline();
  bool WaitForGpu();
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
  Microsoft::WRL::ComPtr<IDXGISwapChain2> swap_chain_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  Microsoft::WRL::ComPtr<ID3D11Query> completion_query_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> video_target_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  mutable std::mutex state_mutex_;
  WindowsNativeCompositorDiagnostics diagnostics_;
};
