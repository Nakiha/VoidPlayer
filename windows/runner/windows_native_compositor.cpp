#include "windows_native_compositor.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "windows/presentation/windows_compositor_viewport_handoff.h"

namespace {

constexpr char kCompositeShader[] = R"hlsl(
Texture2D flutter_surface : register(t0);
Texture2D video_surface : register(t1);
SamplerState surface_sampler : register(s0);
cbuffer CompositeConstants : register(b0) {
  uint has_video;
  uint output_scrgb;
  uint video_is_sdr;
  float sdr_white_scale;
  float4 video_rect;
  float4 background_color;
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
float srgb_channel_to_linear(float value) {
  float c = saturate(value);
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 srgb_to_linear(float3 color) {
  return float3(srgb_channel_to_linear(color.r),
                srgb_channel_to_linear(color.g),
                srgb_channel_to_linear(color.b));
}
float linear_channel_to_srgb(float value) {
  float c = saturate(value);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 linear_to_srgb(float3 color) {
  return float3(linear_channel_to_srgb(color.r),
                linear_channel_to_srgb(color.g),
                linear_channel_to_srgb(color.b));
}
float3 premultiplied_srgb_to_scrgb(float3 color, float alpha) {
  if (alpha <= 0.0001) {
    return float3(0.0, 0.0, 0.0);
  }
  float3 straight = saturate(color / alpha);
  return srgb_to_linear(straight) * alpha * sdr_white_scale;
}
float4 PSMain(VertexOutput input) : SV_TARGET {
  float4 video = float4(saturate(background_color.rgb), 1.0);
  if (output_scrgb != 0) {
    video.rgb = srgb_to_linear(video.rgb) * sdr_white_scale;
  }
  float2 video_end = video_rect.xy + video_rect.zw;
  if (has_video != 0 &&
      input.uv.x >= video_rect.x && input.uv.x <= video_end.x &&
      input.uv.y >= video_rect.y && input.uv.y <= video_end.y) {
    float2 video_uv = (input.uv - video_rect.xy) / max(video_rect.zw, 1e-6);
    video = video_surface.Sample(surface_sampler, video_uv);
    if (output_scrgb != 0 && video_is_sdr != 0) {
      video.rgb = srgb_to_linear(video.rgb) * sdr_white_scale;
    } else if (output_scrgb == 0 && video_is_sdr == 0) {
      // A target from the immediately retired FP16 ring can complete while an
      // HDR -> SDR reconfiguration is draining. Preserve that completion in
      // the SDR domain instead of presenting unclamped linear values.
      video.rgb = linear_to_srgb(video.rgb / max(sdr_white_scale, 1e-6));
    }
  }
  float4 flutter = flutter_surface.Sample(surface_sampler, input.uv);
  if (output_scrgb != 0) {
    flutter.rgb = premultiplied_srgb_to_scrgb(
        flutter.rgb, saturate(flutter.a));
  }
  return float4(flutter.rgb + video.rgb * (1.0 - flutter.a), 1.0);
}
)hlsl";

struct CompositeConstants {
  uint32_t has_video = 0;
  uint32_t output_scrgb = 0;
  uint32_t video_is_sdr = 1;
  float sdr_white_scale = 1.0f;
  float video_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float background_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

class ScopedD3D11ContextLock final {
 public:
  explicit ScopedD3D11ContextLock(ID3D10Multithread* multithread)
      : multithread_(multithread) {
    if (multithread_) {
      multithread_->Enter();
    }
  }
  ~ScopedD3D11ContextLock() {
    if (multithread_) {
      multithread_->Leave();
    }
  }

 private:
  ID3D10Multithread* multithread_ = nullptr;
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

bool WindowsNativeCompositor::ConfigureOutput(
    const WindowsNativeCompositorOutputConfig& config, std::string& error) {
  error.clear();
  if (!std::isfinite(config.sdr_white_level_nits) ||
      config.sdr_white_level_nits <= 0.0 ||
      config.sdr_white_level_nits > 10000.0) {
    error = "Windows compositor SDR white level is invalid";
    return false;
  }
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!running_) {
      error = "Windows compositor is not running";
      return false;
    }
    requested_output_config_ = config;
    generation = ++requested_output_generation_;
  }
  SignalComposite();
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (!state_condition_.wait_for(
          lock, std::chrono::seconds(5), [this, generation]() {
            return completed_output_generation_ >= generation || !running_;
          })) {
    error = "Windows compositor output reconfiguration timed out";
    return false;
  }
  if (completed_output_generation_ < generation ||
      !completed_output_succeeded_) {
    error = diagnostics_.last_error.empty()
                ? "Windows compositor output reconfiguration failed"
                : diagnostics_.last_error;
    return false;
  }
  return true;
}

bool WindowsNativeCompositor::CreateVideoTargetRing(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    size_t target_count,
    std::vector<void*>& textures) {
  textures.clear();
  if (!device_ || width == 0 || height == 0 || target_count < 3 ||
      target_count > 8 ||
      (format != DXGI_FORMAT_B8G8R8A8_UNORM &&
       format != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
    SetError("Windows video target ring request is invalid");
    return false;
  }
  std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> targets;
  targets.reserve(target_count);
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  for (size_t index = 0; index < target_count; ++index) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &target)) || !target) {
      SetError("Windows video target ring allocation failed");
      return false;
    }
    targets.push_back(std::move(target));
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const bool retiring_current_ring = !video_targets_.empty();
    const bool retaining_displayed_target = video_target_ != nullptr;
    D3D11_TEXTURE2D_DESC retained_desc = {};
    if (retaining_displayed_target) {
      video_target_->GetDesc(&retained_desc);
    }
    retired_video_targets_.insert(retired_video_targets_.end(),
                                  video_targets_.begin(), video_targets_.end());
    constexpr size_t kMaxRetiredVideoTargets = 16;
    if (retired_video_targets_.size() > kMaxRetiredVideoTargets) {
      retired_video_targets_.erase(
          retired_video_targets_.begin(),
          retired_video_targets_.begin() +
              (retired_video_targets_.size() - kMaxRetiredVideoTargets));
    }
    video_targets_ = std::move(targets);
    if (retiring_current_ring) {
      // A ring can be replaced before the first video target is displayed.
      // Keep it alive until the first successful presentation from the new
      // ring, but do not require a displayed-target geometry handoff merely
      // to release those startup resources.
      video_target_retirement_serial_ = 0;
    }
    if (retaining_displayed_target) {
      video_target_handoff_pending_ = true;
      video_target_handoff_serial_ = 0;
      ++diagnostics_.video_target_retained_reconfigure_count;
    }
    diagnostics_.video_target_format =
        format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "rgba16f" : "bgra8";
    ++diagnostics_.video_target_generation;
    for (const auto& target : video_targets_) {
      textures.push_back(target.Get());
    }
    spdlog::info(
        "[WindowsCompositor] target ring generation={} next={}x{} format={} "
        "retained={} retained_size={}x{}",
        diagnostics_.video_target_generation, width, height,
        diagnostics_.video_target_format, retaining_displayed_target,
        retained_desc.Width, retained_desc.Height);
  }
  return true;
}

