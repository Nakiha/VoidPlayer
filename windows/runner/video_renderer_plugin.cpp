#include "video_renderer_plugin.h"
#include "analysis_ffi.h"

#include "utils.h"
#include <flutter_windows.h>
#include <spdlog/spdlog.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <cstring>
#include <vector>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {
std::string get_exe_dir() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string dir(exe_path);
    auto last_sep = dir.find_last_of("\\/");
    if (last_sep != std::string::npos) dir = dir.substr(0, last_sep);
    return dir;
}

std::string sanitize_log_file_name(std::string name) {
    if (name.empty()) return name;
    for (auto& ch : name) {
        switch (ch) {
        case '\\':
        case '/':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            ch = '_';
            break;
        default:
            break;
        }
    }
    return name;
}

std::string current_process_role() {
    const char* command_line = GetCommandLineA();
    if (command_line && strstr(command_line, "--standalone-analysis") != nullptr) {
        return "analysis";
    }
    return "main";
}

std::string default_native_log_file_name() {
    std::ostringstream name;
    name << "native_" << current_process_role() << "_" << GetCurrentProcessId() << ".log";
    return name.str();
}

flutter::EncodableMap make_track_map(const vr::TrackInfo& info) {
    flutter::EncodableMap map;
    map[flutter::EncodableValue("fileId")] = flutter::EncodableValue(info.file_id);
    map[flutter::EncodableValue("slot")] = flutter::EncodableValue(info.slot);
    map[flutter::EncodableValue("path")] = flutter::EncodableValue(info.file_path);
    map[flutter::EncodableValue("width")] = flutter::EncodableValue(info.width);
    map[flutter::EncodableValue("height")] = flutter::EncodableValue(info.height);
    map[flutter::EncodableValue("durationUs")] = flutter::EncodableValue(static_cast<int64_t>(info.duration_us));
    return map;
}

std::string format_ffmpeg_version(unsigned version) {
    return std::to_string((version >> 16) & 0xFF) + "." +
           std::to_string((version >> 8) & 0xFF) + "." +
           std::to_string(version & 0xFF);
}

void log_ffmpeg_runtime_versions() {
    spdlog::info(
        "[FFmpeg] av_version_info={} avcodec={} avformat={} avutil={} swscale={} swresample={}",
        av_version_info(),
        format_ffmpeg_version(avcodec_version()),
        format_ffmpeg_version(avformat_version()),
        format_ffmpeg_version(avutil_version()),
        format_ffmpeg_version(swscale_version()),
        format_ffmpeg_version(swresample_version()));
}

std::wstring Utf16FromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length - 1));
    return wide;
}

std::string Fnv1a64Hex(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

struct CaptureStats {
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
};

CaptureStats ComputeCaptureStats(const std::vector<uint8_t>& bgra) {
    CaptureStats stats;
    const size_t pixel_count = bgra.size() / 4;
    if (pixel_count == 0) {
        return stats;
    }

    uint64_t luma_sum = 0;
    size_t non_black = 0;
    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t off = i * 4;
        const uint8_t b = bgra[off + 0];
        const uint8_t g = bgra[off + 1];
        const uint8_t r = bgra[off + 2];
        const int luma = (77 * static_cast<int>(r) +
                          150 * static_cast<int>(g) +
                          29 * static_cast<int>(b)) >> 8;
        luma_sum += static_cast<uint64_t>(luma);
        if (r > 8 || g > 8 || b > 8) {
            ++non_black;
        }
    }

    stats.avg_luma = static_cast<double>(luma_sum) / static_cast<double>(pixel_count);
    stats.non_black_ratio = static_cast<double>(non_black) / static_cast<double>(pixel_count);
    return stats;
}

