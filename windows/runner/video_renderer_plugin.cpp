#include "video_renderer_plugin.h"

#include "common/win_utf8.h"
#include "analysis_ffi.h"
#include "native_player_channel_names.h"

#include "renderer/layout/layout_validation.h"
#include "renderer/renderer_config_validation.h"
#include "utils.h"
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter_windows.h>
#include <spdlog/spdlog.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <exception>
#include <cmath>
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <variant>
#include <limits>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
using PluginResult = flutter::MethodResult<flutter::EncodableValue>;
constexpr UINT_PTR kDisplayPolicyRefreshTimer = 0x56504844;
constexpr UINT kDisplayPolicyRefreshDelayMs = 120;
constexpr UINT kPlatformTaskMessage = WM_APP + 0x564;

void ReportMethodException(
    PluginResult* result,
    const std::string& method,
    const char* code,
    const std::string& message) {
    spdlog::warn("[VideoRendererPlugin] {} failed with {}: {}", method, code, message);
    if (result) {
        result->Error(code, message);
    }
}

void ReportMethodException(
    PluginResult* result,
    const std::string& method,
    const std::bad_variant_access& e) {
    ReportMethodException(result, method, "BAD_ARGS", e.what());
}

void ReportMethodException(
    PluginResult* result,
    const std::string& method,
    const std::exception& e) {
    ReportMethodException(result, method, "NATIVE_EXCEPTION", e.what());
}

void ReportUnknownMethodException(PluginResult* result, const std::string& method) {
    ReportMethodException(result, method, "NATIVE_EXCEPTION", "Unknown native exception");
}

flutter::EncodableValue enc_i64(int64_t value) {
    return flutter::EncodableValue(static_cast<int64_t>(value));
}

flutter::EncodableValue enc_i32(int32_t value) {
    return flutter::EncodableValue(static_cast<int32_t>(value));
}

std::string presented_anchor_mode_name(vr::RendererPresentedAnchorMode mode) {
    switch (mode) {
        case vr::RendererPresentedAnchorMode::ViewportPresent:
            return "viewport-present";
        case vr::RendererPresentedAnchorMode::SourceCachePublish:
            return "source-cache-publish";
        case vr::RendererPresentedAnchorMode::None:
        default:
            return "none";
    }
}

flutter::EncodableMap make_track_map(const vr::TrackInfo& info) {
    flutter::EncodableMap map;
    map[flutter::EncodableValue("fileId")] = flutter::EncodableValue(info.file_id);
    map[flutter::EncodableValue("slot")] = flutter::EncodableValue(info.slot);
    map[flutter::EncodableValue("path")] = flutter::EncodableValue(info.file_path);
    map[flutter::EncodableValue("width")] = flutter::EncodableValue(info.width);
    map[flutter::EncodableValue("height")] = flutter::EncodableValue(info.height);
    map[flutter::EncodableValue("durationUs")] = flutter::EncodableValue(static_cast<int64_t>(info.duration_us));
    map[flutter::EncodableValue("startTimeUs")] = flutter::EncodableValue(static_cast<int64_t>(info.start_time_us));
    map[flutter::EncodableValue("bitRate")] = flutter::EncodableValue(static_cast<int64_t>(info.bit_rate));
    map[flutter::EncodableValue("formatName")] = flutter::EncodableValue(info.format_name);
    map[flutter::EncodableValue("codecName")] = flutter::EncodableValue(info.codec_name);
    map[flutter::EncodableValue("codecLongName")] = flutter::EncodableValue(info.codec_long_name);
    map[flutter::EncodableValue("decoderName")] = flutter::EncodableValue(info.decoder_name);
    return map;
}

}  // namespace

struct PlatformTaskState {
    explicit PlatformTaskState(HWND window) : window_handle(window) {}

    HWND window_handle = nullptr;
    std::mutex mutex;
    std::queue<std::function<void()>> tasks;
    bool alive = true;
};

namespace {

HWND PlatformTaskTarget(HWND window_handle) {
    if (!window_handle || !IsWindow(window_handle)) {
        return nullptr;
    }
    HWND root = GetAncestor(window_handle, GA_ROOT);
    return root ? root : window_handle;
}

void PostPlatformTaskToState(
    const std::shared_ptr<PlatformTaskState>& state,
    std::function<void()> task) {
    if (!state) {
        return;
    }

    HWND target = PlatformTaskTarget(state->window_handle);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->alive || !target) {
            return;
        }
        state->tasks.push(std::move(task));
    }
    PostMessage(target, kPlatformTaskMessage, 0, 0);
}

flutter::EncodableMap make_presented_frame_map(const vr::TrackPerfStats& stats) {
    flutter::EncodableMap frame;
    frame[flutter::EncodableValue("fileId")] =
        flutter::EncodableValue(stats.file_id);
    frame[flutter::EncodableValue("ptsUs")] =
        enc_i64(stats.current_pts_us);
    frame[flutter::EncodableValue("dtsUs")] =
        enc_i64(stats.current_dts_us);
    frame[flutter::EncodableValue("analysisFrameIndex")] =
        enc_i32(stats.analysis_frame_index);
    frame[flutter::EncodableValue("frameIdentityMode")] =
        enc_i32(static_cast<int32_t>(stats.frame_identity_mode));
    frame[flutter::EncodableValue("sourcePacketIndex")] =
        enc_i32(stats.source_packet_index);
    frame[flutter::EncodableValue("sourcePacketSize")] =
        enc_i32(stats.source_packet_size);
    frame[flutter::EncodableValue("sourcePacketPos")] =
        enc_i64(stats.source_packet_pos);
    frame[flutter::EncodableValue("sourcePacketPtsUs")] =
        enc_i64(stats.source_packet_pts);
    frame[flutter::EncodableValue("sourcePacketDtsUs")] =
        enc_i64(stats.source_packet_dts);
    return frame;
}

std::string format_ffmpeg_version(unsigned version) {
    return std::to_string((version >> 16) & 0xFF) + "." +
           std::to_string((version >> 8) & 0xFF) + "." +
           std::to_string(version & 0xFF);
}

void log_ffmpeg_runtime_versions() {
    spdlog::info(
        "[FFmpeg] av_version_info={} avcodec={} avformat={} avutil={} swresample={}",
        av_version_info(),
        format_ffmpeg_version(avcodec_version()),
        format_ffmpeg_version(avformat_version()),
        format_ffmpeg_version(avutil_version()),
        format_ffmpeg_version(swresample_version()));
}

std::shared_ptr<vr::NativePlayer> pin_diagnostics_player() {
    auto session = GlobalNativeDiagnosticsSessionRegistry().PinSession();
    return session ? session->PinPlayer() : nullptr;
}

bool require_player(const std::shared_ptr<vr::NativePlayer>& player, PluginResult* result) {
    if (!player) {
        result->Error("NO_PLAYER", "Player not created");
        return false;
    }
    return true;
}

int64_t query_window_refresh_hz(HWND hwnd) {
    if (!hwnd) {
        return 60;
    }
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return 60;
    }
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
        mode.dmDisplayFrequency == 0) {
        return 60;
    }
    return static_cast<int64_t>(mode.dmDisplayFrequency);
}

} // namespace

bool read_int64_arg(const flutter::EncodableValue& value, int64_t& out) {
    if (std::holds_alternative<int>(value)) {
        out = static_cast<int64_t>(std::get<int>(value));
        return true;
    }
    if (std::holds_alternative<int64_t>(value)) {
        out = std::get<int64_t>(value);
        return true;
    }
    return false;
}

bool read_int_arg(const flutter::EncodableValue& value, int& out) {
    int64_t raw = 0;
    if (!read_int64_arg(value, raw) ||
        raw < std::numeric_limits<int>::min() ||
        raw > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(raw);
    return true;
}

bool read_double_arg(const flutter::EncodableValue& value, double& out) {
    if (std::holds_alternative<double>(value)) {
        out = std::get<double>(value);
        return true;
    }
    if (std::holds_alternative<int>(value)) {
        out = static_cast<double>(std::get<int>(value));
        return true;
    }
    if (std::holds_alternative<int64_t>(value)) {
        out = static_cast<double>(std::get<int64_t>(value));
        return true;
    }
    return false;
}

bool read_bool_arg(const flutter::EncodableValue& value, bool& out) {
    if (!std::holds_alternative<bool>(value)) {
        return false;
    }
    out = std::get<bool>(value);
    return true;
}

bool read_string_arg(const flutter::EncodableValue& value, std::string& out) {
    if (!std::holds_alternative<std::string>(value)) {
        return false;
    }
    out = std::get<std::string>(value);
    return true;
}

extern "C" __declspec(dllexport)
const NakiVrDiagnostics* naki_vr_get_diagnostics() {
    thread_local NakiVrDiagnostics d{};
    static const NativeDiagnosticsProvider diagnostics;
    diagnostics.FillFfiDiagnostics(d, pin_diagnostics_player());
    return &d;
}

// static
void VideoRendererPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar,
    FlutterDesktopPluginRegistrarRef core_registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), native_player_channels::kMethodChannel,
        &flutter::StandardMethodCodec::GetInstance());
    auto event_channel = std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
        registrar->messenger(), native_player_channels::kEventChannel,
        &flutter::StandardMethodCodec::GetInstance());

    auto* texture_registrar = registrar->texture_registrar();

    // Get DXGI adapter from the Flutter view
    IDXGIAdapter* adapter = nullptr;
    auto* view = registrar->GetView();
    if (view) {
        adapter = view->GetGraphicsAdapter();
    }

    HWND window_handle = nullptr;
    void* flutter_view_handle = nullptr;
    if (view) {
        window_handle = view->GetNativeWindow();
        flutter_view_handle =
            FlutterDesktopPluginRegistrarGetView(core_registrar);
    }
    auto plugin = std::make_unique<VideoRendererPlugin>(
        registrar, texture_registrar, adapter, window_handle,
        flutter_view_handle);

    channel->SetMethodCallHandler(
        [plugin_ptr = plugin.get()](const auto& call, auto result) {
            plugin_ptr->HandleMethodCall(call, std::move(result));
        });
    event_channel->SetStreamHandler(
        std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
            [plugin_ptr = plugin.get()](
                const flutter::EncodableValue*,
                std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
                -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
                plugin_ptr->SetEventSink(std::move(events));
                return nullptr;
            },
            [plugin_ptr = plugin.get()](const flutter::EncodableValue*)
                -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
                plugin_ptr->ClearEventSink();
                return nullptr;
            }));
    plugin->event_bridge_.RegisterDrainWindow();

    registrar->AddPlugin(std::move(plugin));
}

VideoRendererPlugin::VideoRendererPlugin(
    flutter::PluginRegistrarWindows* registrar,
    flutter::TextureRegistrar* texture_registrar,
    IDXGIAdapter* dxgi_adapter,
    HWND window_handle,
    void* flutter_view_handle)
    : texture_bridge_(texture_registrar),
      diagnostics_session_(std::make_shared<NativeDiagnosticsSession>()),
      registrar_(registrar),
      platform_task_state_(std::make_shared<PlatformTaskState>(window_handle)),
      window_handle_(window_handle),
      flutter_view_handle_(flutter_view_handle) {
    if (registrar_) {
        window_proc_delegate_id_ = registrar_->RegisterTopLevelWindowProcDelegate(
            [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
                -> std::optional<LRESULT> {
                return HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
            });
    }
    RegisterMethodHandlers();
    GlobalNativeDiagnosticsSessionRegistry().Publish(diagnostics_session_);

    if (dxgi_adapter) {
        dxgi_adapter->AddRef();
        dxgi_adapter_.Attach(dxgi_adapter);
    }

    const auto logging = logging_bootstrap_.InitializeDefaults();

    spdlog::info("[VideoRendererPlugin] Plugin constructed, native logging initialized: {}", logging.file_path);
    spdlog::info("[VideoRendererPlugin] Crash handler installed (VEH + SEH), crash dir: {}", logging.logs_dir);
    log_ffmpeg_runtime_versions();

    // Register PTS callback for analysis FFI (avoids analysis_ffi depending on NativePlayer)
    naki_analysis_register_pts_callback_for_owner(
        diagnostics_session_.get(),
        []() -> int64_t {
            auto r = pin_diagnostics_player();
            return r ? r->current_pts_us() : 0;
        });
}

VideoRendererPlugin::~VideoRendererPlugin() {
    if (window_handle_) {
        KillTimer(window_handle_, kDisplayPolicyRefreshTimer);
    }
    if (registrar_ && window_proc_delegate_id_ >= 0) {
        registrar_->UnregisterTopLevelWindowProcDelegate(
            window_proc_delegate_id_);
        window_proc_delegate_id_ = -1;
    }
    if (native_compositor_) {
        native_compositor_->Stop();
        native_compositor_.reset();
    }
    if (platform_task_state_) {
        std::lock_guard<std::mutex> lock(platform_task_state_->mutex);
        platform_task_state_->alive = false;
        std::queue<std::function<void()>> empty;
        platform_task_state_->tasks.swap(empty);
    }
    event_bridge_.Shutdown();
    if (diagnostics_session_) {
        naki_analysis_clear_pts_callback_for_owner(diagnostics_session_.get());
        diagnostics_session_->ClearPlayer();
        GlobalNativeDiagnosticsSessionRegistry().Clear(diagnostics_session_);
    }
    texture_bridge_.DetachFrameCallback();
    if (player_) {
        player_->set_event_callback(nullptr);
        player_->shutdown();
    }
    texture_bridge_.Unregister();
    if (player_) {
        player_.reset();
    }
}

std::optional<LRESULT> VideoRendererPlugin::HandleTopLevelWindowProc(
    HWND,
    UINT message,
    WPARAM wparam,
    LPARAM) {
    if (message == kPlatformTaskMessage) {
        DrainPlatformTasks();
        return 0;
    }

    switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_MOVE:
    case WM_EXITSIZEMOVE:
    case WM_DPICHANGED:
        ScheduleDisplayPolicyRefresh();
        break;
    case WM_TIMER:
        if (wparam == kDisplayPolicyRefreshTimer) {
            KillTimer(window_handle_, kDisplayPolicyRefreshTimer);
            (void)RefreshPresentationPolicy("window-display-change");
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

void VideoRendererPlugin::ScheduleDisplayPolicyRefresh() {
    if (!window_handle_) {
        return;
    }
    SetTimer(
        window_handle_,
        kDisplayPolicyRefreshTimer,
        kDisplayPolicyRefreshDelayMs,
        nullptr);
}

void VideoRendererPlugin::PostPlatformTask(std::function<void()> task) {
    PostPlatformTaskToState(platform_task_state_, std::move(task));
}

void VideoRendererPlugin::DrainPlatformTasks() {
    auto state = platform_task_state_;
    if (!state) {
        return;
    }

    while (true) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->alive || state->tasks.empty()) {
                return;
            }
            task = std::move(state->tasks.front());
            state->tasks.pop();
        }
        task();
    }
}