void WindowsNativeCompositor::ClearVideoTargetRing() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  video_srv_.Reset();
  video_target_.Reset();
  video_targets_.clear();
  retired_video_targets_.clear();
  video_target_format_ = DXGI_FORMAT_UNKNOWN;
  video_target_handoff_pending_ = false;
  video_target_handoff_serial_ = 0;
  video_target_retirement_serial_ = 0;
  diagnostics_.video_target_format = "unavailable";
  ++diagnostics_.video_target_generation;
}

bool WindowsNativeCompositor::PresentVideoTarget(ID3D11Texture2D* texture,
                                                 uint32_t timeout_ms) {
  if (!texture || !running_) {
    return false;
  }
  uint64_t serial = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto found =
        std::find_if(video_targets_.begin(), video_targets_.end(),
                     [texture](const auto& candidate) {
                       return candidate.Get() == texture;
                     });
    const auto retired = std::find_if(retired_video_targets_.begin(),
                                      retired_video_targets_.end(),
                                      [texture](const auto& candidate) {
                                        return candidate.Get() == texture;
                                      });
    if (found == video_targets_.end() &&
        retired == retired_video_targets_.end() &&
        video_target_.Get() != texture) {
      diagnostics_.last_error =
          "Windows compositor rejected a target outside its ring";
      return false;
    }
    const bool starts_retained_handoff =
        video_target_handoff_pending_ && found != video_targets_.end();
    const bool starts_retired_release =
        !retired_video_targets_.empty() && found != video_targets_.end();
    video_target_ = texture;
    D3D11_TEXTURE2D_DESC presented_desc = {};
    texture->GetDesc(&presented_desc);
    video_target_format_ = presented_desc.Format;
    video_srv_.Reset();
    serial = ++video_publish_serial_;
    if (starts_retained_handoff) {
      vr::WindowsCompositorViewportRect viewport{
          video_viewport_left_,          video_viewport_top_,
          video_viewport_width_,         video_viewport_height_,
          video_viewport_surface_width_, video_viewport_surface_height_};
      if (vr::synchronize_retained_horizontal_viewport_handoff(
              viewport, static_cast<int>(presented_desc.Width),
              static_cast<int>(presented_desc.Height))) {
        video_viewport_width_ = viewport.width;
        ++diagnostics_.video_target_retained_geometry_sync_count;
        spdlog::info(
            "[WindowsCompositor] retained target geometry sync serial={} "
            "viewport=({},{} {}x{}) surface={}x{}",
            serial, video_viewport_left_, video_viewport_top_,
            video_viewport_width_, video_viewport_height_,
            video_viewport_surface_width_, video_viewport_surface_height_);
      }
      video_target_handoff_pending_ = false;
      video_target_handoff_serial_ = serial;
    }
    if (starts_retired_release) {
      video_target_retirement_serial_ = serial;
    }
    ++diagnostics_.video_publish_count;
  }
  SignalComposite();
  std::unique_lock<std::mutex> lock(state_mutex_);
  const bool completed = state_condition_.wait_for(
      lock, std::chrono::milliseconds(timeout_ms), [this, serial] {
        return video_completed_serial_ >= serial || !running_;
      });
  const bool success = completed && video_completed_serial_ >= serial &&
                       last_video_presentation_succeeded_;
  const bool schedule_retry = !success && running_;
  if (schedule_retry) {
    ++diagnostics_.video_present_retry_count;
  }
  if (!success) {
    spdlog::warn(
        "[WindowsCompositor] video present failed serial={} completed={} "
        "completed_serial={} running={} error={}",
        serial, completed, video_completed_serial_, running_.load(),
        diagnostics_.last_error);
  } else if (serial <= 8 || serial % 120 == 0) {
    spdlog::info(
        "[WindowsCompositor] video present serial={} flutter_generation={} "
        "composites={}",
        serial, cached_flutter_generation_, diagnostics_.composite_count);
  }
  lock.unlock();
  if (schedule_retry) {
    // Keep the prior swap-chain backbuffer visible and retry composition from
    // the retained new target. The renderer target stays alive through
    // video_target_, so no viewport redraw or Flutter frame is required.
    SignalComposite();
  }
  return success;
}

