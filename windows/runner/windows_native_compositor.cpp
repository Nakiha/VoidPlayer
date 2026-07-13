#include "windows_native_compositor.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace {

constexpr char kCompositeShader[] = R"hlsl(
Texture2D flutter_surface : register(t0);
Texture2D video_surface : register(t1);
SamplerState surface_sampler : register(s0);
cbuffer CompositeConstants : register(b0) {
  uint has_video;
  float3 padding;
};
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VertexOutput VSMain(uint vertex_id : SV_VertexID) {
  VertexOutput output;
  float2 position = vertex_id == 0 ? float2(-1.0, -1.0) :
                    vertex_id == 1 ? float2(-1.0, 3.0) : float2(3.0, -1.0);
  output.position = float4(position, 0.0, 1.0);
  output.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
  return output;
}
float4 PSMain(VertexOutput input) : SV_TARGET {
  float4 video = has_video != 0
      ? video_surface.Sample(surface_sampler, input.uv)
      : float4(0.0, 0.0, 0.0, 1.0);
  float4 flutter = flutter_surface.Sample(surface_sampler, input.uv);
  return float4(flutter.rgb + video.rgb * (1.0 - flutter.a), 1.0);
}
)hlsl";

struct CompositeConstants {
  uint32_t has_video = 0;
  float padding[3] = {};
};

}  // namespace

WindowsNativeCompositor::WindowsNativeCompositor(
    HWND top_level_window,
    FlutterDesktopViewRef flutter_view)
    : top_level_window_(top_level_window), flutter_view_(flutter_view) {}

WindowsNativeCompositor::~WindowsNativeCompositor() {
  Stop();
}

bool WindowsNativeCompositor::Start() {
  if (running_ || !top_level_window_ || !flutter_view_) {
    return false;
  }
  wake_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  stop_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  ready_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!wake_event_ || !stop_event_ || !ready_event_) {
    Stop();
    return false;
  }
  running_ = true;
  initialization_succeeded_ = false;
  thread_ = std::thread(&WindowsNativeCompositor::Run, this);
  if (WaitForSingleObject(ready_event_, 10000) != WAIT_OBJECT_0 ||
      !initialization_succeeded_) {
    Stop();
    return false;
  }
  return true;
}

void WindowsNativeCompositor::Stop() {
  if (flutter_view_) {
    FlutterDesktopViewSetSurfacePublishedCallback(flutter_view_, nullptr, nullptr);
  }
  if (stop_event_) {
    SetEvent(stop_event_);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  running_ = false;
  if (ready_event_) {
    CloseHandle(ready_event_);
    ready_event_ = nullptr;
  }
  if (wake_event_) {
    CloseHandle(wake_event_);
    wake_event_ = nullptr;
  }
  if (stop_event_) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
}

void WindowsNativeCompositor::NotifyResize() {
  resize_pending_ = true;
  SignalComposite();
}

void WindowsNativeCompositor::PublishVideoTarget(ID3D11Texture2D* texture) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_target_ = texture;
    video_srv_.Reset();
  }
  SignalComposite();
}

WindowsNativeCompositorDiagnostics WindowsNativeCompositor::diagnostics() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return diagnostics_;
}

void WindowsNativeCompositor::OnFlutterSurfacePublished(
    FlutterDesktopViewRef view,
    uint64_t frame_generation,
    void* user_data) {
  (void)view;
  auto* compositor = static_cast<WindowsNativeCompositor*>(user_data);
  if (compositor) {
    compositor->SignalComposite(frame_generation);
  }
}

void WindowsNativeCompositor::SignalComposite(uint64_t flutter_generation) {
  if (flutter_generation != 0) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++diagnostics_.flutter_publish_count;
  }
  if (wake_event_) {
    SetEvent(wake_event_);
  }
}