bool SaveBgraToPng(const std::vector<uint8_t>& bgra, int width, int height, const std::string& path) {
    if (bgra.empty() || width <= 0 || height <= 0 || path.empty()) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        return false;
    }

    const auto wide_path = Utf16FromUtf8(path);
    hr = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) {
        return false;
    }
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) {
        return false;
    }
    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) {
        return false;
    }

    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) {
        return false;
    }

    const UINT stride = static_cast<UINT>(width * 4);
    const UINT image_size = stride * static_cast<UINT>(height);
    hr = frame->WritePixels(
        static_cast<UINT>(height), stride, image_size,
        const_cast<BYTE*>(bgra.data()));
    if (FAILED(hr)) {
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        return false;
    }
    hr = encoder->Commit();
    return SUCCEEDED(hr);
}
} // namespace

// Process-global renderer pointer for cross-engine access (e.g. stats window).
vr::Renderer* g_global_renderer = nullptr;

// ---- dart:ffi diagnostics export ----
static NakiVrDiagnostics g_diag_snapshot = {};

extern "C" __declspec(dllexport)
const NakiVrDiagnostics* naki_vr_get_diagnostics() {
    auto& d = g_diag_snapshot;
    std::memset(&d, 0, sizeof(d));

    auto* r = g_global_renderer;
    if (!r) return &d;

    d.playback_time_s = static_cast<double>(r->current_pts_us()) / 1e6;
    d.is_playing = r->is_playing() ? 1 : 0;

    auto stats = r->track_perf_stats();
    d.track_count = static_cast<int32_t>(stats.size());
    for (int i = 0; i < kMaxTracksFFI && i < static_cast<int>(stats.size()); ++i) {
        const auto& s = stats[i];
        d.tracks[i].slot            = s.slot;
        d.tracks[i].file_id         = s.file_id;
        d.tracks[i].fps             = s.fps;
        d.tracks[i].avg_decode_ms   = s.avg_decode_ms;
        d.tracks[i].max_decode_ms   = s.max_decode_ms;
        d.tracks[i].buffer_count    = static_cast<int32_t>(s.buffer_count);
        d.tracks[i].buffer_capacity = static_cast<int32_t>(s.buffer_capacity);
        d.tracks[i].buffer_state    = static_cast<int32_t>(s.buffer_state);
    }
    // Mark unused slots
    for (int i = static_cast<int>(stats.size()); i < kMaxTracksFFI; ++i) {
        d.tracks[i].slot = -1;
    }
    return &d;
}

// static
void VideoRendererPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "video_renderer",
        &flutter::StandardMethodCodec::GetInstance());

    auto* texture_registrar = registrar->texture_registrar();

    // Get DXGI adapter from the Flutter view
    IDXGIAdapter* adapter = nullptr;
    auto* view = registrar->GetView();
    if (view) {
        adapter = view->GetGraphicsAdapter();
    }

    auto plugin = std::make_unique<VideoRendererPlugin>(texture_registrar, adapter);

    channel->SetMethodCallHandler(
        [plugin_ptr = plugin.get()](const auto& call, auto result) {
            plugin_ptr->HandleMethodCall(call, std::move(result));
        });

    registrar->AddPlugin(std::move(plugin));
}

VideoRendererPlugin::VideoRendererPlugin(
    flutter::TextureRegistrar* texture_registrar,
    IDXGIAdapter* dxgi_adapter)
    : texture_registrar_(texture_registrar), dxgi_adapter_(dxgi_adapter) {
    // Initialize native logging with defaults on plugin construction.
    // This happens before any Dart-side initLogging call, so native logs
    // (including renderer creation) are always captured.
    logs_dir_ = get_exe_dir() + "\\logs";
    log_file_name_ = default_native_log_file_name();

    vr::LogConfig config;
    config.file_path = logs_dir_ + "\\" + log_file_name_;
    config.max_files = 5;
    vr::configure_logging(config);
    vr::install_crash_handler(logs_dir_);

    spdlog::info("[VideoRendererPlugin] Plugin constructed, native logging initialized: {}", config.file_path);
    spdlog::info("[VideoRendererPlugin] Crash handler installed (VEH + SEH), crash dir: {}", logs_dir_);
    log_ffmpeg_runtime_versions();

    // Register PTS callback for analysis FFI (avoids analysis_ffi depending on Renderer)
    naki_analysis_register_pts_callback([]() -> int64_t {
        return g_global_renderer ? g_global_renderer->current_pts_us() : 0;
    });
}