void VideoRendererPlugin::SetEventSink(
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink) {
    event_bridge_.SetSink(std::move(sink));
}

void VideoRendererPlugin::ClearEventSink() {
    event_bridge_.ClearSink();
}

void VideoRendererPlugin::QueueRendererEvent(const vr::RendererEvent& event) {
    event_bridge_.Queue(event);
}

void VideoRendererPlugin::RegisterMethodHandlers() {
    using MethodCall = NativePlayerMethodDispatcher::MethodCall;
    using MethodResultPtr = NativePlayerMethodDispatcher::MethodResultPtr;

    method_dispatcher_.Register(
        "initLogging",
        [this](const MethodCall& call, MethodResultPtr result) {
            InitLogging(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "createPlayer",
        [this](const MethodCall& call, MethodResultPtr result) {
            CreatePlayer(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "destroyPlayer",
        [this](const MethodCall&, MethodResultPtr result) {
            DestroyPlayer(std::move(result));
        });
    method_dispatcher_.Register(
        "addTrack",
        [this](const MethodCall& call, MethodResultPtr result) {
            AddTrack(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "removeTrack",
        [this](const MethodCall& call, MethodResultPtr result) {
            RemoveTrack(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setTrackOffset",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetTrackOffset(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setLoopRange",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetLoopRange(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setAudibleTrack",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetAudibleTrack(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "play",
        [this](const MethodCall&, MethodResultPtr result) {
            Play(std::move(result));
        });
    method_dispatcher_.Register(
        "pause",
        [this](const MethodCall&, MethodResultPtr result) {
            Pause(std::move(result));
        });
    method_dispatcher_.Register(
        "seek",
        [this](const MethodCall& call, MethodResultPtr result) {
            Seek(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "resize",
        [this](const MethodCall& call, MethodResultPtr result) {
            Resize(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setNativeCompositorViewportRect",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetNativeCompositorViewportRect(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "requestNativeCompositorFlutterFrame",
        [this](const MethodCall& call, MethodResultPtr result) {
            RequestNativeCompositorFlutterFrame(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setNativeCompositorViewportTransform",
        [](const MethodCall&, MethodResultPtr result) {
            result->Success();
        });
    method_dispatcher_.Register(
        "prepareNativeCompositorSourceCache",
        [this](const MethodCall& call, MethodResultPtr result) {
            PrepareNativeCompositorSourceCache(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "clearNativeCompositorSourceCache",
        [this](const MethodCall& call, MethodResultPtr result) {
            ClearNativeCompositorSourceCache(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setNativeAnalysisOverlay",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetNativeAnalysisOverlay(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "ackNativeCompositorFlutterState",
        [this](const MethodCall& call, MethodResultPtr result) {
            AckNativeCompositorFlutterState(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "debugFailNativeCompositor",
        [this](const MethodCall& call, MethodResultPtr result) {
            DebugFailNativeCompositor(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "debugSimulateWindowsDeviceLoss",
        [this](const MethodCall& call, MethodResultPtr result) {
            DebugSimulateWindowsDeviceLoss(
                call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "resetNativePerfCounters",
        [this](const MethodCall&, MethodResultPtr result) {
            ResetNativePerfCounters(std::move(result));
        });
    method_dispatcher_.Register(
        "beginNativeInteractionSample",
        [this](const MethodCall& call, MethodResultPtr result) {
            BeginNativeInteractionSample(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "endNativeInteractionSample",
        [this](const MethodCall& call, MethodResultPtr result) {
            EndNativeInteractionSample(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setViewportBackgroundColor",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetViewportBackgroundColor(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "setSpeed",
        [this](const MethodCall& call, MethodResultPtr result) {
            SetSpeed(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "stepForward",
        [this](const MethodCall&, MethodResultPtr result) {
            StepForward(std::move(result));
        });
    method_dispatcher_.Register(
        "stepBackward",
        [this](const MethodCall&, MethodResultPtr result) {
            StepBackward(std::move(result));
        });
    method_dispatcher_.Register(
        "currentPts",
        [this](const MethodCall&, MethodResultPtr result) {
            CurrentPts(std::move(result));
        });
    method_dispatcher_.Register(
        "currentPresentedFrame",
        [this](const MethodCall& call, MethodResultPtr result) {
            CurrentPresentedFrame(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "duration",
        [this](const MethodCall&, MethodResultPtr result) {
            Duration(std::move(result));
        });
    method_dispatcher_.Register(
        "isPlaying",
        [this](const MethodCall&, MethodResultPtr result) {
            IsPlaying(std::move(result));
        });
    method_dispatcher_.Register(
        "getPlaybackSnapshot",
        [this](const MethodCall& call, MethodResultPtr result) {
            GetPlaybackSnapshot(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "applyLayout",
        [this](const MethodCall& call, MethodResultPtr result) {
            ApplyLayout(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "getTracks",
        [this](const MethodCall&, MethodResultPtr result) {
            GetTracks(std::move(result));
        });
    method_dispatcher_.Register(
        "getDiagnostics",
        [this](const MethodCall&, MethodResultPtr result) {
            GetDiagnostics(std::move(result));
        });
    method_dispatcher_.Register(
        "pickFiles",
        [this](const MethodCall& call, MethodResultPtr result) {
            PickFiles(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "captureViewport",
        [this](const MethodCall& call, MethodResultPtr result) {
            CaptureViewport(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "captureViewportRegion",
        [this](const MethodCall& call, MethodResultPtr result) {
            CaptureViewportRegion(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "captureWindow",
        [this](const MethodCall& call, MethodResultPtr result) {
            CaptureWindow(call.arguments(), std::move(result));
        });
    method_dispatcher_.Register(
        "getLayout",
        [this](const MethodCall&, MethodResultPtr result) {
            GetLayout(std::move(result));
        });
}

void VideoRendererPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    method_dispatcher_.Dispatch(method_call, std::move(result));
}

void VideoRendererPlugin::InitLogging(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    std::string level_str = "info";
    std::string logs_dir;
    std::string log_file_name;

    if (arguments) {
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (args) {
            auto it = args->find(flutter::EncodableValue("logLevel"));
            if (it != args->end()) {
                level_str = std::get<std::string>(it->second);
            }
            it = args->find(flutter::EncodableValue("logsDir"));
            if (it != args->end()) {
                logs_dir = std::get<std::string>(it->second);
            }
            it = args->find(flutter::EncodableValue("logFileName"));
            if (it != args->end()) {
                log_file_name = std::get<std::string>(it->second);
            }
        }
    }

    const auto logging = logging_bootstrap_.Reconfigure(level_str, logs_dir, log_file_name);

    spdlog::info(
        "[VideoRendererPlugin] Native logging reconfigured: level={}, file={}",
        logging.level, logging.file_path);

    result->Success();
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "initLogging", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "initLogging", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "initLogging");
    }
}

void VideoRendererPlugin::CreatePlayer(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (player_) {
        result->Error("ALREADY_CREATED", "Player already exists");
        return;
    }

    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }

    // Extract video paths
    auto paths_it = args->find(flutter::EncodableValue("videoPaths"));
    if (paths_it == args->end()) {
        result->Error("INVALID_ARGS", "videoPaths required");
        return;
    }
    if (!std::holds_alternative<flutter::EncodableList>(paths_it->second)) {
        result->Error("BAD_ARGS", "videoPaths must be a list");
        return;
    }
    const auto& paths_list = std::get<flutter::EncodableList>(paths_it->second);
    if (paths_list.empty()) {
        result->Error("BAD_ARGS", "videoPaths must not be empty");
        return;
    }

    int width = 1920;
    int height = 1080;
    auto w_it = args->find(flutter::EncodableValue("width"));
    auto h_it = args->find(flutter::EncodableValue("height"));
    if (w_it != args->end() && !read_int_arg(w_it->second, width)) {
        result->Error("BAD_ARGS", "width must be an integer");
        return;
    }
    if (h_it != args->end() && !read_int_arg(h_it->second, height)) {
        result->Error("BAD_ARGS", "height must be an integer");
        return;
    }
    if (auto validation = vr::validate_renderer_dimensions(width, height, "viewport size");
        !validation.ok) {
        result->Error("BAD_ARGS", validation.message);
        return;
    }
    bool use_hardware_decode = true;
    auto hw_it = args->find(flutter::EncodableValue("useHardwareDecode"));
    if (hw_it != args->end() && !read_bool_arg(hw_it->second, use_hardware_decode)) {
        result->Error("BAD_ARGS", "useHardwareDecode must be a boolean");
        return;
    }

    // Create player in headless mode
    vr::RendererConfig config;
    config.headless = true;
    config.backend.type = vr::RendererBackendType::D3D11;
    config.backend.adapter = dxgi_adapter_.Get();
    config.width = width;
    config.height = height;
    config.use_hardware_decode = use_hardware_decode;

    if (!config.backend.adapter) {
        result->Error(
            "NO_DXGI_ADAPTER",
            "Flutter DXGI adapter is unavailable; cannot create shared D3D11 texture");
        return;
    }

    presentation_request_ = vr::win_utf8::get_env_utf8(
        L"VOIDPLAYER_WINDOWS_PRESENTATION_MODE");
    const auto locked_display = display_probe_tracker_.Update(
        display_resolver_.Probe(window_handle_, dxgi_adapter_.Get()));
    presentation_policy_ = vr::resolve_windows_presentation_policy(
        presentation_request_, false, locked_display.probe);
    if (!presentation_policy_.supported) {
        result->Error(
            "UNSUPPORTED_PRESENTATION_MODE",
            "Windows presentation mode is unsupported; use auto, sdr, "
            "native-compositor-sdr, or native-compositor-scrgb");
        return;
    }
    config.backend.output_target = presentation_policy_.output_target;
    config.backend.shared_fp16_output =
        presentation_policy_.native_compositor_requested;
    config.backend.sdr_white_level_nits =
        presentation_policy_.hdr_output_requested
            ? static_cast<double>(
                  locked_display.probe.sdr_white_level_milli_nits) /
                  1000.0
            : 80.0;
    presentation_sdr_white_level_status_ =
        locked_display.probe.sdr_white_level_status;
    spdlog::info(
        "[WindowsPresentation] request={} mode={} output_target={} "
        "sdr_white_nits={:.3f} white_status={} fallback={}",
        presentation_policy_.request,
        presentation_policy_.mode,
        presentation_policy_.fp16_scrgb_requested ? "fp16-scrgb" : "sdr",
        config.backend.sdr_white_level_nits,
        presentation_sdr_white_level_status_,
        presentation_policy_.fallback_reason);

    for (const auto& p : paths_list) {
        std::string path;
        if (!read_string_arg(p, path)) {
            result->Error("BAD_ARGS", "video paths must be strings");
            return;
        }
        config.video_paths.push_back(path);
    }

    if (auto validation = vr::validate_renderer_config(config); !validation.ok) {
        result->Error("BAD_ARGS", validation.message);
        return;
    }

    player_ = std::make_shared<vr::NativePlayer>();
    diagnostics_session_->PublishPlayer(player_);
    if (!player_->initialize(config)) {
        diagnostics_session_->ClearPlayer();
        player_.reset();
        result->Error("INIT_FAILED", "Failed to initialize player");
        return;
    }

    presentation_policy_ = vr::resolve_windows_presentation_policy(
        presentation_request_,
        vr::windows_tracks_have_hdr_transfer(player_->track_infos()),
        locked_display.probe);
    if (!presentation_policy_.supported) {
        diagnostics_session_->ClearPlayer();
        player_->shutdown();
        player_.reset();
        result->Error(
            "UNSUPPORTED_PRESENTATION_MODE",
            "Windows presentation mode is unsupported; use auto, sdr, "
            "native-compositor-sdr, or native-compositor-scrgb");
        return;
    }
    const double resolved_white_nits =
        presentation_policy_.hdr_output_requested
            ? static_cast<double>(
                  locked_display.probe.sdr_white_level_milli_nits) /
                  1000.0
            : 80.0;
    presentation_sdr_white_level_status_ =
        presentation_policy_.hdr_output_requested
            ? locked_display.probe.sdr_white_level_status
            : "nominal-default";
    (void)player_->update_presentation_sdr_white_level(
        resolved_white_nits);
    presentation_locked_display_generation_ =
        locked_display.generation;
    presentation_locked_sdr_white_level_milli_nits_ =
        static_cast<int64_t>(
            std::llround(resolved_white_nits * 1000.0));

    if (!texture_bridge_.Register(player_)) {
        diagnostics_session_->ClearPlayer();
        player_->shutdown();
        player_.reset();
        result->Error("TEXTURE_FAILED", "Failed to register texture");
        return;
    }

    player_->set_event_callback([this](const vr::RendererEvent& event) {
        QueueRendererEvent(event);
    });

    if (presentation_policy_.native_compositor_requested) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> output_adapter;
        if (!display_resolver_.OpenAdapterForProbe(
                locked_display.probe, &output_adapter)) {
            output_adapter = dxgi_adapter_;
        }
        native_compositor_ = std::make_unique<WindowsNativeCompositor>();
        const bool compositor_started = native_compositor_->Start(
            window_handle_,
            flutter_view_handle_,
            player_,
            dxgi_adapter_.Get(),
            output_adapter.Get(),
            resolved_white_nits,
            presentation_policy_.hdr_output_requested
                ? WindowsNativeCompositor::OutputTarget::ScRGB
                : WindowsNativeCompositor::OutputTarget::SDR,
            [this](WindowsNativeCompositor::Phase phase,
                   uint64_t serial,
                   const std::string& reason) {
                const auto compositor = native_compositor_
                    ? native_compositor_->diagnostics()
                    : WindowsNativeCompositor::Diagnostics{};
                const bool scrgb_output =
                    compositor.output_target == "scrgb";
                const std::string compositor_mode =
                    scrgb_output
                        ? "native-compositor-scrgb"
                        : "native-compositor-sdr";
                const bool hole_requested =
                    phase == WindowsNativeCompositor::Phase::Preparing ||
                    phase == WindowsNativeCompositor::Phase::Active;
                const bool failed =
                    phase == WindowsNativeCompositor::Phase::Failed ||
                    (phase == WindowsNativeCompositor::Phase::Inactive &&
                     reason != "destroy-player" &&
                     reason != "shutdown");
                event_bridge_.QueueNativeCompositorState(
                    hole_requested,
                    true,
                    scrgb_output,
                    compositor_mode,
                    phase == WindowsNativeCompositor::Phase::Preparing
                        ? "preparing"
                        : phase == WindowsNativeCompositor::Phase::Active
                              ? "active"
                              : phase == WindowsNativeCompositor::Phase::Failed
                                    ? "failed"
                                    : "inactive",
                    static_cast<int64_t>(serial),
                    reason,
                    failed ? reason : "");
            });
        if (!compositor_started) {
            const auto diagnostics = native_compositor_->diagnostics();
            presentation_policy_.fallback_reason =
                diagnostics.fallback_reason;
            event_bridge_.QueueNativeCompositorState(
                false, true, false, presentation_policy_.mode,
                "inactive", 0,
                "native-compositor-start-failed",
                diagnostics.fallback_reason);
            native_compositor_.reset();
            texture_bridge_.Unregister();
            diagnostics_session_->ClearPlayer();
            player_->shutdown();
            player_.reset();
            result->Error(
                "NATIVE_COMPOSITOR_UNAVAILABLE",
                diagnostics.fallback_reason.empty()
                    ? "Windows native compositor failed to start"
                    : diagnostics.fallback_reason);
            return;
        }
    }

    spdlog::info(
        "[VideoRendererPlugin] Created player, texture_id={}, tracks={}, hw_decode={}",
        texture_bridge_.texture_id(),
        player_->track_infos().size(),
        use_hardware_decode);

    // Build result map with textureId and track info
    flutter::EncodableMap result_map;
    result_map[flutter::EncodableValue("textureId")] =
        flutter::EncodableValue(texture_bridge_.texture_id());

    flutter::EncodableList tracks_list;
    if (player_) {
        for (const auto& info : player_->track_infos()) {
            tracks_list.push_back(flutter::EncodableValue(make_track_map(info)));
        }
    }
    result_map[flutter::EncodableValue("tracks")] = flutter::EncodableValue(tracks_list);

    result->Success(flutter::EncodableValue(result_map));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "CreatePlayer", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "CreatePlayer", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "CreatePlayer");
    }
}

void VideoRendererPlugin::DestroyPlayer(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (native_compositor_) {
        if (player_) {
            player_->clear_source_cache("destroy-player");
        }
        native_compositor_source_signature_.clear();
        native_compositor_->Stop("destroy-player");
        native_compositor_.reset();
    }
    if (player_) {
        diagnostics_session_->ClearPlayer();
        texture_bridge_.DetachFrameCallback();
        player_->set_event_callback(nullptr);
        player_->shutdown();
    }
    texture_bridge_.Unregister();
    if (player_) {
        player_.reset();
    }

    spdlog::info("[VideoRendererPlugin] Destroyed player");
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "DestroyPlayer", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "DestroyPlayer");
    }
}

void VideoRendererPlugin::AddTrack(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!player_) {
        result->Error("NO_PLAYER", "Player not created");
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }

    auto it = args->find(flutter::EncodableValue("path"));
    if (it == args->end()) {
        result->Error("INVALID_ARGS", "path required");
        return;
    }
    std::string path;
    if (!read_string_arg(it->second, path) || path.empty()) {
        result->Error("BAD_ARGS", "path must be a non-empty string");
        return;
    }
    bool use_hardware_decode = true;
    auto hw_it = args->find(flutter::EncodableValue("useHardwareDecode"));
    if (hw_it != args->end() && !read_bool_arg(hw_it->second, use_hardware_decode)) {
        result->Error("BAD_ARGS", "useHardwareDecode must be a boolean");
        return;
    }

    int slot = player_->add_track(path, use_hardware_decode);
    if (slot < 0) {
        result->Error("ADD_FAILED", "Failed to add track");
        return;
    }

    auto infos = player_->track_infos();
    const vr::TrackInfo* found = nullptr;
    for (const auto& ti : infos) {
        if (ti.slot == slot) { found = &ti; break; }
    }
    if (!found) {
        result->Error("ADD_FAILED", "Track not found after add");
        return;
    }

    spdlog::info(
        "[VideoRendererPlugin] Added track: file_id={}, slot={}, hw_decode={}, path={}, tracks={}",
        found->file_id,
        slot,
        use_hardware_decode,
        path,
        player_->track_infos().size());
    (void)RefreshPresentationPolicy("track-added", false);
    result->Success(flutter::EncodableValue(make_track_map(*found)));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "addTrack", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "addTrack", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "addTrack");
    }
}

void VideoRendererPlugin::RemoveTrack(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!player_) {
        result->Error("NO_PLAYER", "Player not created");
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }

    auto it = args->find(flutter::EncodableValue("fileId"));
    if (it == args->end()) {
        result->Error("INVALID_ARGS", "fileId required");
        return;
    }
    int file_id = 0;
    if (!read_int_arg(it->second, file_id)) {
        result->Error("BAD_ARGS", "fileId must be an integer");
        return;
    }

    player_->remove_track(file_id);
    if (player_->track_count() == 0) {
        player_->clear_source_cache("all-tracks-removed");
        native_compositor_source_signature_.clear();
    }
    (void)RefreshPresentationPolicy("track-removed", false);
    spdlog::info("[VideoRendererPlugin] Removed track: file_id={}", file_id);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "removeTrack", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "removeTrack", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "removeTrack");
    }
}

void VideoRendererPlugin::SetTrackOffset(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!player_) {
        result->Error("NO_PLAYER", "Player not created");
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }

    int file_id = 0;
    int64_t offset_us = 0;
    auto it = args->find(flutter::EncodableValue("fileId"));
    if (it == args->end() || !read_int_arg(it->second, file_id)) {
        result->Error("BAD_ARGS", "fileId must be an integer");
        return;
    }
    it = args->find(flutter::EncodableValue("offsetUs"));
    if (it == args->end() || !read_int64_arg(it->second, offset_us)) {
        result->Error("BAD_ARGS", "offsetUs must be an integer");
        return;
    }

    player_->set_track_offset(file_id, offset_us);
    spdlog::info("[VideoRendererPlugin] setTrackOffset: file_id={}, offset_us={}", file_id, offset_us);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "setTrackOffset", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "setTrackOffset", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "setTrackOffset");
    }
}

void VideoRendererPlugin::SetLoopRange(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!player_) {
        result->Error("NO_PLAYER", "Player not created");
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }

    bool enabled = false;
    int64_t start_us = 0;
    int64_t end_us = 0;
    auto it = args->find(flutter::EncodableValue("enabled"));
    if (it != args->end() && !read_bool_arg(it->second, enabled)) {
        result->Error("BAD_ARGS", "enabled must be a boolean");
        return;
    }
    it = args->find(flutter::EncodableValue("startUs"));
    if (it != args->end() && !read_int64_arg(it->second, start_us)) {
        result->Error("BAD_ARGS", "startUs must be an integer");
        return;
    }
    it = args->find(flutter::EncodableValue("endUs"));
    if (it != args->end() && !read_int64_arg(it->second, end_us)) {
        result->Error("BAD_ARGS", "endUs must be an integer");
        return;
    }
    if (auto validation = vr::validate_loop_range(enabled, start_us, end_us);
        !validation.ok) {
        result->Error("BAD_ARGS", validation.message);
        return;
    }

    player_->set_loop_range(enabled, start_us, end_us);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "setLoopRange", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "setLoopRange", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "setLoopRange");
    }
}

void VideoRendererPlugin::SetAudibleTrack(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    auto it = args->find(flutter::EncodableValue("fileId"));
    int file_id = -1;
    if (it == args->end() || !read_int_arg(it->second, file_id)) {
        result->Error("BAD_ARGS", "fileId must be an integer");
        return;
    }
    player_->set_audible_track(file_id);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "setAudibleTrack", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "setAudibleTrack", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "setAudibleTrack");
    }
}

void VideoRendererPlugin::Play(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    player_->play();
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "play", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "play");
    }
}

void VideoRendererPlugin::Pause(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    player_->pause();
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "pause", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "pause");
    }
}

void VideoRendererPlugin::Seek(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    auto it = args->find(flutter::EncodableValue("ptsUs"));
    int64_t pts = 0;
    if (it == args->end() || !read_int64_arg(it->second, pts) || pts < 0) {
        result->Error("BAD_ARGS", "ptsUs must be a non-negative integer");
        return;
    }
    int64_t request_id = -1;
    it = args->find(flutter::EncodableValue("requestId"));
    if (it != args->end() && !read_int64_arg(it->second, request_id)) {
        result->Error("BAD_ARGS", "requestId must be an integer");
        return;
    }
    spdlog::info("[VideoRendererPlugin] seek: pts={}us request_id={}", pts, request_id);
    player_->seek(pts, vr::SeekType::Exact, request_id);
    spdlog::info("[VideoRendererPlugin] seek completed request_id={}", request_id);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "seek", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "seek", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "seek");
    }
}

void VideoRendererPlugin::Resize(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    int w = 1920;
    int h = 1080;
    auto it = args->find(flutter::EncodableValue("width"));
    if (it != args->end() && !read_int_arg(it->second, w)) {
        result->Error("BAD_ARGS", "width must be an integer");
        return;
    }
    it = args->find(flutter::EncodableValue("height"));
    if (it != args->end() && !read_int_arg(it->second, h)) {
        result->Error("BAD_ARGS", "height must be an integer");
        return;
    }
    if (auto validation = vr::validate_renderer_dimensions(w, h, "viewport size");
        !validation.ok) {
        result->Error("BAD_ARGS", validation.message);
        return;
    }
    spdlog::info(
        "[WindowsCompositorDebug] method resize viewport={}x{} "
        "native_compositor={} player={}",
        w, h, native_compositor_ ? "yes" : "no",
        player_ ? "yes" : "no");
    player_->resize(w, h);
    spdlog::info(
        "[WindowsCompositorDebug] method resize complete viewport={}x{}",
        w, h);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "resize", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "resize", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "resize");
    }
}

void VideoRendererPlugin::SetNativeCompositorViewportRect(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (!arguments || !native_compositor_) {
            result->Success();
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (!args) {
            result->Error("BAD_ARGS", "viewport rect must be a map");
            return;
        }
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        int surface_width = 0;
        int surface_height = 0;
        const auto read = [&](const char* key, int& value) {
            auto it = args->find(flutter::EncodableValue(key));
            return it != args->end() && read_int_arg(it->second, value);
        };
        if (!read("left", left) || !read("top", top) ||
            !read("width", width) || !read("height", height) ||
            !read("surfaceWidth", surface_width) ||
            !read("surfaceHeight", surface_height) ||
            surface_width <= 0 || surface_height <= 0) {
            result->Error("BAD_ARGS", "invalid viewport rect");
            return;
        }
        spdlog::info(
            "[WindowsCompositorDebug] method viewportRect "
            "physical=({},{} {}x{}) surface={}x{}",
            left, top, width, height, surface_width, surface_height);
        native_compositor_->SetViewportRect(
            static_cast<double>(left) / surface_width,
            static_cast<double>(top) / surface_height,
            static_cast<double>(left + width) / surface_width,
            static_cast<double>(top + height) / surface_height);
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "setNativeCompositorViewportRect", e);
    }
}

void VideoRendererPlugin::PrepareNativeCompositorSourceCache(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (!player_ || !native_compositor_) {
            result->Success();
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (!args) {
            result->Error("BAD_ARGS", "source cache payload must be a map");
            return;
        }
        const auto read_int_list = [&](const char* key,
                                       std::vector<int>& out,
                                       size_t expected = 0) {
            const auto it = args->find(flutter::EncodableValue(key));
            if (it == args->end() ||
                !std::holds_alternative<flutter::EncodableList>(it->second)) {
                return false;
            }
            const auto& list = std::get<flutter::EncodableList>(it->second);
            if (expected != 0 && list.size() != expected) {
                return false;
            }
            out.clear();
            for (const auto& value : list) {
                int parsed = 0;
                if (!read_int_arg(value, parsed)) {
                    return false;
                }
                out.push_back(parsed);
            }
            return true;
        };
        const auto read_double_list = [&](const char* key,
                                          std::array<double, 4>& out) {
            const auto it = args->find(flutter::EncodableValue(key));
            if (it == args->end() ||
                !std::holds_alternative<flutter::EncodableList>(it->second)) {
                return false;
            }
            const auto& list = std::get<flutter::EncodableList>(it->second);
            if (list.size() != out.size()) {
                return false;
            }
            for (size_t i = 0; i < out.size(); ++i) {
                if (!read_double_arg(list[i], out[i]) ||
                    !std::isfinite(out[i])) {
                    return false;
                }
            }
            return true;
        };
        const auto read_required_int = [&](const char* key, int& out) {
            const auto it = args->find(flutter::EncodableValue(key));
            return it != args->end() && read_int_arg(it->second, out);
        };
        const auto read_required_double = [&](const char* key, double& out) {
            const auto it = args->find(flutter::EncodableValue(key));
            return it != args->end() && read_double_arg(it->second, out) &&
                   std::isfinite(out);
        };

        std::vector<int> source_slots;
        std::vector<int> source_order;
        std::array<double, 4> display_offset_x{};
        std::array<double, 4> display_offset_y{};
        std::array<double, 4> inv_display_size_x{};
        std::array<double, 4> inv_display_size_y{};
        std::array<double, 4> view_offset_uv_x{};
        std::array<double, 4> view_offset_uv_y{};
        int mode = 0;
        int active_track_count = 0;
        double split_pos = 0.5;
        if (!read_int_list("sourceSlots", source_slots) ||
            source_slots.empty() || source_slots.size() > 4 ||
            !read_int_list("sourceOrder", source_order, 4) ||
            !read_required_int("mode", mode) ||
            !read_required_double("splitPos", split_pos) ||
            !read_required_int("activeTrackCount", active_track_count) ||
            !read_double_list("displayOffsetX", display_offset_x) ||
            !read_double_list("displayOffsetY", display_offset_y) ||
            !read_double_list("invDisplaySizeX", inv_display_size_x) ||
            !read_double_list("invDisplaySizeY", inv_display_size_y) ||
            !read_double_list("viewOffsetUvX", view_offset_uv_x) ||
            !read_double_list("viewOffsetUvY", view_offset_uv_y) ||
            (mode != vr::LAYOUT_SIDE_BY_SIDE &&
             mode != vr::LAYOUT_SPLIT_SCREEN) ||
            split_pos < 0.0 || split_pos > 1.0 ||
            active_track_count < 1 || active_track_count > 4) {
            result->Error("BAD_ARGS", "invalid source cache projection payload");
            return;
        }

        std::array<bool, 4> requested{};
        for (const int slot : source_slots) {
            if (slot < 0 || slot >= 4 || requested[slot]) {
                result->Error("BAD_ARGS", "sourceSlots must be unique slots 0..3");
                return;
            }
            requested[slot] = true;
        }
        for (int index = 0; index < active_track_count; ++index) {
            const int slot = source_order[static_cast<size_t>(index)];
            if (slot < 0 || slot >= 4 || !requested[slot]) {
                result->Error("BAD_ARGS", "sourceOrder references an unavailable slot");
                return;
            }
        }

        std::vector<vr::SourceCacheTrackDescriptor> descriptors;
        const auto infos = player_->track_infos();
        for (const auto& info : infos) {
            if (info.slot >= 0 && info.slot < 4 && requested[info.slot]) {
                if (info.file_id < 0 || info.width <= 0 || info.height <= 0) {
                    result->Error("BAD_ARGS", "source track dimensions are invalid");
                    return;
                }
                descriptors.push_back(
                    {info.slot,
                     info.file_id,
                     info.width,
                     info.height,
                     info.color.transfer});
            }
        }
        if (descriptors.size() != source_slots.size()) {
            result->Error("BAD_ARGS", "sourceSlots do not match native tracks");
            return;
        }
        std::sort(
            descriptors.begin(),
            descriptors.end(),
            [](const auto& left, const auto& right) {
                return left.slot < right.slot;
            });

        WindowsNativeCompositor::SourceProjection projection;
        projection.enabled = true;
        projection.mode = mode;
        projection.split_pos = static_cast<float>(split_pos);
        projection.active_track_count = active_track_count;
        for (size_t i = 0; i < 4; ++i) {
            projection.source_order[i] = source_order[i];
            projection.display_offset_x[i] =
                static_cast<float>(display_offset_x[i]);
            projection.display_offset_y[i] =
                static_cast<float>(display_offset_y[i]);
            projection.inv_display_size_x[i] =
                static_cast<float>(inv_display_size_x[i]);
            projection.inv_display_size_y[i] =
                static_cast<float>(inv_display_size_y[i]);
            projection.view_offset_uv_x[i] =
                static_cast<float>(view_offset_uv_x[i]);
            projection.view_offset_uv_y[i] =
                static_cast<float>(view_offset_uv_y[i]);
        }
        native_compositor_->SetSourceProjection(projection);

        std::string signature = "R16G16B16A16_FLOAT|";
        for (const int slot : source_order) {
            signature += std::to_string(slot) + ",";
        }
        for (const auto& descriptor : descriptors) {
            signature += "|" + std::to_string(descriptor.slot) + ":" +
                         std::to_string(descriptor.file_id) + ":" +
                         std::to_string(descriptor.width) + "x" +
                         std::to_string(descriptor.height) + ":" +
                         std::to_string(descriptor.color_transfer);
        }
        if (signature != native_compositor_source_signature_) {
            if (!player_->configure_source_cache(descriptors)) {
                const std::string error =
                    "source-cache-configuration-failed";
                player_->clear_source_cache(error.c_str());
                native_compositor_->ClearSourceProjection(error);
                native_compositor_->SetSourceCacheError(error);
                native_compositor_source_signature_.clear();
                result->Success();
                return;
            }
            native_compositor_source_signature_ = signature;
            player_->set_source_cache_frame_callback(
                [compositor = native_compositor_.get()]() {
                    if (compositor) {
                        compositor->NotifySourceCachePublished();
                    }
                });
        }
        if (!player_->is_playing()) {
            const auto backend = player_->presentation_backend_diagnostics();
            if (backend.source_cache_publish_count == 0) {
                (void)player_->request_frame_refresh(
                    "windows-source-cache-subscribe");
            }
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "prepareNativeCompositorSourceCache", e);
    }
}

void VideoRendererPlugin::ClearNativeCompositorSourceCache(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        std::string reason = "clear-requested";
        if (const auto* args =
                std::get_if<flutter::EncodableMap>(arguments)) {
            const auto it = args->find(flutter::EncodableValue("reason"));
            if (it != args->end()) {
                (void)read_string_arg(it->second, reason);
            }
        }
        if (player_) {
            player_->clear_source_cache(reason.c_str());
        }
        if (native_compositor_) {
            native_compositor_->ClearSourceProjection(reason);
        }
        native_compositor_source_signature_.clear();
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "clearNativeCompositorSourceCache", e);
    }
}

void VideoRendererPlugin::SetNativeAnalysisOverlay(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (!arguments) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (!args) {
            result->Error("INVALID_ARGS", "Arguments must be a map");
            return;
        }
        const auto read_required_bool = [&](const char* key, int32_t& out) {
            const auto it = args->find(flutter::EncodableValue(key));
            bool value = false;
            if (it == args->end() ||
                !read_bool_arg(it->second, value)) {
                return false;
            }
            out = value ? 1 : 0;
            return true;
        };
        const auto read_required_int = [&](const char* key, int32_t& out) {
            const auto it = args->find(flutter::EncodableValue(key));
            int value = 0;
            if (it == args->end() ||
                !read_int_arg(it->second, value)) {
                return false;
            }
            out = value;
            return true;
        };
        NakiOverlayState state{};
        if (!read_required_bool("showCuGrid", state.show_cu_grid) ||
            !read_required_bool("showPredMode", state.show_pred_mode) ||
            !read_required_bool(
                "showQpHeatmap", state.show_qp_heatmap) ||
            !read_required_bool("showPredLines", state.show_pred_lines) ||
            !read_required_bool(
                "showCuBitCostHeatmap",
                state.show_cu_bit_cost_heatmap) ||
            !read_required_int(
                "opacityPermille", state.opacity_permille) ||
            !read_required_int("mode", state.mode) ||
            !read_required_int("trackFileId", state.track_file_id)) {
            result->Error(
                "BAD_ARGS", "Invalid native analysis overlay state");
            return;
        }
        const auto tracks_it =
            args->find(flutter::EncodableValue("tracks"));
        const auto* tracks =
            tracks_it == args->end()
                ? nullptr
                : std::get_if<flutter::EncodableList>(
                      &tracks_it->second);
        if (!tracks) {
            result->Error("BAD_ARGS", "tracks must be a list");
            return;
        }
        naki_analysis_clear_overlay_tracks();
        for (const auto& value : *tracks) {
            const auto* track =
                std::get_if<flutter::EncodableMap>(&value);
            if (!track) {
                result->Error("BAD_ARGS", "track must be a map");
                return;
            }
            const auto file_it =
                track->find(flutter::EncodableValue("fileId"));
            const auto path_it =
                track->find(flutter::EncodableValue("analysisPath"));
            int file_id = -1;
            std::string analysis_path;
            if (file_it == track->end() ||
                !read_int_arg(file_it->second, file_id) ||
                path_it == track->end() ||
                !read_string_arg(path_it->second, analysis_path) ||
                file_id < 0 || analysis_path.empty() ||
                naki_analysis_set_overlay_track(
                    file_id, analysis_path.c_str()) == 0) {
                naki_analysis_clear_overlay_tracks();
                result->Error(
                    "BAD_ARGS",
                    "Invalid native analysis overlay track");
                return;
            }
        }
        naki_analysis_set_overlay(&state);
        if (player_) {
            (void)player_->request_frame_refresh(
                "windows-analysis-overlay-state");
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "setNativeAnalysisOverlay", e);
    }
}

void VideoRendererPlugin::RequestNativeCompositorFlutterFrame(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        std::string reason = "unspecified";
        if (arguments) {
            const auto* args = std::get_if<flutter::EncodableMap>(arguments);
            if (args) {
                auto it = args->find(flutter::EncodableValue("reason"));
                if (it != args->end()) {
                    (void)read_string_arg(it->second, reason);
                }
            }
        }
        if (native_compositor_) {
            native_compositor_->RequestFlutterFrame(reason);
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "requestNativeCompositorFlutterFrame", e);
    }
}

void VideoRendererPlugin::AckNativeCompositorFlutterState(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (!arguments || !native_compositor_) {
            result->Success();
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (!args) {
            result->Error("BAD_ARGS", "ACK must be a map");
            return;
        }
        int64_t serial = 0;
        bool transparent = false;
        auto serial_it = args->find(flutter::EncodableValue("serial"));
        auto transparent_it =
            args->find(flutter::EncodableValue("transparentViewport"));
        if (serial_it == args->end() ||
            !read_int64_arg(serial_it->second, serial) ||
            transparent_it == args->end() ||
            !read_bool_arg(transparent_it->second, transparent)) {
            result->Error("BAD_ARGS", "serial and transparentViewport required");
            return;
        }
        native_compositor_->AcknowledgeFlutterState(
            static_cast<uint64_t>(serial), transparent);
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "ackNativeCompositorFlutterState", e);
    }
}

void VideoRendererPlugin::DebugFailNativeCompositor(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (!native_compositor_) {
            result->Error(
                "NOT_ACTIVE", "Windows native compositor is not available");
            return;
        }
        std::string reason = "ui-test-forced-failure";
        if (arguments) {
            const auto* args = std::get_if<flutter::EncodableMap>(arguments);
            if (!args) {
                result->Error("BAD_ARGS", "failure arguments must be a map");
                return;
            }
            const auto it = args->find(flutter::EncodableValue("reason"));
            if (it != args->end()) {
                const auto* value = std::get_if<std::string>(&it->second);
                if (!value || value->empty()) {
                    result->Error(
                        "BAD_ARGS", "reason must be a non-empty string");
                    return;
                }
                reason = *value;
            }
        }
        native_compositor_->ForceFailureForTesting(reason);
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "debugFailNativeCompositor", e);
    }
}

void VideoRendererPlugin::DebugSimulateWindowsDeviceLoss(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (!arguments) {
            result->Error("BAD_ARGS", "device-loss arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (!args) {
            result->Error("BAD_ARGS", "device-loss arguments must be a map");
            return;
        }
        const auto target_it =
            args->find(flutter::EncodableValue("target"));
        if (target_it == args->end()) {
            result->Error("BAD_ARGS", "target is required");
            return;
        }
        const auto* target_value =
            std::get_if<std::string>(&target_it->second);
        if (!target_value || target_value->empty()) {
            result->Error("BAD_ARGS", "target must be a non-empty string");
            return;
        }
        const std::string target = *target_value;
        if (target != "presentation" && target != "compositor" &&
            target != "transport" && target != "source-cache") {
            result->Error("BAD_ARGS", "unknown device-loss target");
            return;
        }
        std::string reason = "debug-simulated-device-loss";
        const auto reason_it =
            args->find(flutter::EncodableValue("reason"));
        if (reason_it != args->end()) {
            const auto* reason_value =
                std::get_if<std::string>(&reason_it->second);
            if (!reason_value || reason_value->empty()) {
                result->Error("BAD_ARGS", "reason must be a non-empty string");
                return;
            }
            reason = *reason_value;
        }

        const auto start = std::chrono::steady_clock::now();
        const long removed_reason =
            static_cast<long>(DXGI_ERROR_DEVICE_RESET);
        device_recovery_.state =
            vr::WindowsDeviceRecoveryState::DeviceLostDetected;
        ++device_recovery_.generation;
        ++device_recovery_.attempt_count;
        device_recovery_.last_reason = reason + ":" + target;
        device_recovery_.last_removed_reason =
            vr::windows_hresult_hex(static_cast<HRESULT>(removed_reason));
        device_recovery_.fallback_stage = "none";
        device_recovery_.preserved_player = player_ != nullptr;
        device_recovery_.preserved_track_count =
            player_ ? static_cast<int>(player_->track_infos().size()) : 0;
        device_recovery_.last_frame_held = true;

        bool recovered = false;
        if (target == "presentation" || target == "source-cache") {
            if (!require_player(player_, result.get())) {
                return;
            }
            device_recovery_.state =
                vr::WindowsDeviceRecoveryState::RebuildingPresentation;
            if (target == "source-cache") {
                player_->clear_source_cache("debug-device-loss-source-cache");
            }
            recovered = player_->recover_presentation_device_loss(
                reason.c_str(), removed_reason);
            if (recovered) {
                player_->request_frame_refresh("windows-device-recovery");
            }
        } else {
            if (!native_compositor_) {
                result->Error(
                    "NOT_ACTIVE",
                    "Windows native compositor is not available");
                return;
            }
            device_recovery_.state =
                vr::WindowsDeviceRecoveryState::RebuildingPresentation;
            recovered =
                native_compositor_->BeginDeviceRecovery(reason, removed_reason);
            if (recovered && player_) {
                player_->request_frame_refresh("windows-device-recovery");
            }
        }

        device_recovery_.last_duration_ms =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        if (recovered) {
            device_recovery_.state =
                vr::WindowsDeviceRecoveryState::Recovered;
            ++device_recovery_.success_count;
        } else {
            device_recovery_.state =
                target == "presentation"
                    ? vr::WindowsDeviceRecoveryState::FallbackNativeSdr
                    : vr::WindowsDeviceRecoveryState::FailedTerminal;
            ++device_recovery_.failure_count;
            device_recovery_.fallback_stage =
                target == "presentation" ? "native-sdr"
                                         : "native-compositor-failed";
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(
            result.get(), "debugSimulateWindowsDeviceLoss", e);
    }
}

void VideoRendererPlugin::ResetNativePerfCounters(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        if (native_compositor_) {
            native_compositor_->SetHighRefreshDisplayHz(
                query_window_refresh_hz(window_handle_));
            native_compositor_->ResetHighRefreshMetrics();
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "resetNativePerfCounters", e);
    }
}

void VideoRendererPlugin::BeginNativeInteractionSample(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        std::string label = "interaction";
        if (arguments) {
            const auto* args = std::get_if<flutter::EncodableMap>(arguments);
            if (!args) {
                result->Error("BAD_ARGS", "sample arguments must be a map");
                return;
            }
            const auto it = args->find(flutter::EncodableValue("label"));
            if (it != args->end() && !read_string_arg(it->second, label)) {
                result->Error("BAD_ARGS", "label must be a string");
                return;
            }
        }
        if (native_compositor_) {
            native_compositor_->SetHighRefreshDisplayHz(
                query_window_refresh_hz(window_handle_));
            native_compositor_->BeginInteractionSample(label);
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "beginNativeInteractionSample", e);
    }
}

void VideoRendererPlugin::EndNativeInteractionSample(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    try {
        std::string label = "interaction";
        if (arguments) {
            const auto* args = std::get_if<flutter::EncodableMap>(arguments);
            if (!args) {
                result->Error("BAD_ARGS", "sample arguments must be a map");
                return;
            }
            const auto it = args->find(flutter::EncodableValue("label"));
            if (it != args->end() && !read_string_arg(it->second, label)) {
                result->Error("BAD_ARGS", "label must be a string");
                return;
            }
        }
        if (native_compositor_) {
            native_compositor_->EndInteractionSample(label);
        }
        result->Success();
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "endNativeInteractionSample", e);
    }
}

void VideoRendererPlugin::SetViewportBackgroundColor(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    auto it = args->find(flutter::EncodableValue("color"));
    int64_t raw = 0;
    if (it == args->end() || !read_int64_arg(it->second, raw)) {
        result->Error("BAD_ARGS", "color must be an integer");
        return;
    }
    const uint32_t color = static_cast<uint32_t>(raw);
    const float a = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
    const float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(color & 0xFF) / 255.0f;
    player_->set_background_color(r, g, b, a);
    if (native_compositor_) {
        native_compositor_->SetViewportBackgroundColor(color);
    }
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "setViewportBackgroundColor", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "setViewportBackgroundColor", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "setViewportBackgroundColor");
    }
}

