#include "dcomp_alpha_probe.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <flutter_windows.h>
#include <dxgi1_6.h>
#include <spdlog/spdlog.h>

namespace {

constexpr UINT_PTR kProbeTimerId = 0xD0C0;
constexpr UINT kProbeTimerMs = 500;
constexpr DXGI_COLOR_SPACE_TYPE kScRgbColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

constexpr char kHdrProbeShader[] = R"(
struct VsOut {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

VsOut VSMain(uint vertex_id : SV_VertexID) {
  float2 positions[3] = {
    float2(-1.0, -1.0),
    float2(-1.0,  3.0),
    float2( 3.0, -1.0),
  };
  VsOut output;
  output.pos = float4(positions[vertex_id], 0.0, 1.0);
  output.uv = output.pos.xy * float2(0.5, -0.5) + 0.5;
  return output;
}

float4 PSMain(VsOut input) : SV_TARGET {
  float x = input.uv.x;
  float y = input.uv.y;
  float grid = (step(0.985, frac(x * 16.0)) + step(0.985, frac(y * 9.0))) * 0.10;

  if (x < 0.25) {
    return float4(0.04 + grid, 0.04 + grid, 0.04 + grid, 1.0);
  }
  if (x < 0.50) {
    return float4(1.0, 1.0, 1.0, 1.0);
  }
  if (x < 0.75) {
    return float4(4.0, 4.0, 4.0, 1.0);
  }
  return float4(0.0, 8.0, 0.0, 1.0);
}
)";

std::string HrString(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << std::setw(8)
         << std::setfill('0') << static_cast<unsigned long>(hr);
  return stream.str();
}

std::string NarrowFromWide(const wchar_t* text) {
  if (!text || text[0] == L'\0') {
    return {};
  }
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, text, -1, &result[0], size, nullptr, nullptr);
  return result;
}

std::string PtrString(const void* pointer) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase
         << reinterpret_cast<uintptr_t>(pointer);
  return stream.str();
}

std::string ColorSpaceName(DXGI_COLOR_SPACE_TYPE color_space) {
  switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
      return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
      return "RGB_FULL_G10_NONE_P709(scRGB)";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
      return "RGB_STUDIO_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
      return "RGB_STUDIO_G22_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
      return "RGB_FULL_G2084_NONE_P2020(PQ)";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
      return "RGB_STUDIO_G2084_NONE_P2020(PQ)";
    default: {
      std::ostringstream stream;
      stream << "UNKNOWN(" << static_cast<int>(color_space) << ")";
      return stream.str();
    }
  }
}

void ProbeLog(const std::string& message) {
  const std::string line = "[DCompHDR] " + message;
  spdlog::info("{}", line);
  OutputDebugStringA((line + "\n").c_str());

  std::ofstream file("dcomp_hdr_probe.log", std::ios::app);
  if (file.is_open()) {
    file << line << "\n";
  }
}

void ResetProbeLog() {
  std::ofstream file("dcomp_hdr_probe.log", std::ios::trunc);
  if (file.is_open()) {
    file << "[DCompHDR] new probe session\n";
  }
}

void LogOutputInfo(HWND hwnd, IDXGIAdapter* adapter) {
  if (!adapter) {
    ProbeLog("DXGI adapter missing; cannot query HDR output metadata.");
    return;
  }

  const HMONITOR window_monitor =
      MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  ProbeLog("window monitor=" + PtrString(window_monitor));

  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    HRESULT hr = adapter->EnumOutputs(index, &output);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(hr) || !output) {
      ProbeLog("EnumOutputs(" + std::to_string(index) + ") failed hr=" +
               HrString(hr));
      break;
    }

    DXGI_OUTPUT_DESC desc{};
    hr = output->GetDesc(&desc);
    if (FAILED(hr)) {
      ProbeLog("IDXGIOutput::GetDesc failed hr=" + HrString(hr));
      continue;
    }

    const bool is_window_output = desc.Monitor == window_monitor;
    std::ostringstream output_stream;
    output_stream << "output[" << index << "] name='"
                  << NarrowFromWide(desc.DeviceName)
                  << "' monitor=" << PtrString(desc.Monitor)
                  << (is_window_output ? " MATCH" : "");
    ProbeLog(output_stream.str());

    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    hr = output.As(&output6);
    if (FAILED(hr) || !output6) {
      ProbeLog("output[" + std::to_string(index) +
               "] does not expose IDXGIOutput6 hr=" + HrString(hr));
      continue;
    }

    DXGI_OUTPUT_DESC1 desc1{};
    hr = output6->GetDesc1(&desc1);
    if (FAILED(hr)) {
      ProbeLog("IDXGIOutput6::GetDesc1 failed hr=" + HrString(hr));
      continue;
    }

    std::ostringstream hdr_stream;
    hdr_stream << "output[" << index << "] ColorSpace="
               << ColorSpaceName(desc1.ColorSpace)
               << " MinLuminance=" << desc1.MinLuminance
               << " MaxLuminance=" << desc1.MaxLuminance
               << " MaxFullFrameLuminance="
               << desc1.MaxFullFrameLuminance;
    ProbeLog(hdr_stream.str());
  }
}

}  // namespace