bool WindowsNativeCompositor::IsCurrentVideoTarget(
    ID3D11Texture2D* texture) const {
  if (!texture) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return std::any_of(video_targets_.begin(), video_targets_.end(),
                     [texture](const auto& candidate) {
                       return candidate.Get() == texture;
                     });
}

void WindowsNativeCompositor::SetVideoViewportRect(
    int left,
    int top,
    int width,
    int height,
    int surface_width,
    int surface_height) {
  if (width <= 0 || height <= 0 || surface_width <= 0 ||
      surface_height <= 0) {
    return;
  }
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    changed = video_viewport_left_ != left || video_viewport_top_ != top ||
              video_viewport_width_ != width ||
              video_viewport_height_ != height ||
              video_viewport_surface_width_ != surface_width ||
              video_viewport_surface_height_ != surface_height;
    video_viewport_left_ = left;
    video_viewport_top_ = top;
    video_viewport_width_ = width;
    video_viewport_height_ = height;
    video_viewport_surface_width_ = surface_width;
    video_viewport_surface_height_ = surface_height;
  }
  if (changed) {
    spdlog::info(
        "[WindowsCompositor] viewport rect=({},{} {}x{}) surface={}x{}",
        left, top, width, height, surface_width, surface_height);
    SignalComposite();
  }
}

