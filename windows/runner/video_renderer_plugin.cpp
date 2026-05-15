#include "video_renderer_plugin.h"
#include "analysis_ffi.h"

#include "video_renderer/layout/layout_validation.h"
#include "video_renderer/renderer_config_validation.h"
#include "utils.h"
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter_windows.h>
#include <spdlog/spdlog.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <exception>
#include <cmath>
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
    plugin->event_bridge_.RegisterDrainWindow();

    registrar->AddPlugin(std::move(plugin));
}

VideoRendererPlugin::VideoRendererPlugin(
    flutter::PluginRegistrarWindows*,
    flutter::TextureRegistrar* texture_registrar,
    IDXGIAdapter* dxgi_adapter)
    : texture_bridge_(texture_registrar),
      diagnostics_session_(std::make_shared<NativeDiagnosticsSession>()) {
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
    player_->resize(w, h);
    result->Success(flutter::EncodableValue(std::monostate{}));
    } catch (const std::bad_variant_access& e) {
        ReportMethodException(result.get(), "resize", e);
    } catch (const std::exception& e) {
        ReportMethodException(result.get(), "resize", e);
    } catch (...) {
        ReportUnknownMethodException(result.get(), "resize");
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

void VideoRendererPlugin::GetDiagnostics(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    try {
    result->Success(flutter::EncodableValue(
        diagnostics_.BuildMethodChannelDiagnostics(player_)));
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