DCompAlphaProbe::DCompAlphaProbe() = default;

DCompAlphaProbe::~DCompAlphaProbe() {
  Shutdown();
}

bool DCompAlphaProbe::Initialize(HWND target_hwnd,
                                 HWND timer_hwnd,
                                 bool topmost) {
  ResetProbeLog();
  hwnd_ = target_hwnd;
  timer_hwnd_ = timer_hwnd ? timer_hwnd : target_hwnd;
  if (!hwnd_) {
    ProbeLog("Initialize failed: target HWND is null.");
    return false;
  }

  RECT rect{};
  GetClientRect(hwnd_, &rect);
  UpdateVisualRect(rect);
  ProbeLog("Initialize target_hwnd=" + PtrString(hwnd_) +
           " timer_hwnd=" + PtrString(timer_hwnd_) +
           " topmost=" + std::to_string(topmost ? 1 : 0) +
           " visual=" + std::to_string(width_) + "x" +
           std::to_string(height_) + " offset=" + std::to_string(offset_x_) +
           "," + std::to_string(offset_y_));

  if (!CreateDevice() || !CreateShaders() || !CreateSwapChain(width_, height_) ||
      !CreateComposition(hwnd_, topmost)) {
    ProbeLog("Initialize failed; shutting down probe.");
    Shutdown();
    return false;
  }

  Render();
  SetTimer(timer_hwnd_, kProbeTimerId, kProbeTimerMs, nullptr);
  ProbeLog("Initialize succeeded; timer armed.");
  return true;
}

void DCompAlphaProbe::Shutdown() {
  ProbeLog("Shutdown.");
  if (timer_hwnd_) {
    KillTimer(timer_hwnd_, kProbeTimerId);
  }
  rtv_.Reset();
  swap_chain_.Reset();
  pixel_shader_.Reset();
  vertex_shader_.Reset();
  video_visual_.Reset();
  root_visual_.Reset();
  dcomp_target_.Reset();
  dcomp_device_.Reset();
  dxgi_factory_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
  hwnd_ = nullptr;
  timer_hwnd_ = nullptr;
}

void DCompAlphaProbe::Resize(const RECT& client_rect) {
  if (!swap_chain_) {
    UpdateVisualRect(client_rect);
    return;
  }

  UpdateVisualRect(client_rect);
  rtv_.Reset();
  HRESULT hr = swap_chain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr)) {
    ProbeLog("ResizeBuffers failed " + std::to_string(width_) + "x" +
             std::to_string(height_) + " hr=" + HrString(hr));
    return;
  }
  ProbeLog("ResizeBuffers succeeded " + std::to_string(width_) + "x" +
           std::to_string(height_) + " offset=" + std::to_string(offset_x_) +
           "," + std::to_string(offset_y_));

  if (video_visual_) {
    video_visual_->SetOffsetX(static_cast<float>(offset_x_));
    video_visual_->SetOffsetY(static_cast<float>(offset_y_));
  }
  Render();
}

void DCompAlphaProbe::Render() {
  if (!d3d_device_ || !d3d_context_ || !swap_chain_) {
    return;
  }

  if (!rtv_) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr) || !back_buffer) {
      ProbeLog("GetBuffer failed hr=" + HrString(hr));
      return;
    }
    hr = d3d_device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
      ProbeLog("CreateRenderTargetView failed hr=" + HrString(hr));
      return;
    }
    ProbeLog("CreateRenderTargetView succeeded.");
  }

  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(width_);
  viewport.Height = static_cast<float>(height_);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  d3d_context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
  d3d_context_->RSSetViewports(1, &viewport);
  d3d_context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d3d_context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  d3d_context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  d3d_context_->Draw(3, 0);

  const HRESULT present_hr = swap_chain_->Present(1, 0);
  if (FAILED(present_hr)) {
    ProbeLog("Present failed hr=" + HrString(present_hr));
  }
  if (dcomp_device_) {
    const HRESULT hr = dcomp_device_->Commit();
    if (FAILED(hr)) {
      ProbeLog("DComposition Commit failed hr=" + HrString(hr));
    }
  }
  color_flip_ = !color_flip_;
}

