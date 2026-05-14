#include "video_renderer_plugin.h"
#include "analysis_ffi.h"

#include "video_renderer/layout_validation.h"
#include "video_renderer/renderer_config_validation.h"
#include "utils.h"
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter_windows.h>
#include <spdlog/spdlog.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <cwchar>
#include <exception>
#include <cmath>
#include <mutex>
#include <variant>
#include <limits>

namespace {
constexpr UINT kVideoRendererEventDrainMessage = WM_APP + 0x4B7;
constexpr wchar_t kVideoRendererEventWindowClass[] = L"VoidPlayerVideoRendererEvents";

LRESULT CALLBACK VideoRendererEventWindowProc(HWND hwnd,
                                              UINT message,
                                              WPARAM wparam,
                                              LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto* plugin = reinterpret_cast<VideoRendererPlugin*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kVideoRendererEventDrainMessage && plugin) {
        plugin->DrainEventQueue();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

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

std::shared_ptr<vr::NativePlayer> pin_global_player() {
    return GlobalNativePlayerRegistry().Pin();
}

void publish_global_player(const std::shared_ptr<vr::NativePlayer>& player) {
    GlobalNativePlayerRegistry().Publish(player);
}

void clear_global_player() {
    GlobalNativePlayerRegistry().Clear();
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
    diagnostics.FillFfiDiagnostics(d, pin_global_player());
    return &d;
}

// static
void VideoRendererPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "video_renderer",
        &flutter::StandardMethodCodec::GetInstance());
    auto event_channel = std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
        registrar->messenger(), "video_renderer/events",
        &flutter::StandardMethodCodec::GetInstance());

    auto* texture_registrar = registrar->texture_registrar();

    // Get DXGI adapter from the Flutter view
    IDXGIAdapter* adapter = nullptr;
    auto* view = registrar->GetView();
    if (view) {
        adapter = view->GetGraphicsAdapter();
    }

    auto plugin = std::make_unique<VideoRendererPlugin>(registrar, texture_registrar, adapter);

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
    plugin->RegisterEventDrainWindowProc();

    registrar->AddPlugin(std::move(plugin));
}

VideoRendererPlugin::VideoRendererPlugin(
    flutter::PluginRegistrarWindows*,
    flutter::TextureRegistrar* texture_registrar,
    IDXGIAdapter* dxgi_adapter)
    : texture_bridge_(texture_registrar) {
    if (dxgi_adapter) {
        dxgi_adapter->AddRef();
        dxgi_adapter_.Attach(dxgi_adapter);
    }

    const auto logging = logging_bootstrap_.InitializeDefaults();

    spdlog::info("[VideoRendererPlugin] Plugin constructed, native logging initialized: {}", logging.file_path);
    spdlog::info("[VideoRendererPlugin] Crash handler installed (VEH + SEH), crash dir: {}", logging.logs_dir);
    log_ffmpeg_runtime_versions();

    // Register PTS callback for analysis FFI (avoids analysis_ffi depending on NativePlayer)
    naki_analysis_register_pts_callback([]() -> int64_t {
        auto r = pin_global_player();
        return r ? r->current_pts_us() : 0;
    });
}

VideoRendererPlugin::~VideoRendererPlugin() {
    if (event_hwnd_) {
        DestroyWindow(event_hwnd_);
        event_hwnd_ = nullptr;
    }
    clear_global_player();
    texture_bridge_.DetachFrameCallback();
    if (player_) {
        player_->set_event_callback(nullptr);
        player_->shutdown();
    }
    texture_bridge_.Unregister();
    if (player_) {
        player_.reset();
    }
    ClearEventSink();
}