void VideoRendererPlugin::SetSpeed(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    auto it = args->find(flutter::EncodableValue("speed"));
    double speed = 0.0;
    if (it == args->end() || !read_double_arg(it->second, speed)) {
        result->Error("BAD_ARGS", "speed must be a number");
        return;
    }
    if (auto validation = vr::validate_playback_speed(speed); !validation.ok) {
        result->Error("BAD_ARGS", validation.message);
        return;
    }
    player_->set_speed(speed);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "setSpeed", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "setSpeed", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "setSpeed");
    }
}

void VideoRendererPlugin::StepForward(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    player_->step_forward();
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "stepForward", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "stepForward");
    }
}

void VideoRendererPlugin::StepBackward(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    player_->step_backward();
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "stepBackward", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "stepBackward");
    }
}

void VideoRendererPlugin::CurrentPts(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    int64_t pts = player_ ? player_->current_pts_us() : 0;
    result->Success(flutter::EncodableValue(pts));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "currentPts", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "currentPts");
    }
}

void VideoRendererPlugin::CurrentPresentedFrame(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    auto it = args->find(flutter::EncodableValue("fileId"));
    int file_id = -1;
    if (it == args->end() || !read_int_arg(it->second, file_id)) {
        result->Error("BAD_ARGS", "fileId must be an integer");
        return;
    }
    vr::TrackPerfStats selected_stats{};
    bool found = false;
    if (player_) {
        for (const auto& stats : player_->track_perf_stats()) {
            if (stats.file_id == file_id) {
                selected_stats = stats;
                found = true;
                break;
            }
        }
    }
    flutter::EncodableMap frame;
    frame[flutter::EncodableValue("ptsUs")] =
        enc_i64(found ? selected_stats.current_pts_us : -1);
    frame[flutter::EncodableValue("dtsUs")] =
        enc_i64(found ? selected_stats.current_dts_us : std::numeric_limits<int64_t>::min());
    frame[flutter::EncodableValue("analysisFrameIndex")] =
        enc_i32(found ? selected_stats.analysis_frame_index : -1);
    frame[flutter::EncodableValue("frameIdentityMode")] =
        enc_i32(found ? static_cast<int32_t>(selected_stats.frame_identity_mode) : 0);
    frame[flutter::EncodableValue("sourcePacketIndex")] =
        enc_i32(found ? selected_stats.source_packet_index : -1);
    frame[flutter::EncodableValue("sourcePacketSize")] =
        enc_i32(found ? selected_stats.source_packet_size : 0);
    frame[flutter::EncodableValue("sourcePacketPos")] =
        enc_i64(found ? selected_stats.source_packet_pos : -1);
    frame[flutter::EncodableValue("sourcePacketPtsUs")] =
        enc_i64(found ? selected_stats.source_packet_pts : std::numeric_limits<int64_t>::min());
    frame[flutter::EncodableValue("sourcePacketDtsUs")] =
        enc_i64(found ? selected_stats.source_packet_dts : std::numeric_limits<int64_t>::min());
    result->Success(flutter::EncodableValue(frame));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "currentPresentedFrame", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "currentPresentedFrame", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "currentPresentedFrame");
    }
}