void WindowsNativeCompositor::SetBackgroundColor(
    float red, float green, float blue, float alpha) {
  const float next[4] = {
      std::clamp(red, 0.0f, 1.0f),
      std::clamp(green, 0.0f, 1.0f),
      std::clamp(blue, 0.0f, 1.0f),
      std::clamp(alpha, 0.0f, 1.0f),
  };
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (size_t index = 0; index < 4; ++index) {
      changed = changed || background_color_[index] != next[index];
      background_color_[index] = next[index];
    }
    const auto to_byte = [](float value) -> uint32_t {
      return static_cast<uint32_t>(std::lround(value * 255.0f));
    };
    diagnostics_.background_color_argb =
        (to_byte(next[3]) << 24u) | (to_byte(next[0]) << 16u) |
        (to_byte(next[1]) << 8u) | to_byte(next[2]);
  }
  if (changed) {
    spdlog::info(
        "[WindowsCompositor] background color argb=0x{:08X}",
        diagnostics().background_color_argb);
    SignalComposite();
  }
}

WindowsNativeCompositorDiagnostics WindowsNativeCompositor::diagnostics() const {
  WindowsNativeCompositorDiagnostics result;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    result = diagnostics_;
    result.video_target_retired_count = retired_video_targets_.size();
    result.flutter_publish_sample_count =
        diagnostics_.flutter_publish_count - flutter_publish_sample_baseline_;
  }
  FlutterDesktopWindowsSurfaceExportState export_state = {};
  export_state.struct_size = sizeof(export_state);
  if (flutter_view_ &&
      FlutterDesktopViewGetSurfaceExportState(flutter_view_, &export_state)) {
    result.flutter_export_request_count = export_state.request_count;
    result.flutter_export_request_dispatch_count =
        export_state.request_dispatch_count;
    result.flutter_export_schedule_frame_count =
        export_state.schedule_frame_count;
    result.flutter_export_vsync_count = export_state.vsync_count;
    result.flutter_export_present_count = export_state.present_count;
    result.flutter_export_begin_count = export_state.export_begin_count;
    result.flutter_export_flush_count = export_state.export_flush_count;
    result.flutter_export_finish_count = export_state.export_finish_count;
    result.flutter_export_pending_pump_frames =
        export_state.pending_frame_pump_frames;
  }
  return result;
}

void WindowsNativeCompositor::ResetFlutterPublishSample() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  flutter_publish_sample_baseline_ = diagnostics_.flutter_publish_count;
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
    latest_flutter_published_generation_ =
        std::max(latest_flutter_published_generation_, flutter_generation);
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
    running_ = false;
    ShutdownOnThread();
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
    if (!ApplyPendingOutputConfiguration()) {
      continue;
    }
    if (resize_pending_.exchange(false) && !ResizeSwapChain()) {
      continue;
    }
    (void)CompositeLatest();
  }
  running_ = false;
  ShutdownOnThread();
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
  if (FAILED(device_.As(&multithread_)) || !multithread_) {
    SetError("DComp compositor could not enable D3D11 multithread protection");
    return false;
  }
  multithread_->SetMultithreadProtected(TRUE);

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
  swap_desc.Format = output_format_;
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
  UINT color_space_support = 0;
  if (FAILED(swap_chain_->CheckColorSpaceSupport(output_color_space_,
                                                 &color_space_support)) ||
      (color_space_support &
       DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0 ||
      FAILED(swap_chain_->SetColorSpace1(output_color_space_))) {
    SetError("DComp compositor could not configure its SDR color space");
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
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    flutter_cache_srv_.Reset();
    flutter_cache_.Reset();
    cached_flutter_generation_ = 0;
    cached_flutter_ring_generation_ = 0;
    flutter_cache_refresh_count_ = 0;
    video_srv_.Reset();
    video_target_.Reset();
    video_targets_.clear();
    retired_video_targets_.clear();
    video_target_handoff_pending_ = false;
    video_target_handoff_serial_ = 0;
    video_target_retirement_serial_ = 0;
  }
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
  multithread_.Reset();
  device_.Reset();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    diagnostics_.initialized = false;
    diagnostics_.flutter_export_enabled = false;
  }
  state_condition_.notify_all();
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
  std::string error;
  if (!ResizeSwapChainBuffers(width, height, output_format_,
                              output_color_space_, error)) {
    SetError(error);
    return false;
  }
  width_ = width;
  height_ = height;
  return true;
}