VideoRendererPlugin::~VideoRendererPlugin() {
    g_global_renderer = nullptr;
    if (texture_id_ >= 0 && texture_registrar_) {
        texture_registrar_->UnregisterTexture(texture_id_);
        texture_id_ = -1;
    }
    if (renderer_) {
        renderer_->shutdown();
        renderer_.reset();
    }
}

void VideoRendererPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    const auto& method = method_call.method_name();

    if (method == "initLogging") {
        InitLogging(method_call.arguments(), std::move(result));
    } else if (method == "createRenderer") {
        CreateRenderer(method_call.arguments(), std::move(result));
    } else if (method == "destroyRenderer") {
        DestroyRenderer(std::move(result));
    } else if (method == "addTrack") {
        AddTrack(method_call.arguments(), std::move(result));
    } else if (method == "removeTrack") {
        RemoveTrack(method_call.arguments(), std::move(result));
    } else if (method == "setTrackOffset") {
        SetTrackOffset(method_call.arguments(), std::move(result));
    } else if (method == "play") {
        if (renderer_) renderer_->play();
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "pause") {
        if (renderer_) renderer_->pause();
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "seek") {
        if (renderer_ && method_call.arguments()) {
            const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
            if (args) {
                auto it = args->find(flutter::EncodableValue("ptsUs"));
                if (it != args->end()) {
                    int64_t pts = 0;
                    // Handle both int (32-bit) and long (64-bit) from Dart
                    if (std::holds_alternative<int>(it->second)) {
                        pts = static_cast<int64_t>(std::get<int>(it->second));
                    } else if (std::holds_alternative<int64_t>(it->second)) {
                        pts = std::get<int64_t>(it->second);
                    }
                    spdlog::info("[VideoRendererPlugin] seek: pts={}us, renderer alive={}", pts, (bool)renderer_);
                    renderer_->seek(pts, vr::SeekType::Exact);
                    spdlog::info("[VideoRendererPlugin] seek completed");
                }
            }
        }
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "resize") {
        if (renderer_ && method_call.arguments()) {
            const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
            if (args) {
                int w = 1920, h = 1080;
                auto it = args->find(flutter::EncodableValue("width"));
                if (it != args->end()) w = std::get<int>(it->second);
                it = args->find(flutter::EncodableValue("height"));
                if (it != args->end()) h = std::get<int>(it->second);
                renderer_->resize(w, h);
            }
        }
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "setSpeed") {
        if (renderer_ && method_call.arguments()) {
            const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
            if (args) {
                auto it = args->find(flutter::EncodableValue("speed"));
                if (it != args->end()) {
                    double speed = std::get<double>(it->second);
                    renderer_->set_speed(speed);
                }
            }
        }
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "stepForward") {
        if (renderer_) renderer_->step_forward();
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "stepBackward") {
        if (renderer_) renderer_->step_backward();
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "currentPts") {
        int64_t pts = renderer_ ? renderer_->current_pts_us() : 0;
        result->Success(flutter::EncodableValue(pts));
    } else if (method == "duration") {
        int64_t dur = renderer_ ? renderer_->duration_us() : 0;
        result->Success(flutter::EncodableValue(dur));
    } else if (method == "isPlaying") {
        bool playing = renderer_ ? renderer_->is_playing() : false;
        result->Success(flutter::EncodableValue(playing));
    } else if (method == "applyLayout") {
        if (renderer_ && method_call.arguments()) {
            const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
            if (args) {
                vr::LayoutState ls;
                auto it = args->find(flutter::EncodableValue("mode"));
                if (it != args->end()) ls.mode = std::get<int>(it->second);
                it = args->find(flutter::EncodableValue("splitPos"));
                if (it != args->end()) ls.split_pos = static_cast<float>(std::get<double>(it->second));
                it = args->find(flutter::EncodableValue("zoomRatio"));
                if (it != args->end()) ls.zoom_ratio = static_cast<float>(std::get<double>(it->second));
                it = args->find(flutter::EncodableValue("viewOffsetX"));
                if (it != args->end()) ls.view_offset[0] = static_cast<float>(std::get<double>(it->second));
                it = args->find(flutter::EncodableValue("viewOffsetY"));
                if (it != args->end()) ls.view_offset[1] = static_cast<float>(std::get<double>(it->second));
                it = args->find(flutter::EncodableValue("order"));
                if (it != args->end()) {
                    const auto& order_list = std::get<flutter::EncodableList>(it->second);
                    for (size_t i = 0; i < 4 && i < order_list.size(); ++i) {
                        ls.order[i] = std::get<int>(order_list[i]);
                    }
                }
                renderer_->apply_layout(ls);
            }
        }
        result->Success(flutter::EncodableValue(nullptr));
    } else if (method == "getTracks") {
        flutter::EncodableList tracks_list;
        if (renderer_) {
            for (const auto& info : renderer_->track_infos()) {
                tracks_list.push_back(flutter::EncodableValue(make_track_map(info)));
            }
        }
        result->Success(flutter::EncodableValue(tracks_list));
    } else if (method == "getDiagnostics") {
        // Use global renderer so stats window (secondary engine) can query directly
        auto* diag_renderer = g_global_renderer;
        flutter::EncodableMap map;
        if (diag_renderer) {
            map[flutter::EncodableValue("playbackTime")] =
                flutter::EncodableValue(static_cast<double>(diag_renderer->current_pts_us()) / 1e6);
            map[flutter::EncodableValue("isPlaying")] =
                flutter::EncodableValue(diag_renderer->is_playing());

            flutter::EncodableList tracks_list;
            for (const auto& ts : diag_renderer->track_perf_stats()) {
                flutter::EncodableMap tm;
                tm[flutter::EncodableValue("slot")] = flutter::EncodableValue(ts.slot);
                tm[flutter::EncodableValue("fileId")] = flutter::EncodableValue(ts.file_id);

                tm[flutter::EncodableValue("fps")] = flutter::EncodableValue(ts.fps);
                tm[flutter::EncodableValue("avgDecodeMs")] = flutter::EncodableValue(ts.avg_decode_ms);
                tm[flutter::EncodableValue("maxDecodeMs")] = flutter::EncodableValue(ts.max_decode_ms);
                tm[flutter::EncodableValue("bufferCount")] = flutter::EncodableValue(static_cast<int>(ts.buffer_count));
                tm[flutter::EncodableValue("bufferCapacity")] = flutter::EncodableValue(static_cast<int>(ts.buffer_capacity));
                tm[flutter::EncodableValue("bufferState")] = flutter::EncodableValue(static_cast<int>(ts.buffer_state));
                tracks_list.push_back(flutter::EncodableValue(tm));
            }
            map[flutter::EncodableValue("tracks")] = flutter::EncodableValue(tracks_list);
        }
        result->Success(flutter::EncodableValue(map));
    } else if (method == "pickFiles") {
        PickFiles(method_call.arguments(), std::move(result));
    } else if (method == "captureViewport") {
        CaptureViewport(method_call.arguments(), std::move(result));
    } else if (method == "getLayout") {
        flutter::EncodableMap map;
        if (renderer_) {
            auto ls = renderer_->layout();
            map[flutter::EncodableValue("mode")] = flutter::EncodableValue(ls.mode);
            map[flutter::EncodableValue("splitPos")] = flutter::EncodableValue(static_cast<double>(ls.split_pos));
            map[flutter::EncodableValue("zoomRatio")] = flutter::EncodableValue(static_cast<double>(ls.zoom_ratio));
            map[flutter::EncodableValue("viewOffsetX")] = flutter::EncodableValue(static_cast<double>(ls.view_offset[0]));
            map[flutter::EncodableValue("viewOffsetY")] = flutter::EncodableValue(static_cast<double>(ls.view_offset[1]));
            flutter::EncodableList order_list;
            for (int i = 0; i < 4; ++i) order_list.push_back(flutter::EncodableValue(ls.order[i]));
            map[flutter::EncodableValue("order")] = flutter::EncodableValue(order_list);
        }
        result->Success(flutter::EncodableValue(map));
    } else {
        result->NotImplemented();
    }
}