void VideoRendererPlugin::Duration(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    int64_t dur = player_ ? player_->duration_us() : 0;
    result->Success(flutter::EncodableValue(dur));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "duration", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "duration");
    }
}

void VideoRendererPlugin::IsPlaying(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    bool playing = player_ ? player_->is_playing() : false;
    result->Success(flutter::EncodableValue(playing));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "isPlaying", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "isPlaying");
    }
}

void VideoRendererPlugin::GetPlaybackSnapshot(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    bool include_presented_frames = false;
    if (arguments) {
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (!args) {
            result->Error("INVALID_ARGS", "Arguments must be a map");
            return;
        }
        auto it = args->find(flutter::EncodableValue("includePresentedFrames"));
        if (it != args->end() && !read_bool_arg(it->second, include_presented_frames)) {
            result->Error("BAD_ARGS", "includePresentedFrames must be a boolean");
            return;
        }
    }

    flutter::EncodableMap snapshot;
    snapshot[flutter::EncodableValue("currentPtsUs")] =
        enc_i64(player_ ? player_->current_pts_us() : 0);
    snapshot[flutter::EncodableValue("durationUs")] =
        enc_i64(player_ ? player_->duration_us() : 0);
    snapshot[flutter::EncodableValue("isPlaying")] =
        flutter::EncodableValue(player_ ? player_->is_playing() : false);

    if (include_presented_frames) {
        flutter::EncodableList frames;
        if (player_) {
            for (const auto& stats : player_->track_perf_stats()) {
                frames.push_back(flutter::EncodableValue(make_presented_frame_map(stats)));
            }
        }
        snapshot[flutter::EncodableValue("presentedFrames")] =
            flutter::EncodableValue(frames);
    }

    result->Success(flutter::EncodableValue(snapshot));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "getPlaybackSnapshot", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "getPlaybackSnapshot", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "getPlaybackSnapshot");
    }
}

