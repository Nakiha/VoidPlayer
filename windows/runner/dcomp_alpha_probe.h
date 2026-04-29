#ifndef RUNNER_DCOMP_ALPHA_PROBE_H_
#define RUNNER_DCOMP_ALPHA_PROBE_H_

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

class DCompAlphaProbe {
 public:
  DCompAlphaProbe();
  ~DCompAlphaProbe();

  DCompAlphaProbe(const DCompAlphaProbe&) = delete;
  DCompAlphaProbe& operator=(const DCompAlphaProbe&) = delete;

  bool Initialize(HWND target_hwnd, HWND timer_hwnd, bool topmost);
  bool InitializeWithSdrOverlay(HWND target_hwnd,
                                HWND timer_hwnd,
                                bool topmost);
  bool InitializeWithFlutterSurface(HWND target_hwnd,
                                    HWND flutter_hwnd,
                                    HWND timer_hwnd);
  void Shutdown();
  void Resize(const RECT& client_rect);
  void Render();

 private:
  bool CreateDevice();
  bool CreateShaders();
  bool CreateSwapChain(int width, int height);
  bool CreateSdrOverlaySwapChain(int width, int height);
  bool CreateComposition(HWND target_hwnd, bool topmost);
  void RenderHdrSurface();
  void RenderSdrOverlaySurface();
  void UpdateVisualRect(const RECT& client_rect);

  HWND hwnd_ = nullptr;
  HWND timer_hwnd_ = nullptr;
  HWND flutter_surface_hwnd_ = nullptr;
  int width_ = 640;
  int height_ = 360;
  int offset_x_ = 0;
  int offset_y_ = 0;
  bool color_flip_ = false;
  bool compose_flutter_surface_ = false;
  bool compose_sdr_overlay_ = false;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> sdr_overlay_swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> sdr_overlay_rtv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> sdr_overlay_pixel_shader_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> root_visual_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> video_visual_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> sdr_overlay_visual_;
  Microsoft::WRL::ComPtr<IUnknown> flutter_surface_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> flutter_visual_;
};

#endif  // RUNNER_DCOMP_ALPHA_PROBE_H_