bool DCompAlphaProbe::CreateDevice() {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL level{};
  HRESULT hr = D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 flags,
                                 levels,
                                 ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION,
                                 &d3d_device_,
                                 &level,
                                 &d3d_context_);
  ProbeLog("D3D11CreateDevice(debug=" +
           std::to_string((flags & D3D11_CREATE_DEVICE_DEBUG) ? 1 : 0) +
           ") hr=" + HrString(hr));
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr,
                           D3D_DRIVER_TYPE_HARDWARE,
                           nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels,
                           ARRAYSIZE(levels),
                           D3D11_SDK_VERSION,
                           &d3d_device_,
                           &level,
                           &d3d_context_);
    ProbeLog("D3D11CreateDevice fallback hr=" + HrString(hr));
  }
  if (FAILED(hr) || !d3d_device_) {
    return false;
  }
  ProbeLog("D3D feature level=" + HrString(static_cast<HRESULT>(level)));

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  hr = d3d_device_.As(&dxgi_device);
  if (FAILED(hr) || !dxgi_device) {
    ProbeLog("Query IDXGIDevice failed hr=" + HrString(hr));
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  hr = dxgi_device->GetAdapter(&adapter);
  if (FAILED(hr) || !adapter) {
    ProbeLog("IDXGIDevice::GetAdapter failed hr=" + HrString(hr));
    return false;
  }
  DXGI_ADAPTER_DESC adapter_desc{};
  if (SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
    ProbeLog("adapter='" + NarrowFromWide(adapter_desc.Description) +
             "' vendor=" +
             HrString(static_cast<HRESULT>(adapter_desc.VendorId)) +
             " device=" +
             HrString(static_cast<HRESULT>(adapter_desc.DeviceId)));
  }
  LogOutputInfo(hwnd_, adapter.Get());

  Microsoft::WRL::ComPtr<IDXGIFactory> factory;
  hr = adapter->GetParent(IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) {
    ProbeLog("IDXGIAdapter::GetParent factory failed hr=" + HrString(hr));
    return false;
  }
  hr = factory.As(&dxgi_factory_);
  ProbeLog("Query IDXGIFactory2 hr=" + HrString(hr));
  return SUCCEEDED(hr) && dxgi_factory_;
}

bool DCompAlphaProbe::CreateShaders() {
  Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

  HRESULT hr = D3DCompile(kHdrProbeShader,
                          std::strlen(kHdrProbeShader),
                          nullptr,
                          nullptr,
                          nullptr,
                          "VSMain",
                          "vs_5_0",
                          flags,
                          0,
                          &vs_blob,
                          &errors);
  if (FAILED(hr) || !vs_blob) {
    std::string error_text =
        errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                             errors->GetBufferSize())
               : std::string();
    ProbeLog("Compile VSMain failed hr=" + HrString(hr) + " " + error_text);
    return false;
  }

  hr = D3DCompile(kHdrProbeShader,
                  std::strlen(kHdrProbeShader),
                  nullptr,
                  nullptr,
                  nullptr,
                  "PSMain",
                  "ps_5_0",
                  flags,
                  0,
                  &ps_blob,
                  &errors);
  if (FAILED(hr) || !ps_blob) {
    std::string error_text =
        errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                             errors->GetBufferSize())
               : std::string();
    ProbeLog("Compile PSMain failed hr=" + HrString(hr) + " " + error_text);
    return false;
  }

  hr = d3d_device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                       vs_blob->GetBufferSize(),
                                       nullptr,
                                       &vertex_shader_);
  if (FAILED(hr) || !vertex_shader_) {
    ProbeLog("CreateVertexShader failed hr=" + HrString(hr));
    return false;
  }

  hr = d3d_device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                      ps_blob->GetBufferSize(),
                                      nullptr,
                                      &pixel_shader_);
  if (FAILED(hr) || !pixel_shader_) {
    ProbeLog("CreatePixelShader failed hr=" + HrString(hr));
  } else {
    ProbeLog("HDR probe shaders created.");
  }
  return SUCCEEDED(hr) && pixel_shader_;
}