void VideoRendererPlugin::ApplyLayout(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!require_player(player_, result.get())) {
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }
    vr::LayoutState ls;
    auto it = args->find(flutter::EncodableValue("mode"));
    if (it != args->end() && !read_int_arg(it->second, ls.mode)) {
        result->Error("BAD_ARGS", "mode must be an integer");
        return;
    }
    if (ls.mode != vr::LAYOUT_SIDE_BY_SIDE && ls.mode != vr::LAYOUT_SPLIT_SCREEN) {
        result->Error("BAD_ARGS", "Invalid layout mode");
        return;
    }
    it = args->find(flutter::EncodableValue("splitPos"));
    double double_arg = 0.0;
    if (it != args->end()) {
        if (!read_double_arg(it->second, double_arg) || !std::isfinite(double_arg)) {
            result->Error("BAD_ARGS", "splitPos must be a finite number");
            return;
        }
        ls.split_pos = static_cast<float>(double_arg);
    }
    it = args->find(flutter::EncodableValue("zoomRatio"));
    if (it != args->end()) {
        if (!read_double_arg(it->second, double_arg) ||
            !std::isfinite(double_arg) ||
            double_arg <= 0.0) {
            result->Error("BAD_ARGS", "zoomRatio must be a positive finite number");
            return;
        }
        ls.zoom_ratio = static_cast<float>(double_arg);
    }
    it = args->find(flutter::EncodableValue("viewOffsetX"));
    if (it != args->end()) {
        if (!read_double_arg(it->second, double_arg) || !std::isfinite(double_arg)) {
            result->Error("BAD_ARGS", "viewOffsetX must be a finite number");
            return;
        }
        ls.view_offset[0] = static_cast<float>(double_arg);
    }
    it = args->find(flutter::EncodableValue("viewOffsetY"));
    if (it != args->end()) {
        if (!read_double_arg(it->second, double_arg) || !std::isfinite(double_arg)) {
            result->Error("BAD_ARGS", "viewOffsetY must be a finite number");
            return;
        }
        ls.view_offset[1] = static_cast<float>(double_arg);
    }
    it = args->find(flutter::EncodableValue("pixelSizeMode"));
    if (it != args->end() && !read_int_arg(it->second, ls.pixel_size_mode)) {
        result->Error("BAD_ARGS", "pixelSizeMode must be an integer");
        return;
    }
    if (ls.pixel_size_mode != vr::PIXEL_SIZE_UNIFORM_VIDEO_PIXELS &&
        ls.pixel_size_mode != vr::PIXEL_SIZE_FILL_VIEW) {
        result->Error("BAD_ARGS", "Invalid pixel size mode");
        return;
    }
    it = args->find(flutter::EncodableValue("order"));
    if (it != args->end()) {
        if (!std::holds_alternative<flutter::EncodableList>(it->second)) {
            result->Error("BAD_ARGS", "order must be a list");
            return;
        }
        const auto& order_list = std::get<flutter::EncodableList>(it->second);
        for (size_t i = 0; i < 4 && i < order_list.size(); ++i) {
            if (!read_int_arg(order_list[i], ls.order[i])) {
                result->Error("BAD_ARGS", "order entries must be integers");
                return;
            }
        }
    }
    if (auto validation = vr::validate_layout_state(ls); !validation.ok) {
        result->Error("BAD_ARGS", validation.message);
        return;
    }
    player_->apply_layout(ls);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "applyLayout", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "applyLayout", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "applyLayout");
    }
}