void WindowsNativeCompositor::Run() {
  const bool initialized = InitializeOnThread();
  initialization_succeeded_ = initialized;
  SetEvent(ready_event_);
  if (!initialized) {
    ShutdownOnThread();
    running_ = false;
    return;
  }
  const HANDLE events[] = {stop_event_, wake_event_};
  while (running_) {
    const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait != WAIT_OBJECT_0 + 1) {
      SetError("DComp compositor wait failed");
      break;
    }
    if (resize_pending_.exchange(false) && !ResizeSwapChain()) {
      continue;
    }
    (void)CompositeLatest();
  }
  ShutdownOnThread();
  running_ = false;
}

bool WindowsNativeCompositor::InitializeOnThread() {
  RECT client = {};
  if (!GetClientRect(top_level_window_, &client)) {
    SetError("DComp compositor could not read client bounds");
    return false;
  }
  width_ = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
  height_ = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  adapter.Attach(FlutterDesktopViewGetGraphicsAdapter(flutter_view_));
  const D3D_DRIVER_TYPE driver_type = adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                              : D3D_DRIVER_TYPE_HARDWARE;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                     D3D_FEATURE_LEVEL_11_0};
  HRESULT result = D3D11CreateDevice(adapter.Get(),
                                     driver_type,
                                     nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     levels,
                                     ARRAYSIZE(levels),
                                     D3D11_SDK_VERSION,
                                     &device_,
                                     nullptr,
                                     &context_);
  if (FAILED(result) || !device_ || !context_) {
    SetError("DComp compositor could not create its D3D11 device");
    return false;
  }
  if (!CreatePipeline()) {
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> device_adapter;
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
  if (FAILED(device_.As(&dxgi_device)) ||
      FAILED(dxgi_device->GetAdapter(&device_adapter)) ||
      FAILED(device_adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    SetError("DComp compositor could not resolve its DXGI factory");
    return false;
  }
  DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
  swap_desc.Width = width_;
  swap_desc.Height = height_;
  swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_desc.SampleDesc.Count = 1;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.BufferCount = 3;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  swap_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain;
  result = factory->CreateSwapChainForComposition(
      device_.Get(), &swap_desc, nullptr, &swap_chain);
  if (FAILED(result) || FAILED(swap_chain.As(&swap_chain_))) {
    SetError("DComp compositor could not create its swap chain");
    return false;
  }
  result = DCompositionCreateDevice(
      dxgi_device.Get(), IID_PPV_ARGS(&dcomp_device_));
  if (FAILED(result) ||
      FAILED(dcomp_device_->CreateTargetForHwnd(
          top_level_window_, TRUE, &dcomp_target_)) ||
      FAILED(dcomp_device_->CreateVisual(&dcomp_visual_)) ||
      FAILED(dcomp_visual_->SetContent(swap_chain_.Get())) ||
      FAILED(dcomp_target_->SetRoot(dcomp_visual_.Get())) ||
      FAILED(dcomp_device_->Commit())) {
    SetError("DComp compositor could not attach its top-level visual");
    return false;
  }
  FlutterDesktopViewSetSurfacePublishedCallback(
      flutter_view_, &WindowsNativeCompositor::OnFlutterSurfacePublished, this);
  if (!FlutterDesktopViewSetSurfaceExportMode(
          flutter_view_,
          kFlutterDesktopWindowsSurfaceExportModeCompositorOwned)) {
    SetError("Flutter surface export could not enter compositor-owned mode");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    diagnostics_.initialized = true;
    diagnostics_.flutter_export_enabled = true;
    diagnostics_.last_error.clear();
  }
  return true;
}

void WindowsNativeCompositor::ShutdownOnThread() {
  if (flutter_view_) {
    FlutterDesktopViewSetSurfaceExportMode(
        flutter_view_, kFlutterDesktopWindowsSurfaceExportModeDisabled);
  }
  if (dcomp_target_) {
    dcomp_target_->SetRoot(nullptr);
  }
  if (dcomp_device_) {
    dcomp_device_->Commit();
  }
  video_srv_.Reset();
  video_target_.Reset();
  completion_query_.Reset();
  constants_.Reset();
  sampler_.Reset();
  pixel_shader_.Reset();
  vertex_shader_.Reset();
  dcomp_visual_.Reset();
  dcomp_target_.Reset();
  dcomp_device_.Reset();
  swap_chain_.Reset();
  context_.Reset();
  device_.Reset();
  std::lock_guard<std::mutex> lock(state_mutex_);
  diagnostics_.initialized = false;
  diagnostics_.flutter_export_enabled = false;
}

bool WindowsNativeCompositor::ResizeSwapChain() {
  if (!swap_chain_) {
    return false;
  }
  RECT client = {};
  if (!GetClientRect(top_level_window_, &client)) {
    SetError("DComp compositor resize could not read client bounds");
    return false;
  }
  const uint32_t width =
      static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
  const uint32_t height =
      static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
  if (width == width_ && height == height_) {
    return true;
  }
  const HRESULT result = swap_chain_->ResizeBuffers(
      3, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
  if (FAILED(result)) {
    SetError("DComp compositor swap-chain resize failed");
    return false;
  }
  width_ = width;
  height_ = height;
  return true;
}

bool WindowsNativeCompositor::CompositeLatest() {
  FlutterDesktopWindowsSurface lease = {};
  lease.struct_size = sizeof(lease);
  if (!FlutterDesktopViewAcquireLatestSurface(flutter_view_, &lease)) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++diagnostics_.acquire_failure_count;
    diagnostics_.last_composite_succeeded = false;
    return false;
  }
  const auto release_lease = [this, lease]() {
    FlutterDesktopViewReleaseSurface(flutter_view_, lease.lease_id);
  };
  if (!lease.shared_texture_handle ||
      lease.format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      lease.alpha_mode !=
          kFlutterDesktopWindowsSurfaceAlphaModePremultiplied) {
    release_lease();
    SetError("Flutter surface lease has an unsupported format or alpha mode");
    return false;
  }
  if (lease.width > 0 && lease.height > 0 &&
      (lease.width != width_ || lease.height != height_)) {
    const HRESULT resize = swap_chain_->ResizeBuffers(
        3,
        lease.width,
        lease.height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        0);
    if (FAILED(resize)) {
      release_lease();
      SetError("DComp compositor could not match the Flutter surface size");
      return false;
    }
    width_ = lease.width;
    height_ = lease.height;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> flutter_texture;
  Microsoft::WRL::ComPtr<ID3D11Device1> device1;
  const HRESULT device1_result = device_.As(&device1);
  const HRESULT open_result = SUCCEEDED(device1_result)
      ? device1->OpenSharedResource1(
            lease.shared_texture_handle, IID_PPV_ARGS(&flutter_texture))
      : device1_result;
  if (FAILED(open_result) ||
      !flutter_texture) {
    release_lease();
    SetError("DComp compositor could not open the Flutter NT-handle surface: " +
             std::to_string(static_cast<uint32_t>(open_result)));
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
  if (FAILED(flutter_texture.As(&keyed_mutex)) ||
      FAILED(keyed_mutex->AcquireSync(lease.consumer_acquire_key, 16))) {
    release_lease();
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++diagnostics_.keyed_mutex_failure_count;
    diagnostics_.last_composite_succeeded = false;
    return false;
  }
  bool acquired_mutex = true;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flutter_srv;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  bool success = SUCCEEDED(device_->CreateShaderResourceView(
      flutter_texture.Get(), nullptr, &flutter_srv));
  success = success && SUCCEEDED(swap_chain_->GetBuffer(
      0, IID_PPV_ARGS(&back_buffer)));
  success = success && SUCCEEDED(device_->CreateRenderTargetView(
      back_buffer.Get(), nullptr, &target));

  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (video_target_ && !video_srv_) {
      device_->CreateShaderResourceView(video_target_.Get(), nullptr, &video_srv_);
    }
    video_srv = video_srv_;
  }
  if (success) {
    ID3D11RenderTargetView* target_pointer = target.Get();
    context_->OMSetRenderTargets(1, &target_pointer, nullptr);
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    CompositeConstants constants;
    constants.has_video = video_srv ? 1u : 0u;
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11Buffer* constant_pointer = constants_.Get();
    context_->PSSetConstantBuffers(0, 1, &constant_pointer);
    ID3D11SamplerState* sampler_pointer = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler_pointer);
    ID3D11ShaderResourceView* resources[] = {
        flutter_srv.Get(), video_srv.Get()};
    context_->PSSetShaderResources(0, 2, resources);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty[] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, empty);
    success = WaitForGpu();
  }
  if (acquired_mutex) {
    keyed_mutex->ReleaseSync(lease.producer_release_key);
  }
  release_lease();
  if (success) {
    const HRESULT present = swap_chain_->Present(1, 0);
    success = SUCCEEDED(present);
    if (!success) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++diagnostics_.present_failure_count;
      diagnostics_.last_error = "DComp compositor swap-chain present failed";
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    diagnostics_.last_composite_succeeded = success;
    diagnostics_.last_flutter_frame_generation = lease.frame_generation;
    if (success) {
      ++diagnostics_.composite_count;
      diagnostics_.last_error.clear();
    }
  }
  return success;
}