bool DCompAlphaProbe::CreateSwapChain(int width, int height) {
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  ProbeLog("CreateSwapChainForComposition request format=R16G16B16A16_FLOAT "
           "buffers=2 alpha=IGNORE size=" +
           std::to_string(width) + "x" + std::to_string(height));
  ProbeLog("shader bands: dark grid, SDR white=1.0, HDR white=4.0, "
           "HDR green=8.0 scRGB.");
  HRESULT hr = dxgi_factory_->CreateSwapChainForComposition(
      d3d_device_.Get(), &desc, nullptr, &swap_chain_);
  if (FAILED(hr) || !swap_chain_) {
    ProbeLog("CreateSwapChainForComposition failed hr=" + HrString(hr));
    return false;
  }
  ProbeLog("CreateSwapChainForComposition succeeded.");

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain3;
  if (SUCCEEDED(swap_chain_.As(&swap_chain3)) && swap_chain3) {
    UINT color_space_support = 0;
    hr = swap_chain3->CheckColorSpaceSupport(
        kScRgbColorSpace, &color_space_support);
    ProbeLog("CheckColorSpaceSupport " + ColorSpaceName(kScRgbColorSpace) +
             " hr=" + HrString(hr) + " support=" +
             HrString(static_cast<HRESULT>(color_space_support)));
    if (SUCCEEDED(hr) &&
        (color_space_support &
         DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) {
      hr = swap_chain3->SetColorSpace1(kScRgbColorSpace);
      ProbeLog("SetColorSpace1 " + ColorSpaceName(kScRgbColorSpace) +
               " hr=" + HrString(hr));
    } else {
      ProbeLog("scRGB PRESENT support is missing; values may be SDR-mapped.");
    }
  } else {
    ProbeLog("Swapchain does not expose IDXGISwapChain3.");
  }

  return true;
}

bool DCompAlphaProbe::CreateComposition(HWND target_hwnd, bool topmost) {
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  HRESULT hr = d3d_device_.As(&dxgi_device);
  if (FAILED(hr) || !dxgi_device) {
    ProbeLog("CreateComposition Query IDXGIDevice failed hr=" + HrString(hr));
    return false;
  }

  hr = DCompositionCreateDevice(
      dxgi_device.Get(), IID_PPV_ARGS(&dcomp_device_));
  if (FAILED(hr) || !dcomp_device_) {
    ProbeLog("DCompositionCreateDevice failed hr=" + HrString(hr));
    return false;
  }
  ProbeLog("DCompositionCreateDevice succeeded.");

  // Bind to the outer runner HWND. With topmost=false, this visual sits behind
  // child windows, so the Flutter child HWND must really carry alpha for the
  // probe to show through.
  hr = dcomp_device_->CreateTargetForHwnd(
      target_hwnd, topmost ? TRUE : FALSE, &dcomp_target_);
  if (FAILED(hr) || !dcomp_target_) {
    ProbeLog("CreateTargetForHwnd failed hr=" + HrString(hr));
    return false;
  }
  ProbeLog("CreateTargetForHwnd target=" + PtrString(target_hwnd) +
           " topmost=" + std::to_string(topmost ? 1 : 0));

  hr = dcomp_device_->CreateVisual(&root_visual_);
  if (FAILED(hr) || !root_visual_) {
    ProbeLog("Create root visual failed hr=" + HrString(hr));
    return false;
  }

  hr = dcomp_device_->CreateVisual(&video_visual_);
  if (FAILED(hr) || !video_visual_) {
    ProbeLog("Create video visual failed hr=" + HrString(hr));
    return false;
  }

  video_visual_->SetContent(swap_chain_.Get());
  video_visual_->SetOffsetX(static_cast<float>(offset_x_));
  video_visual_->SetOffsetY(static_cast<float>(offset_y_));
  root_visual_->AddVisual(video_visual_.Get(), FALSE, nullptr);
  dcomp_target_->SetRoot(root_visual_.Get());
  hr = dcomp_device_->Commit();
  ProbeLog("Initial DComposition Commit hr=" + HrString(hr));
  return SUCCEEDED(hr);
}

void DCompAlphaProbe::UpdateVisualRect(const RECT& client_rect) {
  const double dpi_scale =
      std::max(1.0, FlutterDesktopGetDpiForHWND(hwnd_) / 96.0);
  const int client_width_physical =
      std::max(1, static_cast<int>(client_rect.right - client_rect.left));
  const int client_height_physical =
      std::max(1, static_cast<int>(client_rect.bottom - client_rect.top));
  const int client_width_logical =
      std::max(1, static_cast<int>(client_width_physical / dpi_scale));
  const int client_height_logical =
      std::max(1, static_cast<int>(client_height_physical / dpi_scale));

  const int width_logical =
      std::max(120, std::min(640, client_width_logical - 96));
  const int height_logical =
      std::max(90, std::min(360, client_height_logical - 150));
  const int offset_x_logical =
      std::max(24, (client_width_logical - width_logical) / 2);
  const int offset_y_logical =
      std::max(72, (client_height_logical - height_logical) / 2);

  width_ = std::max(1, static_cast<int>(width_logical * dpi_scale));
  height_ = std::max(1, static_cast<int>(height_logical * dpi_scale));
  offset_x_ = std::max(0, static_cast<int>(offset_x_logical * dpi_scale));
  offset_y_ = std::max(0, static_cast<int>(offset_y_logical * dpi_scale));
}