bool WindowsNativeCompositor::ApplyPendingOutputConfiguration() {
  WindowsNativeCompositorOutputConfig config;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (completed_output_generation_ >= requested_output_generation_) {
      return true;
    }
    config = requested_output_config_;
    generation = requested_output_generation_;
  }
  std::string error;
  const bool success = ApplyOutputConfiguration(config, error);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    completed_output_generation_ = generation;
    completed_output_succeeded_ = success;
    if (!success) {
      diagnostics_.last_error = error;
      diagnostics_.last_composite_succeeded = false;
    }
  }
  state_condition_.notify_all();
  return success;
}

bool WindowsNativeCompositor::ApplyOutputConfiguration(
    const WindowsNativeCompositorOutputConfig& config, std::string& error) {
  const DXGI_FORMAT next_format = config.linear_scrgb
                                      ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                      : DXGI_FORMAT_B8G8R8A8_UNORM;
  const DXGI_COLOR_SPACE_TYPE next_color_space =
      config.linear_scrgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                          : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  if (next_format != output_format_ ||
      next_color_space != output_color_space_) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      video_srv_.Reset();
      video_target_.Reset();
    }
    if (!WaitForGpu()) {
      error = "DComp compositor could not drain before output reconfiguration";
      return false;
    }
    if (!ResizeSwapChainBuffers(width_, height_, next_format, next_color_space,
                                error)) {
      if (config.linear_scrgb) {
        std::string rollback_error;
        if (ResizeSwapChainBuffers(width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM,
                                   DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                                   rollback_error)) {
          output_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
          output_color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
          applied_output_config_.linear_scrgb = false;
          std::lock_guard<std::mutex> lock(state_mutex_);
          diagnostics_.scrgb_output_enabled = false;
          diagnostics_.output_mode = "native-compositor-sdr";
          diagnostics_.swap_chain_format = "bgra8";
          diagnostics_.swap_chain_color_space = "RGB_FULL_G22_NONE_P709";
          diagnostics_.output_fallback_reason =
              "scrgb-swap-chain-configuration-failed";
        } else {
          error += "; SDR rollback failed: " + rollback_error;
        }
      }
      return false;
    }
    output_format_ = next_format;
    output_color_space_ = next_color_space;
  }
  applied_output_config_ = config;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    diagnostics_.scrgb_output_enabled = config.linear_scrgb;
    diagnostics_.output_mode = config.linear_scrgb ? "native-compositor-scrgb"
                                                   : "native-compositor-sdr";
    diagnostics_.swap_chain_format = config.linear_scrgb ? "rgba16f" : "bgra8";
    diagnostics_.swap_chain_color_space = config.linear_scrgb
                                              ? "RGB_FULL_G10_NONE_P709"
                                              : "RGB_FULL_G22_NONE_P709";
    diagnostics_.sdr_white_level_milli_nits = static_cast<int64_t>(
        std::llround(config.sdr_white_level_nits * 1000.0));
    diagnostics_.output_fallback_reason = "none";
    diagnostics_.last_error.clear();
  }
  spdlog::info(
      "[WindowsCompositor] output mode={} format={} color_space={} "
      "sdr_white_nits={:.3f} flutter_source=bgra8-premultiplied-srgb",
      config.linear_scrgb ? "native-compositor-scrgb" : "native-compositor-sdr",
      config.linear_scrgb ? "rgba16f" : "bgra8",
      config.linear_scrgb ? "RGB_FULL_G10_NONE_P709" : "RGB_FULL_G22_NONE_P709",
      config.sdr_white_level_nits);
  return true;
}