void VideoRendererPlugin::GetTracks(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    flutter::EncodableList tracks_list;
    if (player_) {
        for (const auto& info : player_->track_infos()) {
            tracks_list.push_back(flutter::EncodableValue(make_track_map(info)));
        }
    }
    result->Success(flutter::EncodableValue(tracks_list));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "getTracks", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "getTracks");
    }
}

vr::WindowsDisplayProbeSnapshot
VideoRendererPlugin::RefreshPresentationPolicy(
    const char* trigger,
    bool allow_transient_hold) {
    const auto display = display_probe_tracker_.Update(
        display_resolver_.Probe(window_handle_, dxgi_adapter_.Get()));
    if (display.changed) {
        spdlog::info(
            "[WindowsDisplayProbe] generation={} changes={} reason={} "
            "status={} output={} color_space={} hdr={} rect={}x{}+{},{} "
            "matches_presentation_adapter={}",
            display.generation,
            display.change_count,
            display.last_change_reason,
            display.probe.status,
            display.probe.device_name,
            display.probe.color_space,
            display.probe.hdr_active,
            display.probe.desktop_width,
            display.probe.desktop_height,
            display.probe.desktop_left,
            display.probe.desktop_top,
            display.probe.matches_presentation_adapter);
    }
    const bool has_hdr_track =
        player_ &&
        vr::windows_tracks_have_hdr_transfer(player_->track_infos());
    auto next = vr::resolve_windows_presentation_policy(
        presentation_request_, has_hdr_track, display.probe);
    if (allow_transient_hold && player_ &&
        presentation_policy_.native_compositor_requested &&
        !display.probe.output_resolved) {
        next = presentation_policy_;
        next.has_hdr_track = has_hdr_track;
        next.reason = "auto-transient-display-probe-hold";
    }

    const bool policy_changed =
        next.mode != presentation_policy_.mode ||
        next.reason != presentation_policy_.reason ||
        next.has_hdr_track != presentation_policy_.has_hdr_track;
    presentation_policy_ = next;
    if (!player_ || !presentation_policy_.native_compositor_requested ||
        !native_compositor_) {
        return display;
    }

    const double white_nits =
        presentation_policy_.hdr_output_requested
            ? static_cast<double>(
                  display.probe.sdr_white_level_milli_nits) /
                  1000.0
            : 80.0;
    const int64_t white_milli_nits =
        static_cast<int64_t>(std::llround(white_nits * 1000.0));
    const bool white_changed =
        white_milli_nits !=
        presentation_locked_sdr_white_level_milli_nits_;
    const bool display_changed =
        display.generation != presentation_locked_display_generation_;
    if (policy_changed || white_changed || display_changed) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> output_adapter;
        if (!display_resolver_.OpenAdapterForProbe(
                display.probe, &output_adapter)) {
            output_adapter = dxgi_adapter_;
        }
        native_compositor_->RequestOutputTarget(
            presentation_policy_.hdr_output_requested
                ? WindowsNativeCompositor::OutputTarget::ScRGB
                : WindowsNativeCompositor::OutputTarget::SDR,
            output_adapter.Get(),
            white_nits,
            display.generation,
            presentation_policy_.reason);
        if (player_->update_presentation_sdr_white_level(white_nits)) {
            (void)player_->request_frame_refresh(
                "windows-presentation-policy-refresh");
        }
        presentation_locked_display_generation_ = display.generation;
        presentation_locked_sdr_white_level_milli_nits_ =
            white_milli_nits;
        presentation_sdr_white_level_status_ =
            presentation_policy_.hdr_output_requested
                ? display.probe.sdr_white_level_status
                : "nominal-default";
        spdlog::info(
            "[WindowsPresentationAuto] trigger={} request={} mode={} "
            "reason={} hdr_track={} display_generation={} white_nits={:.3f}",
            trigger ? trigger : "unknown",
            presentation_policy_.request,
            presentation_policy_.mode,
            presentation_policy_.reason,
            presentation_policy_.has_hdr_track,
            display.generation,
            white_nits);
    }
    return display;
}