void VideoRendererPlugin::InitLogging(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

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
                log_file_name = sanitize_log_file_name(std::get<std::string>(it->second));
            }
        }
    }

    // Parse level string to spdlog level
    spdlog::level::level_enum level = spdlog::level::from_str(level_str);

    vr::LogConfig config;
    if (!logs_dir.empty()) {
        logs_dir_ = logs_dir;
    }
    if (!log_file_name.empty()) {
        log_file_name_ = log_file_name;
    } else if (log_file_name_.empty()) {
        log_file_name_ = default_native_log_file_name();
    }
    config.file_path = logs_dir_ + "\\" + log_file_name_;
    config.level = level;
    config.max_files = 5;

    vr::configure_logging(config);

    spdlog::info(
        "[VideoRendererPlugin] Native logging reconfigured: level={}, file={}",
        level_str, config.file_path);

    result->Success(flutter::EncodableValue(nullptr));
}

void VideoRendererPlugin::CreateRenderer(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (renderer_) {
        result->Error("ALREADY_CREATED", "Renderer already exists");
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
    const auto& paths_list = std::get<flutter::EncodableList>(paths_it->second);

    int width = 1920;
    int height = 1080;
    auto w_it = args->find(flutter::EncodableValue("width"));
    auto h_it = args->find(flutter::EncodableValue("height"));
    if (w_it != args->end()) width = std::get<int>(w_it->second);
    if (h_it != args->end()) height = std::get<int>(h_it->second);

    // Create renderer in headless mode
    vr::RendererConfig config;
    config.headless = true;
    config.dxgi_adapter = dxgi_adapter_;
    config.width = width;
    config.height = height;
    config.use_hardware_decode = true;

    for (const auto& p : paths_list) {
        config.video_paths.push_back(std::get<std::string>(p));
    }

    renderer_ = std::make_unique<vr::Renderer>();
    g_global_renderer = renderer_.get();
    if (!renderer_->initialize(config)) {
        g_global_renderer = nullptr;
        renderer_.reset();
        result->Error("INIT_FAILED", "Failed to initialize renderer");
        return;
    }

    // Create GPU surface texture for Flutter using DXGI shared handle
    surface_descriptor_ = {};
    surface_descriptor_.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    surface_descriptor_.format = kFlutterDesktopPixelFormatBGRA8888;

    auto gpu_texture = std::make_unique<flutter::GpuSurfaceTexture>(
        kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
        [this](size_t width, size_t height) -> const FlutterDesktopGpuSurfaceDescriptor* {
            if (!renderer_) return nullptr;

            HANDLE handle = renderer_->shared_texture_handle();
            if (!handle) return nullptr;

            surface_descriptor_.handle = handle;
            surface_descriptor_.width = static_cast<size_t>(renderer_->texture_width());
            surface_descriptor_.height = static_cast<size_t>(renderer_->texture_height());
            surface_descriptor_.visible_width = surface_descriptor_.width;
            surface_descriptor_.visible_height = surface_descriptor_.height;
            surface_descriptor_.release_callback = nullptr;
            surface_descriptor_.release_context = nullptr;
            return &surface_descriptor_;
        });

    texture_variant_ = std::make_unique<flutter::TextureVariant>(std::move(*gpu_texture));
    texture_id_ = texture_registrar_->RegisterTexture(texture_variant_.get());

    if (texture_id_ < 0) {
        renderer_->shutdown();
        renderer_.reset();
        texture_variant_.reset();
        result->Error("TEXTURE_FAILED", "Failed to register texture");
        return;
    }

    // Set frame callback to notify Flutter of new frames
    renderer_->set_frame_callback([this]() {
        if (texture_id_ >= 0 && texture_registrar_) {
            texture_registrar_->MarkTextureFrameAvailable(texture_id_);
        }
    });

    spdlog::info("[VideoRendererPlugin] Created renderer, texture_id={}, tracks={}", texture_id_, renderer_->track_infos().size());

    // Build result map with textureId and track info
    flutter::EncodableMap result_map;
    result_map[flutter::EncodableValue("textureId")] = flutter::EncodableValue(texture_id_);

    flutter::EncodableList tracks_list;
    if (renderer_) {
        for (const auto& info : renderer_->track_infos()) {
            tracks_list.push_back(flutter::EncodableValue(make_track_map(info)));
        }
    }
    result_map[flutter::EncodableValue("tracks")] = flutter::EncodableValue(tracks_list);

    result->Success(flutter::EncodableValue(result_map));
}

void VideoRendererPlugin::DestroyRenderer(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (texture_id_ >= 0 && texture_registrar_) {
        texture_registrar_->UnregisterTexture(texture_id_);
        texture_id_ = -1;
    }
    texture_variant_.reset();

    if (renderer_) {
        g_global_renderer = nullptr;
        renderer_->shutdown();
        renderer_.reset();
    }

    spdlog::info("[VideoRendererPlugin] Destroyed renderer");
    result->Success(flutter::EncodableValue(nullptr));
}

void VideoRendererPlugin::AddTrack(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (!renderer_) {
        result->Error("NO_RENDERER", "Renderer not created");
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
    const auto& path = std::get<std::string>(it->second);

    int slot = renderer_->add_track(path);
    if (slot < 0) {
        result->Error("ADD_FAILED", "Failed to add track");
        return;
    }

    auto infos = renderer_->track_infos();
    const vr::TrackInfo* found = nullptr;
    for (const auto& ti : infos) {
        if (ti.slot == slot) { found = &ti; break; }
    }
    if (!found) {
        result->Error("ADD_FAILED", "Track not found after add");
        return;
    }

    spdlog::info("[VideoRendererPlugin] Added track: file_id={}, slot={}, path={}, tracks={}", found->file_id, slot, path, renderer_->track_infos().size());
    result->Success(flutter::EncodableValue(make_track_map(*found)));
}

void VideoRendererPlugin::RemoveTrack(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (!renderer_) {
        result->Error("NO_RENDERER", "Renderer not created");
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
    int file_id = std::get<int>(it->second);

    renderer_->remove_track(file_id);
    spdlog::info("[VideoRendererPlugin] Removed track: file_id={}", file_id);
    result->Success(flutter::EncodableValue(nullptr));
}

void VideoRendererPlugin::SetTrackOffset(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (!renderer_ || !arguments) {
        result->Error("INVALID", "Renderer not created or no arguments");
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
    if (it != args->end()) file_id = std::get<int>(it->second);
    it = args->find(flutter::EncodableValue("offsetUs"));
    if (it != args->end()) {
        if (std::holds_alternative<int>(it->second))
            offset_us = static_cast<int64_t>(std::get<int>(it->second));
        else if (std::holds_alternative<int64_t>(it->second))
            offset_us = std::get<int64_t>(it->second);
    }

    renderer_->set_track_offset(file_id, offset_us);
    spdlog::info("[VideoRendererPlugin] setTrackOffset: file_id={}, offset_us={}", file_id, offset_us);
    result->Success(flutter::EncodableValue(nullptr));
}

void VideoRendererPlugin::PickFiles(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    bool allow_multiple = true;

    if (arguments) {
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (args) {
            auto it = args->find(flutter::EncodableValue("allowMultiple"));
            if (it != args->end()) {
                allow_multiple = std::get<bool>(it->second);
            }
        }
    }

    // Flutter UI thread already has COM initialized — no CoInitializeEx needed.

    IFileOpenDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pfd));
    if (FAILED(hr)) {
        // Return empty list (not null) to avoid Dart type cast issues
        result->Success(flutter::EncodableValue(flutter::EncodableList()));
        return;
    }

    FILEOPENDIALOGOPTIONS options = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
    if (allow_multiple) options |= FOS_ALLOWMULTISELECT;
    pfd->SetOptions(options);

    // Video file filter
    COMDLG_FILTERSPEC filterSpec[] = {
        { L"Video Files", L"*.avi;*.flv;*.mkv;*.mov;*.mp4;*.mpeg;*.webm;*.wmv;*.ts;*.m2ts;*.vob;*.mpg;*.m4v;*.3gp" },
        { L"All Files", L"*.*" },
    };
    pfd->SetFileTypes(2, filterSpec);
    pfd->SetFileTypeIndex(1);

    HWND hwndOwner = GetActiveWindow();

    hr = pfd->Show(hwndOwner);

    flutter::EncodableList paths_list;

    if (SUCCEEDED(hr)) {
        IShellItemArray* items = nullptr;
        hr = pfd->GetResults(&items);
        if (SUCCEEDED(hr)) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    LPWSTR name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                        std::string path = Utf8FromUtf16(name);
                        if (!path.empty()) {
                            paths_list.push_back(flutter::EncodableValue(path));
                        }
                        CoTaskMemFree(name);
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }

    pfd->Release();

    // Always return a list (empty = cancelled, non-empty = selected files)
    result->Success(flutter::EncodableValue(paths_list));
}

void VideoRendererPlugin::CaptureViewport(
    const flutter::EncodableValue* arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

    if (!renderer_) {
        result->Error("NO_RENDERER", "Renderer not created");
        return;
    }

    std::string output_path;
    if (arguments) {
        const auto* args = std::get_if<flutter::EncodableMap>(arguments);
        if (args) {
            auto it = args->find(flutter::EncodableValue("outputPath"));
            if (it != args->end() && std::holds_alternative<std::string>(it->second)) {
                output_path = std::get<std::string>(it->second);
            }
        }
    }

    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!renderer_->capture_front_buffer(bgra, width, height)) {
        result->Error("CAPTURE_FAILED", "Failed to capture viewport");
        return;
    }

    const std::string hash = Fnv1a64Hex(bgra);
    const CaptureStats stats = ComputeCaptureStats(bgra);
    if (!output_path.empty() && !SaveBgraToPng(bgra, width, height, output_path)) {
        result->Error("CAPTURE_SAVE_FAILED", "Failed to save viewport PNG");
        return;
    }

    flutter::EncodableMap map;
    map[flutter::EncodableValue("hash")] = flutter::EncodableValue(hash);
    map[flutter::EncodableValue("width")] = flutter::EncodableValue(width);
    map[flutter::EncodableValue("height")] = flutter::EncodableValue(height);
    map[flutter::EncodableValue("avgLuma")] = flutter::EncodableValue(stats.avg_luma);
    map[flutter::EncodableValue("nonBlackRatio")] = flutter::EncodableValue(stats.non_black_ratio);
    if (!output_path.empty()) {
        map[flutter::EncodableValue("outputPath")] = flutter::EncodableValue(output_path);
    }
    result->Success(flutter::EncodableValue(map));
}
