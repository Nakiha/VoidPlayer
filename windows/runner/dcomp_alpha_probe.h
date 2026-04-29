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
  void Shutdown();
  void Resize(const RECT& client_rect);
  void Render();

 private:
  bool CreateDevice();
  bool CreateShaders();
  bool CreateSwapChain(int width, int height);
  bool CreateComposition(HWND target_hwnd, bool topmost);
  void UpdateVisualRect(const RECT& client_rect);

  HWND hwnd_ = nullptr;
  HWND timer_hwnd_ = nullptr;
  int width_ = 640;
  int height_ = 360;
  int offset_x_ = 0;
  int offset_y_ = 0;
  bool color_flip_ = false;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> root_visual_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> video_visual_;
};

#endif  // RUNNER_DCOMP_ALPHA_PROBE_H_