bool WindowsNativeCompositor::ResizeSwapChainBuffers(
    uint32_t width, uint32_t height, DXGI_FORMAT format,
    DXGI_COLOR_SPACE_TYPE color_space, std::string& error) {
  if (!swap_chain_) {
    error = "DComp compositor swap chain is unavailable";
    return false;
  }
  const HRESULT resize = swap_chain_->ResizeBuffers(
      3, width, height, format, 0);
  if (FAILED(resize)) {
    error = "DComp compositor swap-chain resize failed hr=" +
            std::to_string(static_cast<long>(resize));
    return false;
  }
  UINT support = 0;
  const HRESULT check =
      swap_chain_->CheckColorSpaceSupport(color_space, &support);
  if (FAILED(check) ||
      (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
    error = "DComp compositor color space is unsupported";
    return false;
  }
  const HRESULT set = swap_chain_->SetColorSpace1(color_space);
  if (FAILED(set)) {
    error = "DComp compositor SetColorSpace1 failed hr=" +
            std::to_string(static_cast<long>(set));
    return false;
  }
  return true;
}

bool WindowsNativeCompositor::CompositeLatest() {
  uint64_t video_serial = 0;
  uint64_t latest_flutter_generation = 0;
  bool has_flutter_cache = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_serial = video_publish_serial_;
    latest_flutter_generation = latest_flutter_published_generation_;
    has_flutter_cache = flutter_cache_srv_ != nullptr;
  }
  const auto fail_video = [this, &video_serial]() {
    CompleteVideoPresentation(video_serial, false);
  };
  if (!has_flutter_cache ||
      latest_flutter_generation > cached_flutter_generation_) {
    FlutterDesktopWindowsSurface lease = {};
    lease.struct_size = sizeof(lease);
    if (!FlutterDesktopViewAcquireLatestSurface(flutter_view_, &lease)) {
      const bool cache_unavailable = !flutter_cache_srv_;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++diagnostics_.acquire_failure_count;
        if (cache_unavailable) {
          diagnostics_.last_composite_succeeded = false;
        }
      }
      if (cache_unavailable) {
        fail_video();
        return false;
      }
    } else {
      const auto release_lease = [this, lease]() {
        FlutterDesktopViewReleaseSurface(flutter_view_, lease.lease_id);
      };
      bool cache_success = lease.shared_texture_handle &&
          lease.format == DXGI_FORMAT_B8G8R8A8_UNORM &&
          lease.alpha_mode ==
              kFlutterDesktopWindowsSurfaceAlphaModePremultiplied;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> flutter_texture;
      Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
      if (cache_success) {
        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        const HRESULT device1_result = device_.As(&device1);
        cache_success = SUCCEEDED(device1_result) &&
            SUCCEEDED(device1->OpenSharedResource1(
                lease.shared_texture_handle, IID_PPV_ARGS(&flutter_texture))) &&
            flutter_texture && SUCCEEDED(flutter_texture.As(&keyed_mutex)) &&
            SUCCEEDED(keyed_mutex->AcquireSync(lease.consumer_acquire_key, 16));
      }
      bool mutex_acquired = cache_success;
      if (cache_success) {
        D3D11_TEXTURE2D_DESC cache_desc = {};
        if (flutter_cache_) {
          flutter_cache_->GetDesc(&cache_desc);
        }
        if (!flutter_cache_ || cache_desc.Width != lease.width ||
            cache_desc.Height != lease.height || cache_desc.Format != lease.format) {
          flutter_cache_srv_.Reset();
          flutter_cache_.Reset();
          cache_desc = {};
          cache_desc.Width = lease.width;
          cache_desc.Height = lease.height;
          cache_desc.MipLevels = 1;
          cache_desc.ArraySize = 1;
          cache_desc.Format = lease.format;
          cache_desc.SampleDesc.Count = 1;
          cache_desc.Usage = D3D11_USAGE_DEFAULT;
          cache_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
          cache_success = SUCCEEDED(
              device_->CreateTexture2D(&cache_desc, nullptr, &flutter_cache_));
          if (cache_success) {
            cache_success = SUCCEEDED(device_->CreateShaderResourceView(
                flutter_cache_.Get(), nullptr, &flutter_cache_srv_));
          }
        }
        if (cache_success) {
          {
            ScopedD3D11ContextLock context_lock(multithread_.Get());
            context_->CopyResource(flutter_cache_.Get(), flutter_texture.Get());
          }
          cache_success = WaitForGpu();
        }
      }
      if (mutex_acquired) {
        keyed_mutex->ReleaseSync(lease.producer_release_key);
      }
      release_lease();
      if (cache_success) {
        cached_flutter_generation_ = lease.frame_generation;
        cached_flutter_ring_generation_ = lease.ring_generation;
        ++flutter_cache_refresh_count_;
        if (lease.width != width_ || lease.height != height_) {
          std::string resize_error;
          if (!ResizeSwapChainBuffers(lease.width, lease.height, output_format_,
                                      output_color_space_, resize_error)) {
            SetError(
                "DComp compositor could not match the Flutter surface size: " +
                resize_error);
            fail_video();
            return false;
          }
          width_ = lease.width;
          height_ = lease.height;
        }
        if (flutter_cache_refresh_count_ <= 8 ||
            flutter_cache_refresh_count_ % 120 == 0) {
          FlutterDesktopWindowsSurfaceExportState export_state = {};
          export_state.struct_size = sizeof(export_state);
          FlutterDesktopViewGetSurfaceExportState(flutter_view_, &export_state);
          spdlog::info(
              "[WindowsCompositor] cached Flutter refresh={} generation={} "
              "ring={} size={}x{} requests={} dispatched={} scheduled={} "
              "vsync={} presents={} pending_pump={}",
              flutter_cache_refresh_count_, cached_flutter_generation_,
              cached_flutter_ring_generation_, lease.width, lease.height,
              export_state.request_count,
              export_state.request_dispatch_count,
              export_state.schedule_frame_count, export_state.vsync_count,
              export_state.present_count,
              export_state.pending_frame_pump_frames);
        }
      } else if (!flutter_cache_srv_) {
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          ++diagnostics_.keyed_mutex_failure_count;
          diagnostics_.last_composite_succeeded = false;
        }
        fail_video();
        return false;
      }
    }
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  bool success = flutter_cache_srv_ && SUCCEEDED(swap_chain_->GetBuffer(
      0, IID_PPV_ARGS(&back_buffer)));
  success = success && SUCCEEDED(device_->CreateRenderTargetView(
      back_buffer.Get(), nullptr, &target));

  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv;
  CompositeConstants composite_constants;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    video_serial = video_publish_serial_;
    if (video_target_ && !video_srv_) {
      device_->CreateShaderResourceView(video_target_.Get(), nullptr, &video_srv_);
    }
    video_srv = video_srv_;
    composite_constants.output_scrgb =
        applied_output_config_.linear_scrgb ? 1u : 0u;
    composite_constants.video_is_sdr =
        video_target_format_ == DXGI_FORMAT_R16G16B16A16_FLOAT ? 0u : 1u;
    composite_constants.sdr_white_scale =
        static_cast<float>(applied_output_config_.sdr_white_level_nits / 80.0);
    std::copy_n(background_color_, 4, composite_constants.background_color);
    if (video_viewport_width_ > 0 && video_viewport_height_ > 0 &&
        video_viewport_surface_width_ > 0 &&
        video_viewport_surface_height_ > 0) {
      composite_constants.video_rect[0] = std::clamp(
          static_cast<float>(video_viewport_left_) /
              static_cast<float>(video_viewport_surface_width_),
          0.0f, 1.0f);
      composite_constants.video_rect[1] = std::clamp(
          static_cast<float>(video_viewport_top_) /
              static_cast<float>(video_viewport_surface_height_),
          0.0f, 1.0f);
      composite_constants.video_rect[2] = std::clamp(
          static_cast<float>(video_viewport_width_) /
              static_cast<float>(video_viewport_surface_width_),
          0.0f, 1.0f - composite_constants.video_rect[0]);
      composite_constants.video_rect[3] = std::clamp(
          static_cast<float>(video_viewport_height_) /
              static_cast<float>(video_viewport_surface_height_),
          0.0f, 1.0f - composite_constants.video_rect[1]);
    }
  }
  if (success) {
    {
      ScopedD3D11ContextLock context_lock(multithread_.Get());
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
      composite_constants.has_video = video_srv ? 1u : 0u;
      context_->UpdateSubresource(
          constants_.Get(), 0, nullptr, &composite_constants, 0, 0);
      ID3D11Buffer* constant_pointer = constants_.Get();
      context_->PSSetConstantBuffers(0, 1, &constant_pointer);
      ID3D11SamplerState* sampler_pointer = sampler_.Get();
      context_->PSSetSamplers(0, 1, &sampler_pointer);
      ID3D11ShaderResourceView* resources[] = {
          flutter_cache_srv_.Get(), video_srv.Get()};
      context_->PSSetShaderResources(0, 2, resources);
      context_->Draw(3, 0);
      ID3D11ShaderResourceView* empty[] = {nullptr, nullptr};
      context_->PSSetShaderResources(0, 2, empty);
    }
    success = WaitForGpu();
  }
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
    diagnostics_.last_flutter_frame_generation = cached_flutter_generation_;
    if (success) {
      ++diagnostics_.composite_count;
      diagnostics_.last_error.clear();
    }
  }
  CompleteVideoPresentation(video_serial, success);
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