bool WindowsNativeCompositor::CreatePipeline() {
  Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT result = D3DCompile(kCompositeShader,
                              sizeof(kCompositeShader) - 1,
                              "voidplayer_windows_compositor",
                              nullptr,
                              nullptr,
                              "VSMain",
                              "vs_5_0",
                              D3DCOMPILE_ENABLE_STRICTNESS,
                              0,
                              &vertex_blob,
                              &errors);
  if (FAILED(result)) {
    SetError("DComp compositor vertex shader compilation failed");
    return false;
  }
  result = D3DCompile(kCompositeShader,
                      sizeof(kCompositeShader) - 1,
                      "voidplayer_windows_compositor",
                      nullptr,
                      nullptr,
                      "PSMain",
                      "ps_5_0",
                      D3DCOMPILE_ENABLE_STRICTNESS,
                      0,
                      &pixel_blob,
                      &errors);
  if (FAILED(result) ||
      FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                         vertex_blob->GetBufferSize(),
                                         nullptr,
                                         &vertex_shader_)) ||
      FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                        pixel_blob->GetBufferSize(),
                                        nullptr,
                                        &pixel_shader_))) {
    SetError("DComp compositor shader creation failed");
    return false;
  }
  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_))) {
    SetError("DComp compositor sampler creation failed");
    return false;
  }
  D3D11_BUFFER_DESC constant_desc = {};
  constant_desc.ByteWidth = sizeof(CompositeConstants);
  constant_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(device_->CreateBuffer(&constant_desc, nullptr, &constants_))) {
    SetError("DComp compositor constant-buffer creation failed");
    return false;
  }
  D3D11_QUERY_DESC query_desc = {};
  query_desc.Query = D3D11_QUERY_EVENT;
  if (FAILED(device_->CreateQuery(&query_desc, &completion_query_))) {
    SetError("DComp compositor completion-query creation failed");
    return false;
  }
  return true;
}

bool WindowsNativeCompositor::WaitForGpu() {
  context_->End(completion_query_.Get());
  context_->Flush();
  const auto start = std::chrono::steady_clock::now();
  HRESULT result = S_FALSE;
  while ((result = context_->GetData(
              completion_query_.Get(), nullptr, 0, 0)) == S_FALSE) {
    if (std::chrono::steady_clock::now() - start >
        std::chrono::milliseconds(100)) {
      SetError("DComp compositor GPU completion timed out");
      return false;
    }
    SwitchToThread();
  }
  if (FAILED(result)) {
    SetError("DComp compositor GPU completion query failed");
    return false;
  }
  return true;
}

void WindowsNativeCompositor::SetError(std::string error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  diagnostics_.last_error = std::move(error);
  diagnostics_.last_composite_succeeded = false;
}