void VideoRendererPlugin::RegisterEventDrainWindowProc() {
    if (event_hwnd_) {
        return;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = VideoRendererEventWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kVideoRendererEventWindowClass;
    RegisterClassW(&wc);
    event_hwnd_ = CreateWindowExW(
        0,
        kVideoRendererEventWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    if (!event_hwnd_) {
        spdlog::warn("[VideoRendererPlugin] failed to create renderer event message window");
    }
}

void VideoRendererPlugin::SetEventSink(
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink) {
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_sink_ = std::move(sink);
    }
    DrainEventQueue();
}

void VideoRendererPlugin::ClearEventSink() {
    std::lock_guard<std::mutex> lock(event_mutex_);
    event_sink_.reset();
    pending_events_.clear();
}

void VideoRendererPlugin::QueueRendererEvent(const vr::RendererEvent& event) {
    flutter::EncodableMap payload;
    payload[flutter::EncodableValue("schemaVersion")] = flutter::EncodableValue(1);
    payload[flutter::EncodableValue("sequence")] =
        flutter::EncodableValue(event_sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
    payload[flutter::EncodableValue("timestampUs")] =
        flutter::EncodableValue(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
    switch (event.type) {
    case vr::RendererEvent::Type::SeekPreviewPresented:
        payload[flutter::EncodableValue("type")] =
            flutter::EncodableValue("seekPreviewPresented");
        break;
    case vr::RendererEvent::Type::TrackError:
        payload[flutter::EncodableValue("type")] =
            flutter::EncodableValue("trackError");
        break;
    }
    payload[flutter::EncodableValue("requestId")] = flutter::EncodableValue(event.request_id);
    payload[flutter::EncodableValue("trackFileId")] = flutter::EncodableValue(event.track_file_id);
    payload[flutter::EncodableValue("ptsUs")] = flutter::EncodableValue(event.pts_us);
    payload[flutter::EncodableValue("dtsUs")] = flutter::EncodableValue(event.dts_us);
    payload[flutter::EncodableValue("targetPtsUs")] = flutter::EncodableValue(event.target_pts_us);
    payload[flutter::EncodableValue("errorCode")] = flutter::EncodableValue(event.error_code);

    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (pending_events_.size() >= 256) {
            pending_events_.pop_front();
            spdlog::warn("[VideoRendererPlugin] renderer event queue overflow, dropped oldest event");
        }
        pending_events_.emplace_back(std::move(payload));
    }
    spdlog::debug("[VideoRendererPlugin] queued renderer event request_id={} file_id={}",
                  event.request_id, event.track_file_id);
    if (event_hwnd_) {
        PostMessage(event_hwnd_, kVideoRendererEventDrainMessage, 0, 0);
    }
}

void VideoRendererPlugin::DrainEventQueue() {
    for (;;) {
        flutter::EncodableValue event;
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            if (!event_sink_ || pending_events_.empty()) {
                return;
            }
            event = std::move(pending_events_.front());
            pending_events_.pop_front();
            spdlog::debug("[VideoRendererPlugin] draining renderer event");
            event_sink_->Success(event);
        }
    }
}

void VideoRendererPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    const auto& method = method_call.method_name();
    try {
    auto require_player = [&]() -> bool {
        if (!player_) {
            result->Error("NO_PLAYER", "Player not created");
            return false;
        }
        return true;
    };

    if (method == "initLogging") {
        InitLogging(method_call.arguments(), std::move(result));
    } else if (method == "createPlayer") {
        CreatePlayer(method_call.arguments(), std::move(result));
    } else if (method == "destroyPlayer") {
        DestroyPlayer(std::move(result));
    } else if (method == "addTrack") {
        AddTrack(method_call.arguments(), std::move(result));
    } else if (method == "removeTrack") {
        RemoveTrack(method_call.arguments(), std::move(result));
    } else if (method == "setTrackOffset") {
        SetTrackOffset(method_call.arguments(), std::move(result));
    } else if (method == "setLoopRange") {
        SetLoopRange(method_call.arguments(), std::move(result));
    } else if (method == "setAudibleTrack") {
        if (!require_player()) return;
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
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
    } else if (method == "play") {
        if (!require_player()) return;
        player_->play();
        result->Success(flutter::EncodableValue(std::monostate{}));
    } else if (method == "pause") {
        if (!require_player()) return;
        player_->pause();
        result->Success(flutter::EncodableValue(std::monostate{}));
    } else if (method == "seek") {
        if (!require_player()) return;
        if (!method_call.arguments()) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
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
    } else if (method == "resize") {
        if (!require_player()) return;
        if (!method_call.arguments()) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
        if (!args) {
            result->Error("INVALID_ARGS", "Arguments must be a map");
            return;
        }
        int w = 1920, h = 1080;
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
        player_->resize(w, h);
        result->Success(flutter::EncodableValue(std::monostate{}));
    } else if (method == "setViewportBackgroundColor") {
        if (!require_player()) return;
        if (!method_call.arguments()) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
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
        result->Success(flutter::EncodableValue(std::monostate{}));
    } else if (method == "setSpeed") {
        if (!require_player()) return;
        if (!method_call.arguments()) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
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
    } else if (method == "stepForward") {
        if (!require_player()) return;
        player_->step_forward();
        result->Success(flutter::EncodableValue(std::monostate{}));
    } else if (method == "stepBackward") {
        if (!require_player()) return;
        player_->step_backward();
        result->Success(flutter::EncodableValue(std::monostate{}));
    } else if (method == "currentPts") {
        int64_t pts = player_ ? player_->current_pts_us() : 0;
        result->Success(flutter::EncodableValue(pts));
    } else if (method == "currentPresentedFrame") {
        if (!method_call.arguments()) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
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
        int64_t pts = -1;
        int64_t dts = std::numeric_limits<int64_t>::min();
        if (player_) {
            for (const auto& stats : player_->track_perf_stats()) {
                if (stats.file_id == file_id) {
                    pts = stats.current_pts_us;
                    dts = stats.current_dts_us;
                    break;
                }
            }
        }
        flutter::EncodableMap frame;
        frame[flutter::EncodableValue("ptsUs")] = flutter::EncodableValue(pts);
        frame[flutter::EncodableValue("dtsUs")] = flutter::EncodableValue(dts);
        result->Success(flutter::EncodableValue(frame));
    } else if (method == "duration") {
        int64_t dur = player_ ? player_->duration_us() : 0;
        result->Success(flutter::EncodableValue(dur));
    } else if (method == "isPlaying") {
        bool playing = player_ ? player_->is_playing() : false;
        result->Success(flutter::EncodableValue(playing));
    } else if (method == "applyLayout") {
        if (!require_player()) return;
        if (!method_call.arguments()) {
            result->Error("INVALID_ARGS", "Arguments required");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
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
    } else if (method == "getTracks") {
        flutter::EncodableList tracks_list;
        if (player_) {
            for (const auto& info : player_->track_infos()) {
                tracks_list.push_back(flutter::EncodableValue(make_track_map(info)));
            }
        }
        result->Success(flutter::EncodableValue(tracks_list));
    } else if (method == "getDiagnostics") {
        // Use global player so stats window (secondary engine) can query directly.
        result->Success(flutter::EncodableValue(
            diagnostics_.BuildMethodChannelDiagnostics(pin_global_player())));
    } else if (method == "pickFiles") {
        PickFiles(method_call.arguments(), std::move(result));
    } else if (method == "captureViewport") {
        CaptureViewport(method_call.arguments(), std::move(result));
    } else if (method == "getLayout") {
        flutter::EncodableMap map;
        if (player_) {
            auto ls = player_->layout();
            map[flutter::EncodableValue("mode")] = flutter::EncodableValue(ls.mode);
            map[flutter::EncodableValue("splitPos")] = flutter::EncodableValue(static_cast<double>(ls.split_pos));
            map[flutter::EncodableValue("zoomRatio")] = flutter::EncodableValue(static_cast<double>(ls.zoom_ratio));
            map[flutter::EncodableValue("viewOffsetX")] = flutter::EncodableValue(static_cast<double>(ls.view_offset[0]));
            map[flutter::EncodableValue("viewOffsetY")] = flutter::EncodableValue(static_cast<double>(ls.view_offset[1]));
            map[flutter::EncodableValue("pixelSizeMode")] = flutter::EncodableValue(ls.pixel_size_mode);
            flutter::EncodableList order_list;
            for (int i = 0; i < 4; ++i) order_list.push_back(flutter::EncodableValue(ls.order[i]));
            map[flutter::EncodableValue("order")] = flutter::EncodableValue(order_list);
        }
        result->Success(flutter::EncodableValue(map));
    } else {
        result->NotImplemented();
    }
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), method, e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), method, e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), method);
    }
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
    publish_global_player(player_);
    if (!player_->initialize(config)) {
        clear_global_player();
        player_.reset();
        result->Error("INIT_FAILED", "Failed to initialize player");
        return;
    }

    if (!texture_bridge_.Register(player_)) {
        clear_global_player();
        player_->shutdown();
        player_.reset();
        result->Error("TEXTURE_FAILED", "Failed to register texture");
        return;
    }

    player_->set_event_callback([this](const vr::RendererEvent& event) {
        QueueRendererEvent(event);
    });

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
    if (player_) {
        clear_global_player();
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

    flutter::EncodableList paths_list;
    for (const auto& path : file_picker_.PickVideoFiles(allow_multiple)) {
        paths_list.push_back(flutter::EncodableValue(path));
    }

    // Always return a list (empty = cancelled, non-empty = selected files)
    result->Success(flutter::EncodableValue(paths_list));
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