void VideoRendererPlugin::GetDiagnostics(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    const auto display =
        RefreshPresentationPolicy("get-diagnostics");
    auto diagnostics =
        diagnostics_.BuildMethodChannelDiagnostics(
            player_,
            display,
            presentation_policy_,
            presentation_sdr_white_level_status_);
    const auto event_diagnostics = event_bridge_.diagnostics();
    diagnostics[flutter::EncodableValue("nativeEventListenCount")] =
        enc_i64(event_diagnostics.listen_count);
    diagnostics[flutter::EncodableValue("nativeEventEmitCount")] =
        enc_i64(event_diagnostics.emit_count);
    diagnostics[flutter::EncodableValue("nativeEventDropNoSinkCount")] =
        enc_i64(event_diagnostics.drop_no_sink_count);
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryState")] =
        flutter::EncodableValue(vr::windows_device_recovery_state_name(
            device_recovery_.state));
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryGeneration")] =
        enc_i64(static_cast<int64_t>(device_recovery_.generation));
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryAttemptCount")] =
        enc_i64(static_cast<int64_t>(device_recovery_.attempt_count));
    diagnostics[flutter::EncodableValue("windowsDeviceRecoverySuccessCount")] =
        enc_i64(static_cast<int64_t>(device_recovery_.success_count));
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryFailureCount")] =
        enc_i64(static_cast<int64_t>(device_recovery_.failure_count));
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryLastReason")] =
        flutter::EncodableValue(device_recovery_.last_reason);
    diagnostics[flutter::EncodableValue(
        "windowsDeviceRecoveryLastRemovedReason")] =
        flutter::EncodableValue(device_recovery_.last_removed_reason);
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryLastDurationMs")] =
        enc_i64(static_cast<int64_t>(device_recovery_.last_duration_ms));
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryFallbackStage")] =
        flutter::EncodableValue(device_recovery_.fallback_stage);
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryPreservedPlayer")] =
        flutter::EncodableValue(device_recovery_.preserved_player);
    diagnostics[flutter::EncodableValue(
        "windowsDeviceRecoveryPreservedTrackCount")] =
        enc_i64(device_recovery_.preserved_track_count);
    diagnostics[flutter::EncodableValue("windowsDeviceRecoveryLastFrameHeld")] =
        flutter::EncodableValue(device_recovery_.last_frame_held);
    if (native_compositor_) {
        native_compositor_->SetHighRefreshDisplayHz(
            query_window_refresh_hz(window_handle_));
        native_compositor_->RequestDiagnosticCapture();
        const auto compositor = native_compositor_->diagnostics();
        if (compositor.device_recovery_attempt_count > 0) {
            diagnostics[flutter::EncodableValue("windowsDeviceRecoveryState")] =
                flutter::EncodableValue(compositor.device_recovery_state);
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryGeneration")] =
                enc_i64(static_cast<int64_t>(
                    compositor.device_recovery_generation));
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryAttemptCount")] =
                enc_i64(static_cast<int64_t>(
                    compositor.device_recovery_attempt_count));
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoverySuccessCount")] =
                enc_i64(static_cast<int64_t>(
                    compositor.device_recovery_success_count));
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryFailureCount")] =
                enc_i64(static_cast<int64_t>(
                    compositor.device_recovery_failure_count));
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryLastReason")] =
                flutter::EncodableValue(
                    compositor.device_recovery_last_reason);
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryLastRemovedReason")] =
                flutter::EncodableValue(
                    compositor.device_recovery_last_removed_reason);
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryLastDurationMs")] =
                enc_i64(static_cast<int64_t>(
                    compositor.device_recovery_last_duration_ms));
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryFallbackStage")] =
                flutter::EncodableValue(
                    compositor.device_recovery_fallback_stage);
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryPreservedPlayer")] =
                flutter::EncodableValue(
                    compositor.device_recovery_preserved_player);
            diagnostics[flutter::EncodableValue(
                "windowsDeviceRecoveryLastFrameHeld")] =
                flutter::EncodableValue(
                    compositor.device_recovery_last_frame_held);
        }
        diagnostics[flutter::EncodableValue("windowsNativeCompositorPhase")] =
            flutter::EncodableValue(compositor.phase);
        diagnostics[flutter::EncodableValue("windowsNativeCompositorStateSerial")] =
            enc_i64(static_cast<int64_t>(compositor.state_serial));
        diagnostics[flutter::EncodableValue("windowsNativeCompositorAckSerial")] =
            enc_i64(static_cast<int64_t>(compositor.ack_serial));
        diagnostics[flutter::EncodableValue("windowsFlutterExportGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.flutter_generation));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportStateGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_state_generation));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportRingGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_ring_generation));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportPublishCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_publish_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportRequestCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_request_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportRequestDispatchCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_request_dispatch_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportScheduleFrameCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_schedule_frame_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportVsyncCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_vsync_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportPresentCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_present_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportBeginCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_begin_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportBeginFailCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_begin_fail_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportMakeCurrentFailCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_make_current_fail_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportPublishFailCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_publish_fail_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportBackpressureCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_backpressure_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportPendingFramePumpFrames")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_pending_frame_pump_frames));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportStaleTimeoutCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_export_stale_timeout_count));
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportLatestAvailable")] =
            flutter::EncodableValue(
                compositor.flutter_export_latest_available);
        diagnostics[flutter::EncodableValue(
            "windowsFlutterExportFramePumpAvailable")] =
            flutter::EncodableValue(
                compositor.engine_export_frame_pump_available);
        diagnostics[flutter::EncodableValue("windowsVideoRingGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.video_generation));
        diagnostics[flutter::EncodableValue("windowsDCompCompositeCount")] =
            enc_i64(static_cast<int64_t>(compositor.composite_count));
        diagnostics[flutter::EncodableValue("windowsDCompPresentCount")] =
            enc_i64(static_cast<int64_t>(compositor.present_count));
        diagnostics[flutter::EncodableValue("windowsDCompDropCount")] =
            enc_i64(static_cast<int64_t>(compositor.drop_count));
        diagnostics[flutter::EncodableValue("windowsDCompFailureCount")] =
            enc_i64(static_cast<int64_t>(compositor.failure_count));
        diagnostics[flutter::EncodableValue("windowsHighRefreshGateSupported")] =
            flutter::EncodableValue(compositor.high_refresh_gate_supported);
        diagnostics[flutter::EncodableValue("windowsHighRefreshDisplayHz")] =
            enc_i64(compositor.high_refresh_display_hz);
        diagnostics[flutter::EncodableValue("windowsDCompPresentIntervalP95Us")] =
            enc_i64(compositor.dcomp_present_interval_p95_us);
        diagnostics[flutter::EncodableValue("windowsDCompCompositeP95Us")] =
            enc_i64(compositor.dcomp_composite_p95_us);
        diagnostics[flutter::EncodableValue("windowsDCompAcquireWaitP95Us")] =
            enc_i64(compositor.dcomp_acquire_wait_p95_us);
        diagnostics[flutter::EncodableValue(
            "windowsInteractionInputToPresentP95Us")] =
            enc_i64(compositor.interaction_input_to_present_p95_us);
        diagnostics[flutter::EncodableValue("windowsDCompDropRateX1000")] =
            enc_i64(compositor.dcomp_drop_rate_x1000);
        diagnostics[flutter::EncodableValue(
            "windowsSourceProjectionReuseCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.source_projection_reuse_count));
        diagnostics[flutter::EncodableValue(
            "windowsViewportRedrawDuringProjectionCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.viewport_redraw_during_projection_count));
        diagnostics[flutter::EncodableValue("windowsOverlayLayerRasterCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_raster_count));
        diagnostics[flutter::EncodableValue("windowsOverlayLayerUploadCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_upload_count));
        diagnostics[flutter::EncodableValue("windowsOverlayLayerReuseCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_reuse_count));
        diagnostics[flutter::EncodableValue("windowsOverlayRetainedLayerActive")] =
            flutter::EncodableValue(compositor.overlay_retained_layer_active);
        diagnostics[flutter::EncodableValue("windowsOverlayRetainedLayerMode")] =
            flutter::EncodableValue(compositor.overlay_layer_mode);
        diagnostics[flutter::EncodableValue("windowsOverlayLayerTextureCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_texture_count));
        diagnostics[flutter::EncodableValue("windowsOverlayLayerBytes")] =
            enc_i64(static_cast<int64_t>(compositor.overlay_layer_bytes));
        diagnostics[flutter::EncodableValue("windowsOverlayLayerGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_generation));
        diagnostics[flutter::EncodableValue(
            "windowsOverlayLayerCommittedGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_committed_generation));
        diagnostics[flutter::EncodableValue(
            "windowsOverlayLayerPendingGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_pending_generation));
        diagnostics[flutter::EncodableValue(
            "windowsOverlayLayerCompositeCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_composite_count));
        diagnostics[flutter::EncodableValue("windowsOverlayLayerMissCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_miss_count));
        diagnostics[flutter::EncodableValue(
            "windowsOverlayLayerBackpressureCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_layer_backpressure_count));
        diagnostics[flutter::EncodableValue(
            "windowsOverlayLayerFallbackReason")] =
            flutter::EncodableValue(compositor.overlay_layer_fallback_reason);
        diagnostics[flutter::EncodableValue("windowsOverlayLayerLastError")] =
            flutter::EncodableValue(compositor.overlay_layer_last_error);
        diagnostics[flutter::EncodableValue("windowsOverlayCompositeP95Us")] =
            enc_i64(compositor.overlay_composite_p95_us);
        diagnostics[flutter::EncodableValue("windowsOverlayLayerCompositeP95Us")] =
            enc_i64(compositor.overlay_composite_p95_us);
        diagnostics[flutter::EncodableValue("windowsOverlayLayerRasterP95Us")] =
            enc_i64(compositor.overlay_raster_p95_us);
        diagnostics[flutter::EncodableValue("windowsOverlayLayerUploadP95Us")] =
            enc_i64(compositor.overlay_upload_p95_us);
        diagnostics[flutter::EncodableValue("windowsHighRefreshGateLastResult")] =
            flutter::EncodableValue(compositor.high_refresh_gate_last_result);
        diagnostics[flutter::EncodableValue("windowsHotPathActive")] =
            flutter::EncodableValue(compositor.hot_path_active);
        diagnostics[flutter::EncodableValue("windowsHotPathMode")] =
            flutter::EncodableValue(compositor.hot_path_mode);
        diagnostics[flutter::EncodableValue("windowsHotPathDisplayHz")] =
            enc_i64(compositor.hot_path_display_hz);
        diagnostics[flutter::EncodableValue("windowsHotPathFrameBudgetUs")] =
            enc_i64(compositor.hot_path_frame_budget_us);
        diagnostics[flutter::EncodableValue(
            "windowsHotPathPresentIntervalP95Us")] =
            enc_i64(compositor.hot_path_present_interval_p95_us);
        diagnostics[flutter::EncodableValue("windowsHotPathCompositeP95Us")] =
            enc_i64(compositor.hot_path_composite_p95_us);
        diagnostics[flutter::EncodableValue(
            "windowsHotPathAcquireWaitP95Us")] =
            enc_i64(compositor.hot_path_acquire_wait_p95_us);
        diagnostics[flutter::EncodableValue(
            "windowsHotPathInputToPresentP95Us")] =
            enc_i64(compositor.hot_path_input_to_present_p95_us);
        diagnostics[flutter::EncodableValue("windowsHotPathDropRateX1000")] =
            enc_i64(compositor.hot_path_drop_rate_x1000);
        diagnostics[flutter::EncodableValue(
            "windowsHotPathProjectionOnlyUpdateCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hot_path_projection_only_update_count));
        diagnostics[flutter::EncodableValue(
            "windowsHotPathViewportRedrawDuringProjectionCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hot_path_viewport_redraw_during_projection_count));
        diagnostics[flutter::EncodableValue(
            "windowsHotPathSourceCacheReuseCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hot_path_source_cache_reuse_count));
        diagnostics[flutter::EncodableValue(
            "windowsHotPathOverlayReuseCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hot_path_overlay_reuse_count));
        diagnostics[flutter::EncodableValue(
            "windowsHotPathOverlayRasterCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hot_path_overlay_raster_count));
        diagnostics[flutter::EncodableValue(
            "windowsHotPathOverlayUploadCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hot_path_overlay_upload_count));
        diagnostics[flutter::EncodableValue(
            "windowsHotPathLastFailureReason")] =
            flutter::EncodableValue(compositor.hot_path_last_failure_reason);
        diagnostics[flutter::EncodableValue("windowsHotPathGateResult")] =
            flutter::EncodableValue(compositor.hot_path_gate_result);
        diagnostics[flutter::EncodableValue("windowsDCompResizeCount")] =
            enc_i64(static_cast<int64_t>(compositor.resize_count));
        diagnostics[flutter::EncodableValue("windowsDCompDiagnosticCaptureCount")] =
            enc_i64(static_cast<int64_t>(compositor.diagnostic_capture_count));
        diagnostics[flutter::EncodableValue("windowsFlutterAlphaAverageX1000")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_alpha_average_x1000));
        diagnostics[flutter::EncodableValue("windowsFlutterTransparentPixelsX1000")] =
            enc_i64(static_cast<int64_t>(
                compositor.flutter_transparent_pixels_x1000));
        diagnostics[flutter::EncodableValue("windowsDCompFinalMaxRGBX1000")] =
            enc_i64(static_cast<int64_t>(compositor.final_max_rgb_x1000));
        diagnostics[flutter::EncodableValue("windowsDCompFinalPixelsOver1")] =
            enc_i64(static_cast<int64_t>(compositor.final_pixels_over_1));
        diagnostics[flutter::EncodableValue("windowsDCompSwapChainWidth")] =
            enc_i64(static_cast<int64_t>(compositor.swap_chain_width));
        diagnostics[flutter::EncodableValue("windowsDCompSwapChainHeight")] =
            enc_i64(static_cast<int64_t>(compositor.swap_chain_height));
        diagnostics[flutter::EncodableValue("windowsFlutterExportAvailable")] =
            flutter::EncodableValue(compositor.engine_export_available);
        diagnostics[flutter::EncodableValue("windowsDCompSwapChainActive")] =
            flutter::EncodableValue(compositor.swap_chain_active);
        diagnostics[flutter::EncodableValue("windowsDCompSwapChainFormat")] =
            flutter::EncodableValue(compositor.swap_chain_format);
        diagnostics[flutter::EncodableValue("windowsDCompColorSpace")] =
            flutter::EncodableValue(compositor.color_space);
        diagnostics[flutter::EncodableValue("windowsPresentationProducerAdapterLuid")] =
            flutter::EncodableValue(compositor.producer_adapter_luid);
        diagnostics[flutter::EncodableValue("windowsPresentationOutputAdapterLuid")] =
            flutter::EncodableValue(compositor.output_adapter_luid);
        diagnostics[flutter::EncodableValue("windowsPresentationPendingOutputAdapterLuid")] =
            flutter::EncodableValue(compositor.pending_output_adapter_luid);
        diagnostics[flutter::EncodableValue("windowsPresentationCrossAdapterSupported")] =
            flutter::EncodableValue(compositor.cross_adapter_supported);
        diagnostics[flutter::EncodableValue("windowsPresentationCrossAdapterActive")] =
            flutter::EncodableValue(compositor.cross_adapter_required);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportMode")] =
            flutter::EncodableValue(compositor.cross_adapter_transport_mode);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportStatus")] =
            flutter::EncodableValue(compositor.cross_adapter_transport_status);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSyncKind")] =
            flutter::EncodableValue(compositor.cross_adapter_sync_kind);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterRequestedSyncKind")] =
            flutter::EncodableValue(compositor.cross_adapter_requested_sync_kind);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterActiveSyncKind")] =
            flutter::EncodableValue(compositor.cross_adapter_active_sync_kind);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSyncFallbackReason")] =
            flutter::EncodableValue(compositor.cross_adapter_sync_fallback_reason);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterLastError")] =
            flutter::EncodableValue(compositor.cross_adapter_last_error);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterBGRA8Supported")] =
            flutter::EncodableValue(compositor.transport_bgra8_supported);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterFP16Supported")] =
            flutter::EncodableValue(compositor.transport_fp16_supported);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceSupported")] =
            flutter::EncodableValue(compositor.transport_shared_fence_supported);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceProducerSupported")] =
            flutter::EncodableValue(compositor.transport_shared_fence_producer_supported);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceOutputSupported")] =
            flutter::EncodableValue(compositor.transport_shared_fence_output_supported);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceHandleCreated")] =
            flutter::EncodableValue(compositor.transport_shared_fence_handle_created);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceOpenSucceeded")] =
            flutter::EncodableValue(compositor.transport_shared_fence_open_succeeded);
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.transport_generation));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportCopyCount")] =
            enc_i64(static_cast<int64_t>(compositor.transport_copy_count));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportCopyBytes")] =
            enc_i64(static_cast<int64_t>(compositor.transport_copy_bytes));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportTimeoutCount")] =
            enc_i64(static_cast<int64_t>(compositor.transport_timeout_count));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportLastCopyUs")] =
            enc_i64(static_cast<int64_t>(compositor.transport_last_copy_us));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterTransportTotalCopyUs")] =
            enc_i64(static_cast<int64_t>(compositor.transport_total_copy_us));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceSignalCount")] =
            enc_i64(static_cast<int64_t>(compositor.shared_fence_signal_count));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceWaitCount")] =
            enc_i64(static_cast<int64_t>(compositor.shared_fence_wait_count));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceTimeoutCount")] =
            enc_i64(static_cast<int64_t>(compositor.shared_fence_timeout_count));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceLastWaitUs")] =
            enc_i64(static_cast<int64_t>(compositor.shared_fence_last_wait_us));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSharedFenceP95WaitUs")] =
            enc_i64(static_cast<int64_t>(compositor.shared_fence_p95_wait_us));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterEventQueryP95WaitUs")] =
            enc_i64(static_cast<int64_t>(compositor.event_query_p95_wait_us));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterFlutterConsumedGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.flutter_transport_generation));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterVideoConsumedGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.video_transport_generation));
        diagnostics[flutter::EncodableValue("windowsCrossAdapterSourceConsumedGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.source_transport_generation));
        diagnostics[flutter::EncodableValue("windowsPresentationOutputMigrationCount")] =
            enc_i64(static_cast<int64_t>(compositor.output_migration_count));
        diagnostics[flutter::EncodableValue("windowsPresentationOutputMigrationFailureCount")] =
            enc_i64(static_cast<int64_t>(compositor.output_migration_failure_count));
        diagnostics[flutter::EncodableValue("windowsDCompBufferCount")] =
            flutter::EncodableValue(3);
        diagnostics[flutter::EncodableValue(
            "windowsDCompColorSpaceSupported")] =
            flutter::EncodableValue(
                compositor.color_space_supported);
        diagnostics[flutter::EncodableValue(
            "windowsDCompSDRToneMapActive")] =
            flutter::EncodableValue(
                compositor.sdr_tone_map_active);
        diagnostics[flutter::EncodableValue(
            "windowsPresentationTransitionState")] =
            flutter::EncodableValue(
                compositor.transition_state);
        diagnostics[flutter::EncodableValue(
            "windowsPresentationTransitionSerial")] =
            enc_i64(static_cast<int64_t>(
                compositor.transition_serial));
        diagnostics[flutter::EncodableValue(
            "windowsPresentationTransitionReason")] =
            flutter::EncodableValue(
                compositor.transition_reason);
        diagnostics[flutter::EncodableValue(
            "windowsPresentationOutputGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.output_generation));
        diagnostics[flutter::EncodableValue(
            "windowsPresentationHDRPromotionCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hdr_promotion_count));
        diagnostics[flutter::EncodableValue(
            "windowsPresentationHDRDemotionCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.hdr_demotion_count));
        diagnostics[flutter::EncodableValue(
            "windowsPresentationTargetFallbackCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.target_fallback_count));
        diagnostics[flutter::EncodableValue(
            "windowsPresentationLockedDisplayGeneration")] =
            enc_i64(static_cast<int64_t>(
                presentation_locked_display_generation_));
        diagnostics[flutter::EncodableValue(
            "windowsPresentationLockedSDRWhiteLevelMilliNits")] =
            enc_i64(
                presentation_locked_sdr_white_level_milli_nits_);
        diagnostics[flutter::EncodableValue("windowsNativeCompositorFallbackReason")] =
            flutter::EncodableValue(compositor.fallback_reason);
        const auto backend =
            player_ ? player_->presentation_backend_diagnostics()
                    : vr::PresentationBackendDiagnostics{};
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceProjectionEnabled")] =
            flutter::EncodableValue(compositor.source_projection_enabled);
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceCacheActive")] =
            flutter::EncodableValue(compositor.source_cache_active);
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceCacheTextureCount")] =
            enc_i64(backend.source_cache_texture_count);
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceCacheGeneration")] =
            enc_i64(static_cast<int64_t>(backend.source_cache_generation));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceCacheBytes")] =
            enc_i64(static_cast<int64_t>(backend.source_cache_bytes));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceCacheLastError")] =
            flutter::EncodableValue(
                compositor.source_cache_last_error != "none"
                    ? compositor.source_cache_last_error
                    : backend.source_cache_last_error);
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceCacheHz")] =
            flutter::EncodableValue(compositor.source_cache_hz);
        const auto anchor =
            player_ ? player_->presented_anchor_diagnostics()
                    : vr::RendererPresentedAnchorDiagnostics{};
        diagnostics[flutter::EncodableValue(
            "nativeCompositorPresentedAnchorMode")] =
            flutter::EncodableValue(presented_anchor_mode_name(anchor.mode));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorPresentedAnchorGeneration")] =
            enc_i64(static_cast<int64_t>(anchor.generation));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorPresentedAnchorSourceCacheGeneration")] =
            enc_i64(static_cast<int64_t>(
                anchor.source_cache_ring_generation));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorPresentedAnchorSourceCacheFrameGeneration")] =
            enc_i64(static_cast<int64_t>(
                anchor.source_cache_frame_generation));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorPresentedAnchorUpdateCount")] =
            enc_i64(static_cast<int64_t>(anchor.update_count));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorPresentedAnchorStaleAfterSeek")] =
            flutter::EncodableValue(anchor.stale_after_seek);
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceProjectionHz")] =
            flutter::EncodableValue(compositor.source_projection_hz);
        diagnostics[flutter::EncodableValue(
            "nativeCompositorOverlayGeneration")] =
            enc_i64(static_cast<int64_t>(compositor.overlay_generation));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorOverlayFillRectCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_fill_rect_count));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorOverlayLineRectCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_line_rect_count));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorOverlayMotionLineCount")] =
            enc_i64(static_cast<int64_t>(
                compositor.overlay_motion_line_count));
        diagnostics[flutter::EncodableValue(
            "nativeCompositorSourceBakedOverlayDisabled")] =
            flutter::EncodableValue(true);
        diagnostics[flutter::EncodableValue("windowsSourceCacheFormat")] =
            flutter::EncodableValue(backend.source_cache_format);
        diagnostics[flutter::EncodableValue("windowsSourceCacheRingDepth")] =
            enc_i64(backend.source_cache_ring_depth);
        diagnostics[flutter::EncodableValue(
            "windowsSourceCacheFrozenSnapshot")] =
            flutter::EncodableValue(
                backend.source_cache_frozen_snapshot);
        diagnostics[flutter::EncodableValue("windowsSourceCachePublishCount")] =
            enc_i64(static_cast<int64_t>(
                backend.source_cache_publish_count));
        diagnostics[flutter::EncodableValue(
            "windowsSourceCachePresentedAnchorPublishCount")] =
            enc_i64(static_cast<int64_t>(
                anchor.source_cache_publish_count));
        diagnostics[flutter::EncodableValue(
            "windowsSourceCacheBackpressureCount")] =
            enc_i64(static_cast<int64_t>(
                backend.source_cache_backpressure_count));
        diagnostics[flutter::EncodableValue(
            "windowsSourceCacheConsumedGeneration")] =
            enc_i64(static_cast<int64_t>(
                compositor.source_cache_consumed_generation));
        diagnostics[flutter::EncodableValue(
            "windowsSourceCacheFallbackCount")] =
            enc_i64(static_cast<int64_t>(
                backend.source_cache_fallback_count +
                compositor.source_cache_fallback_count));
        const bool compositor_active =
            compositor.phase == "preparing" || compositor.phase == "active";
        diagnostics[flutter::EncodableValue("windowsPresentationCompositorActive")] =
            flutter::EncodableValue(compositor_active);
        if (compositor_active) {
            diagnostics[flutter::EncodableValue(
                "windowsPresentationMode")] =
                flutter::EncodableValue(
                    compositor.output_target == "scrgb"
                        ? "native-compositor-scrgb"
                        : "native-compositor-sdr");
        } else if (
            compositor.fallback_reason != "none") {
            diagnostics[flutter::EncodableValue("windowsPresentationMode")] =
                flutter::EncodableValue("native-compositor-failed");
            diagnostics[flutter::EncodableValue(
                "windowsPresentationFallbackReason")] =
                flutter::EncodableValue(compositor.fallback_reason);
        }
    }
    result->Success(flutter::EncodableValue(std::move(diagnostics)));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "getDiagnostics", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "getDiagnostics");
    }
}

void VideoRendererPlugin::PickFiles(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
        bool allow_multiple = true;

        if (arguments) {
            const auto* args = std::get_if<flutter::EncodableMap>(arguments);
            if (args) {
                auto it = args->find(flutter::EncodableValue("allowMultiple"));
                if (it != args->end()) {
                    if (!read_bool_arg(it->second, allow_multiple)) {
                        result->Error("BAD_ARGS", "allowMultiple must be a boolean");
                        return;
                    }
                }
            }
        }

        auto shared_result =
            std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
                std::move(result));
        HWND owner_window = window_handle_;
        auto platform_task_state = platform_task_state_;
        std::thread([allow_multiple, owner_window, platform_task_state, shared_result]() {
            std::vector<std::string> paths;
            std::string error_message;
            try {
                FilePickerService picker;
                paths = picker.PickVideoFiles(allow_multiple, owner_window);
            } catch (const std::exception& e) {
                error_message = e.what();
            } catch (...) {
                error_message = "Unknown native exception";
            }

            PostPlatformTaskToState(
                platform_task_state,
                [paths = std::move(paths),
                 error_message = std::move(error_message),
                 shared_result]() mutable {
                    auto& result_ref = *shared_result;
                    if (!result_ref) {
                        return;
                    }
                    if (!error_message.empty()) {
                        ReportMethodException(
                            result_ref.get(), "pickFiles", "NATIVE_EXCEPTION", error_message);
                        result_ref.reset();
                        return;
                    }

                    flutter::EncodableList paths_list;
                    for (const auto& path : paths) {
                        paths_list.push_back(flutter::EncodableValue(path));
                    }
                    // Always return a list (empty = cancelled, non-empty = selected files).
                    result_ref->Success(flutter::EncodableValue(paths_list));
                    result_ref.reset();
                });
        }).detach();
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "pickFiles", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "pickFiles", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "pickFiles");
    }
}

void VideoRendererPlugin::CaptureViewport(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!player_) {
        result->Error("NO_PLAYER", "Player not created");
        return;
    }

    std::string output_path;
    if (arguments) {
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (args) {
            auto it = args->find(flutter::EncodableValue("outputPath"));
            if (it != args->end() && !read_string_arg(it->second, output_path)) {
                result->Error("BAD_ARGS", "outputPath must be a string");
                return;
            }
        }
    }

    ViewportCaptureResult capture;
    const auto capture_status =
        viewport_capture_.Capture(*player_, output_path, capture);
    if (capture_status == ViewportCaptureStatus::CaptureFailed) {
        result->Error("CAPTURE_FAILED", "Failed to capture viewport");
        return;
    }
    if (capture_status == ViewportCaptureStatus::SaveFailed) {
        result->Error("CAPTURE_SAVE_FAILED", "Failed to save viewport PNG");
        return;
    }

    flutter::EncodableMap map;
    map[flutter::EncodableValue("hash")] = flutter::EncodableValue(capture.hash);
    map[flutter::EncodableValue("width")] = flutter::EncodableValue(capture.width);
    map[flutter::EncodableValue("height")] = flutter::EncodableValue(capture.height);
    map[flutter::EncodableValue("avgLuma")] = flutter::EncodableValue(capture.avg_luma);
    map[flutter::EncodableValue("nonBlackRatio")] =
        flutter::EncodableValue(capture.non_black_ratio);
    if (!capture.output_path.empty()) {
        map[flutter::EncodableValue("outputPath")] =
            flutter::EncodableValue(capture.output_path);
    }
    result->Success(flutter::EncodableValue(map));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "captureViewport", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "captureViewport", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "captureViewport");
    }
}

void VideoRendererPlugin::CaptureViewportRegion(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    if (!player_) {
        result->Error("NO_PLAYER", "Player not created");
        return;
    }
    if (!arguments) {
        result->Error("INVALID_ARGS", "Arguments required");
        return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(arguments);
    if (!args) {
        result->Error("INVALID_ARGS", "Arguments must be a map");
        return;
    }

    const auto read_required_int = [&](const char* key, int& value) -> bool {
        auto it = args->find(flutter::EncodableValue(key));
        return it != args->end() && read_int_arg(it->second, value);
    };

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int max_size = 0;
    if (!read_required_int("x", x) ||
        !read_required_int("y", y) ||
        !read_required_int("width", width) ||
        !read_required_int("height", height) ||
        !read_required_int("maxSize", max_size)) {
        result->Error("BAD_ARGS", "x, y, width, height and maxSize must be integers");
        return;
    }
    if (width <= 0 || height <= 0) {
        result->Error("BAD_ARGS", "width and height must be positive");
        return;
    }

    std::string output_path;
    auto output_it = args->find(flutter::EncodableValue("outputPath"));
    if (output_it != args->end() && !read_string_arg(output_it->second, output_path)) {
        result->Error("BAD_ARGS", "outputPath must be a string");
        return;
    }

    ViewportCaptureResult capture;
    const auto capture_status =
        viewport_capture_.CaptureRegion(
            *player_, x, y, width, height, max_size, output_path, capture);
    if (capture_status == ViewportCaptureStatus::CaptureFailed) {
        result->Error("CAPTURE_FAILED", "Failed to capture viewport region");
        return;
    }
    if (capture_status == ViewportCaptureStatus::SaveFailed) {
        result->Error("CAPTURE_SAVE_FAILED", "Failed to save viewport region PNG");
        return;
    }

    flutter::EncodableMap map;
    map[flutter::EncodableValue("hash")] = flutter::EncodableValue(capture.hash);
    map[flutter::EncodableValue("width")] = flutter::EncodableValue(capture.width);
    map[flutter::EncodableValue("height")] = flutter::EncodableValue(capture.height);
    map[flutter::EncodableValue("avgLuma")] = flutter::EncodableValue(capture.avg_luma);
    map[flutter::EncodableValue("nonBlackRatio")] =
        flutter::EncodableValue(capture.non_black_ratio);
    if (!capture.output_path.empty()) {
        map[flutter::EncodableValue("outputPath")] =
            flutter::EncodableValue(capture.output_path);
    }
    result->Success(flutter::EncodableValue(map));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "captureViewportRegion", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "captureViewportRegion", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "captureViewportRegion");
    }
}

void VideoRendererPlugin::CaptureWindow(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
        std::string output_path;
        if (arguments) {
            const auto* args = std::get_if<flutter::EncodableMap>(arguments);
            if (args) {
                auto it = args->find(flutter::EncodableValue("outputPath"));
                if (it != args->end() && !read_string_arg(it->second, output_path)) {
                    result->Error("BAD_ARGS", "outputPath must be a string");
                    return;
                }
            }
        }

        WindowCaptureResult capture;
        const auto capture_status =
            window_capture_.Capture(window_handle_, output_path, capture);
        if (capture_status == WindowCaptureStatus::InvalidWindow) {
            result->Error("NO_WINDOW", "Window handle is unavailable");
            return;
        }
        if (capture_status == WindowCaptureStatus::CaptureFailed) {
            result->Error("CAPTURE_FAILED", "Failed to capture window");
            return;
        }
        if (capture_status == WindowCaptureStatus::SaveFailed) {
            result->Error("CAPTURE_SAVE_FAILED", "Failed to save window PNG");
            return;
        }

        flutter::EncodableMap map;
        map[flutter::EncodableValue("hash")] = flutter::EncodableValue(capture.hash);
        map[flutter::EncodableValue("width")] = flutter::EncodableValue(capture.width);
        map[flutter::EncodableValue("height")] = flutter::EncodableValue(capture.height);
        map[flutter::EncodableValue("avgLuma")] = flutter::EncodableValue(capture.avg_luma);
        map[flutter::EncodableValue("nonBlackRatio")] =
            flutter::EncodableValue(capture.non_black_ratio);
        if (!capture.output_path.empty()) {
            map[flutter::EncodableValue("outputPath")] =
                flutter::EncodableValue(capture.output_path);
        }
        result->Success(flutter::EncodableValue(map));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "captureWindow", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "captureWindow", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "captureWindow");
    }
}

void VideoRendererPlugin::GetLayout(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    flutter::EncodableMap map;
    if (player_) {
        auto ls = player_->layout();
        map[flutter::EncodableValue("mode")] = flutter::EncodableValue(ls.mode);
        map[flutter::EncodableValue("splitPos")] =
            flutter::EncodableValue(static_cast<double>(ls.split_pos));
        map[flutter::EncodableValue("zoomRatio")] =
            flutter::EncodableValue(static_cast<double>(ls.zoom_ratio));
        map[flutter::EncodableValue("viewOffsetX")] =
            flutter::EncodableValue(static_cast<double>(ls.view_offset[0]));
        map[flutter::EncodableValue("viewOffsetY")] =
            flutter::EncodableValue(static_cast<double>(ls.view_offset[1]));
        map[flutter::EncodableValue("pixelSizeMode")] =
            flutter::EncodableValue(ls.pixel_size_mode);
        flutter::EncodableList order_list;
        for (int i = 0; i < 4; ++i) {
            order_list.push_back(flutter::EncodableValue(ls.order[i]));
        }
        map[flutter::EncodableValue("order")] = flutter::EncodableValue(order_list);
    }
    result->Success(flutter::EncodableValue(map));
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "getLayout", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "getLayout");
    }
}