void WindowsNativeCompositor::CompleteVideoPresentation(uint64_t serial,
                                                        bool success) {
  if (serial == 0) {
    return;
  }
  bool completed_retained_handoff = false;
  bool completed_retired_release = false;
  uint64_t handoff_count = 0;
  uint64_t reconfigure_count = 0;
  uint64_t generation = 0;
  size_t released_retired_targets = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (serial < video_completed_serial_) {
      return;
    }
    video_completed_serial_ = serial;
    last_video_presentation_succeeded_ = success;
    if (success) {
      ++diagnostics_.video_present_count;
    }
    if (success && video_target_handoff_serial_ != 0 &&
        serial >= video_target_handoff_serial_) {
      ++diagnostics_.video_target_retained_handoff_count;
      completed_retained_handoff = true;
      handoff_count = diagnostics_.video_target_retained_handoff_count;
      reconfigure_count =
          diagnostics_.video_target_retained_reconfigure_count;
      generation = diagnostics_.video_target_generation;
      video_target_handoff_serial_ = 0;
    }
    if (success && video_target_retirement_serial_ != 0 &&
        serial >= video_target_retirement_serial_) {
      released_retired_targets = retired_video_targets_.size();
      diagnostics_.video_target_retired_release_count +=
          released_retired_targets;
      retired_video_targets_.clear();
      video_target_retirement_serial_ = 0;
      completed_retired_release = released_retired_targets > 0;
    }
  }
  if (completed_retained_handoff) {
    spdlog::info(
        "[WindowsCompositor] retained target handoff complete generation={} "
        "reconfigures={} handoffs={} released_retired={}",
        generation, reconfigure_count, handoff_count,
        released_retired_targets);
  } else if (completed_retired_release) {
    spdlog::info(
        "[WindowsCompositor] startup target retirement complete "
        "released_retired={}",
        released_retired_targets);
  }
  state_condition_.notify_all();
}

void WindowsNativeCompositor::SetError(std::string error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  diagnostics_.last_error = std::move(error);
  diagnostics_.last_composite_succeeded = false;
}
