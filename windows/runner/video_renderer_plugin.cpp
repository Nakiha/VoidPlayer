#include "video_renderer_plugin.h"

#include "analysis/analysis_ffi_abi.h"
#include "native_player_channel_names.h"
#include "common/logging.h"
#include "common/win_utf8.h"
#include "renderer/capture/bgra_capture_metrics.h"
#include "windows/presentation/windows_d3d11_target_ring.h"

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <wincodec.h>
#include <psapi.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

namespace {

using EncodableMap = flutter::EncodableMap;
using EncodableValue = flutter::EncodableValue;

constexpr UINT_PTR kPresentationPolicyRefreshTimer = 0x56504844;
constexpr UINT kPresentationPolicyRefreshDelayMs = 120;

const EncodableMap* AsMap(const EncodableValue* arguments) {
  return arguments ? std::get_if<EncodableMap>(arguments) : nullptr;
}

const EncodableValue* Find(const EncodableValue* arguments, const char* key) {
  const auto* map = AsMap(arguments);
  if (!map) {
    return nullptr;
  }
  const auto found = map->find(EncodableValue(key));
  return found == map->end() ? nullptr : &found->second;
}

int64_t ReadInt(const EncodableValue* arguments,
                const char* key,
                int64_t fallback = 0) {
  const auto* value = Find(arguments, key);
  if (!value) {
    return fallback;
  }
  if (const auto* result = std::get_if<int32_t>(value)) {
    return *result;
  }
  if (const auto* result = std::get_if<int64_t>(value)) {
    return *result;
  }
  if (const auto* result = std::get_if<double>(value)) {
    return static_cast<int64_t>(*result);
  }
  return fallback;
}

double ReadDouble(const EncodableValue* arguments,
                  const char* key,
                  double fallback = 0.0) {
  const auto* value = Find(arguments, key);
  if (!value) {
    return fallback;
  }
  if (const auto* result = std::get_if<double>(value)) {
    return *result;
  }
  if (const auto* result = std::get_if<int32_t>(value)) {
    return static_cast<double>(*result);
  }
  if (const auto* result = std::get_if<int64_t>(value)) {
    return static_cast<double>(*result);
  }
  return fallback;
}

bool ReadBool(const EncodableValue* arguments,
              const char* key,
              bool fallback = false) {
  const auto* value = Find(arguments, key);
  const auto* result = value ? std::get_if<bool>(value) : nullptr;
  return result ? *result : fallback;
}

std::string ReadString(const EncodableValue* arguments, const char* key) {
  const auto* value = Find(arguments, key);
  const auto* result = value ? std::get_if<std::string>(value) : nullptr;
  return result ? *result : std::string();
}

std::vector<std::string> ReadStringList(const EncodableValue* arguments,
                                        const char* key) {
  std::vector<std::string> result;
  const auto* value = Find(arguments, key);
  const auto* list = value ? std::get_if<flutter::EncodableList>(value) : nullptr;
  if (!list) {
    return result;
  }
  for (const auto& entry : *list) {
    if (const auto* path = std::get_if<std::string>(&entry)) {
      result.push_back(*path);
    }
  }
  return result;
}

int64_t SaturatingInt64(uint64_t value) {
  return static_cast<int64_t>(std::min<uint64_t>(
      value, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
}

struct ProcessMemorySnapshot {
  uint64_t rss_bytes = 0;
  uint64_t private_bytes = 0;
};

ProcessMemorySnapshot QueryProcessMemory() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters))) {
    return {};
  }
  return {static_cast<uint64_t>(counters.WorkingSetSize),
          static_cast<uint64_t>(counters.PrivateUsage)};
}

uint64_t QueryDedicatedGpuUsage(ID3D11Device* device) {
  if (!device) {
    return 0;
  }
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
      FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter.As(&adapter3)) ||
      FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                            &info))) {
    return 0;
  }
  return info.CurrentUsage;
}

EncodableMap TrackMap(const vr::TrackInfo& track) {
  return {
      {EncodableValue("fileId"), EncodableValue(track.file_id)},
      {EncodableValue("slot"), EncodableValue(track.slot)},
      {EncodableValue("path"), EncodableValue(track.file_path)},
      {EncodableValue("width"), EncodableValue(track.width)},
      {EncodableValue("height"), EncodableValue(track.height)},
      {EncodableValue("durationUs"), EncodableValue(track.duration_us)},
      {EncodableValue("startTimeUs"), EncodableValue(track.start_time_us)},
      {EncodableValue("bitRate"), EncodableValue(track.bit_rate)},
      {EncodableValue("formatName"), EncodableValue(track.format_name)},
      {EncodableValue("codecName"), EncodableValue(track.codec_name)},
      {EncodableValue("codecLongName"), EncodableValue(track.codec_long_name)},
      {EncodableValue("decoderName"), EncodableValue(track.decoder_name)},
      {EncodableValue("colorRange"), EncodableValue(track.color.range)},
      {EncodableValue("colorMatrix"), EncodableValue(track.color.matrix)},
      {EncodableValue("colorTransfer"), EncodableValue(track.color.transfer)},
      {EncodableValue("colorPrimaries"), EncodableValue(track.color.primaries)},
  };
}

flutter::EncodableList TrackList(const std::vector<vr::TrackInfo>& tracks) {
  flutter::EncodableList result;
  result.reserve(tracks.size());
  for (const auto& track : tracks) {
    result.emplace_back(TrackMap(track));
  }
  return result;
}

flutter::EncodableList TrackDiagnosticList(
    const vr::WindowsNativePlayer& player,
    const std::vector<vr::TrackInfo>& tracks,
    const std::vector<vr::TrackPerfStats>& perf_stats,
    const vr::RendererGpuMemoryStats& memory_stats) {
  flutter::EncodableList result;
  result.reserve(perf_stats.size());
  for (const auto& perf : perf_stats) {
    const auto track = std::find_if(tracks.begin(), tracks.end(),
                                    [&perf](const auto& candidate) {
                                      return candidate.file_id == perf.file_id;
                                    });
    const auto memory =
        std::find_if(memory_stats.tracks.begin(), memory_stats.tracks.end(),
                     [&perf](const auto& candidate) {
                       return candidate.file_id == perf.file_id;
                     });
    const uint64_t cpu_frame_bytes =
        memory == memory_stats.tracks.end() ? 0 : memory->total_cpu_frame_bytes;
    const uint64_t packet_queue_bytes =
        memory == memory_stats.tracks.end() ? 0 : memory->packet_queue_bytes;
    const bool hardware_decode =
        memory != memory_stats.tracks.end() && memory->hardware_enabled;
    const bool hardware_download =
        memory != memory_stats.tracks.end() && memory->hardware_download_to_cpu;
    EncodableMap item = {
        {EncodableValue("fileId"), EncodableValue(perf.file_id)},
        {EncodableValue("slot"), EncodableValue(perf.slot)},
        {EncodableValue("durationUs"),
         EncodableValue(track == tracks.end() ? int64_t{0}
                                              : track->duration_us)},
        {EncodableValue("offsetUs"),
         EncodableValue(player.track_offset_us(perf.file_id))},
        {EncodableValue("hardwareDecodeActive"),
         EncodableValue(hardware_decode)},
        {EncodableValue("hardwareDecodeDownloadsToCpu"),
         EncodableValue(hardware_download)},
        {EncodableValue("framesDecoded"),
         EncodableValue(SaturatingInt64(perf.frames_decoded))},
        {EncodableValue("fps"), EncodableValue(perf.fps)},
        {EncodableValue("decodeFps"), EncodableValue(perf.fps)},
        {EncodableValue("decodeAvgMs"), EncodableValue(perf.avg_decode_ms)},
        {EncodableValue("decodeMaxMs"), EncodableValue(perf.max_decode_ms)},
        {EncodableValue("decodeStagePacketSendCount"),
         EncodableValue(SaturatingInt64(perf.decode_stage_packet_send_count))},
        {EncodableValue("decodeStagePacketSendAvgMs"),
         EncodableValue(perf.decode_stage_packet_send_avg_ms)},
        {EncodableValue("decodeStagePacketSendMaxMs"),
         EncodableValue(perf.decode_stage_packet_send_max_ms)},
        {EncodableValue("decodeStageReceiveFrameCount"),
         EncodableValue(
             SaturatingInt64(perf.decode_stage_receive_frame_count))},
        {EncodableValue("decodeStageReceiveAvgMs"),
         EncodableValue(perf.decode_stage_receive_avg_ms)},
        {EncodableValue("decodeStageReceiveMaxMs"),
         EncodableValue(perf.decode_stage_receive_max_ms)},
        {EncodableValue("decodeStageConvertCount"),
         EncodableValue(SaturatingInt64(perf.decode_stage_convert_count))},
        {EncodableValue("decodeStageConvertAvgMs"),
         EncodableValue(perf.decode_stage_convert_avg_ms)},
        {EncodableValue("decodeStageConvertMaxMs"),
         EncodableValue(perf.decode_stage_convert_max_ms)},
        {EncodableValue("decodeStagePublishCount"),
         EncodableValue(SaturatingInt64(perf.decode_stage_publish_count))},
        {EncodableValue("decodeStagePublishAvgMs"),
         EncodableValue(perf.decode_stage_publish_avg_ms)},
        {EncodableValue("decodeStagePublishMaxMs"),
         EncodableValue(perf.decode_stage_publish_max_ms)},
        {EncodableValue("decodeStagePublishWaitAvgMs"),
         EncodableValue(perf.decode_stage_publish_wait_avg_ms)},
        {EncodableValue("decodeStagePublishWaitMaxMs"),
         EncodableValue(perf.decode_stage_publish_wait_max_ms)},
        {EncodableValue("bufferState"),
         EncodableValue(static_cast<int32_t>(perf.buffer_state))},
        {EncodableValue("bufferCount"),
         EncodableValue(static_cast<int64_t>(perf.buffer_count))},
        {EncodableValue("bufferCapacity"),
         EncodableValue(static_cast<int64_t>(perf.buffer_capacity))},
        {EncodableValue("bufferPrerollTarget"),
         EncodableValue(static_cast<int64_t>(perf.buffer_preroll_target))},
        {EncodableValue("cpuFrameMemoryBytes"),
         EncodableValue(SaturatingInt64(cpu_frame_bytes))},
        {EncodableValue("packetQueueMemoryBytes"),
         EncodableValue(SaturatingInt64(packet_queue_bytes))},
        {EncodableValue("currentPtsUs"), EncodableValue(perf.current_pts_us)},
        {EncodableValue("currentDtsUs"), EncodableValue(perf.current_dts_us)},
    };
    result.emplace_back(std::move(item));
  }
  return result;
}

vr::LayoutState ReadLayout(const EncodableValue* arguments) {
  vr::LayoutState layout;
  layout.mode = static_cast<int>(ReadInt(arguments, "mode", layout.mode));
  layout.split_pos = ReadDouble(arguments, "splitPos", layout.split_pos);
  layout.zoom_ratio = ReadDouble(arguments, "zoomRatio", layout.zoom_ratio);
  layout.view_offset[0] =
      ReadDouble(arguments, "viewOffsetX", layout.view_offset[0]);
  layout.view_offset[1] =
      ReadDouble(arguments, "viewOffsetY", layout.view_offset[1]);
  layout.pixel_size_mode = static_cast<int>(
      ReadInt(arguments, "pixelSizeMode", layout.pixel_size_mode));
  const auto* order_value = Find(arguments, "order");
  const auto* order = order_value
      ? std::get_if<flutter::EncodableList>(order_value)
      : nullptr;
  if (order) {
    for (size_t index = 0; index < std::min<size_t>(4, order->size()); ++index) {
      const auto& entry = (*order)[index];
      if (const auto* value = std::get_if<int32_t>(&entry)) {
        layout.order[index] = *value;
      } else if (const auto* value = std::get_if<int64_t>(&entry)) {
        layout.order[index] = static_cast<int>(*value);
      }
    }
  }
  return layout;
}

EncodableMap LayoutMap(const vr::LayoutState& layout) {
  flutter::EncodableList order;
  for (int entry : layout.order) {
    order.emplace_back(entry);
  }
  return {
      {EncodableValue("mode"), EncodableValue(layout.mode)},
      {EncodableValue("splitPos"), EncodableValue(layout.split_pos)},
      {EncodableValue("zoomRatio"), EncodableValue(layout.zoom_ratio)},
      {EncodableValue("viewOffsetX"), EncodableValue(layout.view_offset[0])},
      {EncodableValue("viewOffsetY"), EncodableValue(layout.view_offset[1])},
      {EncodableValue("pixelSizeMode"),
       EncodableValue(layout.pixel_size_mode)},
      {EncodableValue("order"), EncodableValue(std::move(order))},
  };
}

EncodableMap FrameMap(int file_id,
                      const vr::PresentationBackendFrameInfo& frame) {
  return {
      {EncodableValue("fileId"), EncodableValue(file_id)},
      {EncodableValue("ptsUs"), EncodableValue(frame.pts_us)},
      {EncodableValue("dtsUs"), EncodableValue(frame.dts_us)},
      {EncodableValue("durationUs"), EncodableValue(frame.duration_us)},
      {EncodableValue("analysisFrameIndex"),
       EncodableValue(frame.analysis_frame_index)},
      {EncodableValue("frameIdentityMode"),
       EncodableValue(frame.frame_identity_mode)},
      {EncodableValue("sourcePacketIndex"),
       EncodableValue(frame.source_packet_index)},
      {EncodableValue("sourcePacketSize"),
       EncodableValue(frame.source_packet_size)},
      {EncodableValue("sourcePacketPos"),
       EncodableValue(frame.source_packet_pos)},
      {EncodableValue("sourcePacketPtsUs"),
       EncodableValue(frame.source_packet_pts)},
      {EncodableValue("sourcePacketDtsUs"),
       EncodableValue(frame.source_packet_dts)},
  };
}

std::string Fnv1a64(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << hash;
  return output.str();
}

bool SaveBgraPng(const std::vector<uint8_t>& bgra,
                 int width,
                 int height,
                 const std::string& path) {
  if (bgra.empty() || width <= 0 || height <= 0 || path.empty()) {
    return path.empty();
  }
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))) ||
      !factory) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(&stream)) || !stream) {
    return false;
  }
  const auto wide_path = vr::win_utf8::utf16_from_utf8(path);
  if (FAILED(stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  Microsoft::WRL::ComPtr<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(&frame, &properties)) || !frame ||
      FAILED(frame->Initialize(properties.Get())) ||
      FAILED(frame->SetSize(static_cast<UINT>(width),
                            static_cast<UINT>(height)))) {
    return false;
  }
  WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&pixel_format))) {
    return false;
  }
  const UINT stride = static_cast<UINT>(width * 4);
  const UINT image_size = stride * static_cast<UINT>(height);
  return SUCCEEDED(frame->WritePixels(
             static_cast<UINT>(height), stride, image_size,
             const_cast<BYTE*>(bgra.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

EncodableMap CaptureMap(const std::vector<uint8_t>& bgra,
                        int width,
                        int height,
                        const std::string& output_path) {
  uint64_t luma_sum = 0;
  size_t non_black = 0;
  const size_t pixels = bgra.size() / 4;
  for (size_t index = 0; index < pixels; ++index) {
    const uint8_t blue = bgra[index * 4];
    const uint8_t green = bgra[index * 4 + 1];
    const uint8_t red = bgra[index * 4 + 2];
    luma_sum += static_cast<uint64_t>(
        (77 * red + 150 * green + 29 * blue) >> 8);
    if (red > 8 || green > 8 || blue > 8) {
      ++non_black;
    }
  }
  const double average =
      pixels == 0 ? 0.0
                  : static_cast<double>(luma_sum) / static_cast<double>(pixels);
  const double ratio = pixels == 0
      ? 0.0
      : static_cast<double>(non_black) / static_cast<double>(pixels);
  const vr::BgraOverlayLineStyleMetrics overlay_line_style =
      vr::measure_bgra_overlay_line_style(
          bgra.data(), width, height, width * 4);
  const bool saved = SaveBgraPng(bgra, width, height, output_path);
  return {
      {EncodableValue("hash"), EncodableValue(Fnv1a64(bgra))},
      {EncodableValue("width"), EncodableValue(width)},
      {EncodableValue("height"), EncodableValue(height)},
      {EncodableValue("avgLuma"), EncodableValue(average)},
      {EncodableValue("nonBlackRatio"), EncodableValue(ratio)},
      {EncodableValue("overlayLinePairedCenters"),
       EncodableValue(static_cast<int64_t>(overlay_line_style.paired_centers))},
      {EncodableValue("overlayLineWeakWhiteCenters"),
       EncodableValue(
           static_cast<int64_t>(overlay_line_style.weak_white_centers))},
      {EncodableValue("overlayLineBlackOnlyCenters"),
       EncodableValue(
           static_cast<int64_t>(overlay_line_style.black_only_centers))},
      {EncodableValue("outputPath"), EncodableValue(output_path)},
      {EncodableValue("saved"), EncodableValue(saved)},
  };
}

void ScaleBgraToMaxSize(std::vector<uint8_t>& bgra,
                        int& width,
                        int& height,
                        int max_size) {
  if (max_size <= 0 || width <= 0 || height <= 0 ||
      std::max(width, height) <= max_size) {
    return;
  }
  const double scale = static_cast<double>(max_size) /
                       static_cast<double>(std::max(width, height));
  const int output_width =
      std::max(1, static_cast<int>(std::lround(width * scale)));
  const int output_height =
      std::max(1, static_cast<int>(std::lround(height * scale)));
  std::vector<uint8_t> scaled(
      static_cast<size_t>(output_width) * output_height * 4);
  for (int output_y = 0; output_y < output_height; ++output_y) {
    const int source_y = std::min(
        height - 1,
        static_cast<int>((static_cast<int64_t>(output_y) * height) /
                         output_height));
    for (int output_x = 0; output_x < output_width; ++output_x) {
      const int source_x = std::min(
          width - 1,
          static_cast<int>((static_cast<int64_t>(output_x) * width) /
                           output_width));
      const size_t source_offset =
          (static_cast<size_t>(source_y) * width + source_x) * 4;
      const size_t output_offset =
          (static_cast<size_t>(output_y) * output_width + output_x) * 4;
      std::copy_n(bgra.data() + source_offset, 4,
                  scaled.data() + output_offset);
    }
  }
  bgra = std::move(scaled);
  width = output_width;
  height = output_height;
}

void AddCompositorDiagnostics(EncodableMap& diagnostics,
                              const WindowsNativeCompositor* compositor,
                              bool started,
                              const WindowsViewportPresentationController*
                                  viewport_controller) {
  diagnostics[EncodableValue("nativeCompositorEnabled")] =
      EncodableValue(started);
  if (!compositor) {
    return;
  }
  const auto state = compositor->diagnostics();
  diagnostics[EncodableValue("windowsCompositorInitialized")] =
      EncodableValue(state.initialized);
  diagnostics[EncodableValue("windowsFlutterExportEnabled")] =
      EncodableValue(state.flutter_export_enabled);
  diagnostics[EncodableValue("windowsCompositeCount")] =
      EncodableValue(static_cast<int64_t>(state.composite_count));
  diagnostics[EncodableValue("windowsFlutterPublishCount")] =
      EncodableValue(static_cast<int64_t>(state.flutter_publish_count));
  diagnostics[EncodableValue("windowsFlutterPublishSampleCount")] =
      EncodableValue(
          static_cast<int64_t>(state.flutter_publish_sample_count));
  diagnostics[EncodableValue("windowsFlutterExportRequestCount")] =
      EncodableValue(
          static_cast<int64_t>(state.flutter_export_request_count));
  diagnostics[EncodableValue("windowsFlutterExportRequestDispatchCount")] =
      EncodableValue(static_cast<int64_t>(
          state.flutter_export_request_dispatch_count));
  diagnostics[EncodableValue("windowsFlutterExportScheduleFrameCount")] =
      EncodableValue(static_cast<int64_t>(
          state.flutter_export_schedule_frame_count));
  diagnostics[EncodableValue("windowsFlutterExportVsyncCount")] =
      EncodableValue(static_cast<int64_t>(state.flutter_export_vsync_count));
  diagnostics[EncodableValue("windowsFlutterExportPresentCount")] =
      EncodableValue(static_cast<int64_t>(state.flutter_export_present_count));
  diagnostics[EncodableValue("windowsFlutterExportBeginCount")] =
      EncodableValue(static_cast<int64_t>(state.flutter_export_begin_count));
  diagnostics[EncodableValue("windowsFlutterExportFlushCount")] =
      EncodableValue(static_cast<int64_t>(state.flutter_export_flush_count));
  diagnostics[EncodableValue("windowsFlutterExportFinishCount")] =
      EncodableValue(static_cast<int64_t>(state.flutter_export_finish_count));
  diagnostics[EncodableValue("windowsFlutterExportPendingPumpFrames")] =
      EncodableValue(static_cast<int64_t>(
          state.flutter_export_pending_pump_frames));
  diagnostics[EncodableValue("windowsVideoPublishCount")] =
      EncodableValue(static_cast<int64_t>(state.video_publish_count));
  diagnostics[EncodableValue("windowsVideoPresentCount")] =
      EncodableValue(static_cast<int64_t>(state.video_present_count));
  diagnostics[EncodableValue("windowsVideoTargetRetainedReconfigureCount")] =
      EncodableValue(
          static_cast<int64_t>(state.video_target_retained_reconfigure_count));
  diagnostics[EncodableValue("windowsVideoTargetRetainedHandoffCount")] =
      EncodableValue(
          static_cast<int64_t>(state.video_target_retained_handoff_count));
  diagnostics[EncodableValue("windowsVideoTargetRetainedGeometrySyncCount")] =
      EncodableValue(static_cast<int64_t>(
          state.video_target_retained_geometry_sync_count));
  diagnostics[EncodableValue("windowsVideoPresentRetryCount")] =
      EncodableValue(static_cast<int64_t>(state.video_present_retry_count));
  diagnostics[EncodableValue("windowsFlutterAcquireFailureCount")] =
      EncodableValue(static_cast<int64_t>(state.acquire_failure_count));
  diagnostics[EncodableValue("windowsFlutterKeyedMutexFailureCount")] =
      EncodableValue(static_cast<int64_t>(state.keyed_mutex_failure_count));
  diagnostics[EncodableValue("windowsPresentFailureCount")] =
      EncodableValue(static_cast<int64_t>(state.present_failure_count));
  diagnostics[EncodableValue("windowsLastFlutterFrameGeneration")] =
      EncodableValue(
          static_cast<int64_t>(state.last_flutter_frame_generation));
  diagnostics[EncodableValue("windowsCompositorLastError")] =
      EncodableValue(state.last_error);
  diagnostics[EncodableValue("windowsCompositorOutputMode")] =
      EncodableValue(state.output_mode);
  diagnostics[EncodableValue("windowsCompositorScRGBEnabled")] =
      EncodableValue(state.scrgb_output_enabled);
  diagnostics[EncodableValue("windowsCompositorSwapChainFormat")] =
      EncodableValue(state.swap_chain_format);
  diagnostics[EncodableValue("windowsCompositorColorSpace")] =
      EncodableValue(state.swap_chain_color_space);
  diagnostics[EncodableValue("windowsFlutterSourceFormat")] =
      EncodableValue(state.flutter_source_format);
  diagnostics[EncodableValue("windowsVideoTargetFormat")] =
      EncodableValue(state.video_target_format);
  diagnostics[EncodableValue("windowsSDRWhiteLevelMilliNits")] =
      EncodableValue(state.sdr_white_level_milli_nits);
  diagnostics[EncodableValue("windowsCompositorBackgroundColorArgb")] =
      EncodableValue(static_cast<int64_t>(state.background_color_argb));
  diagnostics[EncodableValue("windowsCompositorOutputFallbackReason")] =
      EncodableValue(state.output_fallback_reason);
  if (!viewport_controller) {
    return;
  }
  const auto viewport = viewport_controller->diagnostics();
  diagnostics[EncodableValue("viewportClockSource")] =
      EncodableValue("dxgi-present-vsync");
  diagnostics[EncodableValue("displayRefreshHzEstimateX1000")] = EncodableValue(
      static_cast<int64_t>(std::llround(viewport.nominal_refresh_hz * 1000.0)));
  diagnostics[EncodableValue("displayRefreshHzEstimate")] =
      EncodableValue(viewport.nominal_refresh_hz);
  diagnostics[EncodableValue("displayTickHz")] =
      EncodableValue(viewport.nominal_refresh_hz);
  diagnostics[EncodableValue("layoutIntentCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_intent_count));
  diagnostics[EncodableValue("layoutSubmitCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_submit_count));
  diagnostics[EncodableValue("layoutDrawCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_presented_count));
  diagnostics[EncodableValue("layoutPublishedCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_presented_count));
  diagnostics[EncodableValue("interactionLayoutSubmitCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_submit_count));
  diagnostics[EncodableValue("interactionLayoutSubmitHzX1000")] =
      EncodableValue(static_cast<int64_t>(
          std::llround(viewport.measured_submit_hz * 1000.0)));
  diagnostics[EncodableValue("interactionLayoutSubmitHz")] =
      EncodableValue(viewport.measured_submit_hz);
  diagnostics[EncodableValue("layoutDrawHz")] =
      EncodableValue(viewport.measured_submit_hz);
  diagnostics[EncodableValue("layoutRefreshSupersededCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_superseded_count));
  diagnostics[EncodableValue("layoutRefreshFailureCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_failed_count));
  diagnostics[EncodableValue("layoutRefreshBackpressureCount")] =
      EncodableValue(static_cast<int64_t>(viewport.layout_backpressure_count));
  diagnostics[EncodableValue("interactionFramesInFlight")] = EncodableValue(
      static_cast<int64_t>(viewport.interaction_frames_in_flight));
  diagnostics[EncodableValue("maxInteractionFramesInFlight")] =
      EncodableValue(int64_t{2});
}

void AddPresentationPolicyDiagnostics(
    EncodableMap& diagnostics, const vr::WindowsPresentationPolicy& policy,
    const vr::WindowsDisplayProbeResult& display, double sdr_white_level_nits) {
  diagnostics[EncodableValue("windowsPresentationRequest")] =
      EncodableValue(policy.request);
  diagnostics[EncodableValue("windowsPresentationMode")] =
      EncodableValue(policy.mode);
  diagnostics[EncodableValue("windowsPresentationReason")] =
      EncodableValue(policy.reason);
  diagnostics[EncodableValue("windowsPresentationFallbackReason")] =
      EncodableValue(policy.fallback_reason);
  diagnostics[EncodableValue("windowsMediaHasHDRTrack")] =
      EncodableValue(policy.has_hdr_track);
  diagnostics[EncodableValue("windowsDisplayProbeStatus")] =
      EncodableValue(display.status);
  diagnostics[EncodableValue("windowsDisplayDeviceName")] =
      EncodableValue(display.device_name);
  diagnostics[EncodableValue("windowsDisplayAdapterDescription")] =
      EncodableValue(display.adapter_description);
  diagnostics[EncodableValue("windowsDisplayColorSpace")] =
      EncodableValue(display.color_space);
  diagnostics[EncodableValue("windowsDisplayHDRActive")] =
      EncodableValue(display.hdr_active);
  diagnostics[EncodableValue("windowsDisplayAdvancedColorActive")] =
      EncodableValue(display.advanced_color_active);
  diagnostics[EncodableValue("windowsDisplayMatchesPresentationAdapter")] =
      EncodableValue(display.matches_presentation_adapter);
  diagnostics[EncodableValue("windowsDisplayBitsPerColor")] =
      EncodableValue(display.bits_per_color);
  diagnostics[EncodableValue("windowsDisplaySDRWhiteLevelStatus")] =
      EncodableValue(display.sdr_white_level_status);
  diagnostics[EncodableValue("windowsPresentationSDRWhiteLevelMilliNits")] =
      EncodableValue(
          static_cast<int64_t>(std::llround(sdr_white_level_nits * 1000.0)));
}

}  // namespace

void VideoRendererPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar,
    FlutterDesktopPluginRegistrarRef core_registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), native_player_channels::kMethodChannel,
          &flutter::StandardMethodCodec::GetInstance());
  auto events =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), native_player_channels::kEventChannel,
          &flutter::StandardMethodCodec::GetInstance());

  FlutterDesktopViewRef flutter_view =
      FlutterDesktopPluginRegistrarGetView(core_registrar);
  HWND flutter_window = flutter_view ? FlutterDesktopViewGetHWND(flutter_view)
                                     : nullptr;
  HWND window_handle = flutter_window ? GetAncestor(flutter_window, GA_ROOT)
                                      : nullptr;
  auto plugin = std::make_unique<VideoRendererPlugin>(
      registrar, window_handle, flutter_view);
  channel->SetMethodCallHandler(
      [plugin_ptr = plugin.get()](const auto& call, auto result) {
        plugin_ptr->HandleMethodCall(call, std::move(result));
      });
  events->SetStreamHandler(std::make_unique<
      flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [plugin_ptr = plugin.get()](
          const flutter::EncodableValue*,
          std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& sink)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        plugin_ptr->SetEventSink(std::move(sink));
        return nullptr;
      },
      [plugin_ptr = plugin.get()](const flutter::EncodableValue*)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        plugin_ptr->ClearEventSink();
        return nullptr;
      }));
  plugin->event_bridge_.RegisterDrainWindow();
  registrar->AddPlugin(std::move(plugin));
}

VideoRendererPlugin::VideoRendererPlugin(
    flutter::PluginRegistrarWindows* registrar,
    HWND window_handle,
    FlutterDesktopViewRef flutter_view)
    : window_handle_(window_handle), registrar_(registrar) {
  if (registrar_) {
    window_proc_delegate_id_ = registrar_->RegisterTopLevelWindowProcDelegate(
        [this](HWND hwnd, UINT message, WPARAM wparam,
               LPARAM lparam) -> std::optional<LRESULT> {
          return HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
        });
  }
  if (window_handle_) {
    viewport_presentation_controller_ =
        std::make_unique<WindowsViewportPresentationController>(window_handle_);
  }
  if (window_handle_ && flutter_view) {
    compositor_ = std::make_unique<WindowsNativeCompositor>(
        window_handle_, flutter_view);
    compositor_started_ = compositor_->Start();
  }
}

VideoRendererPlugin::~VideoRendererPlugin() {
  if (window_handle_) {
    KillTimer(window_handle_, kPresentationPolicyRefreshTimer);
  }
  if (registrar_ && window_proc_delegate_id_ >= 0) {
    registrar_->UnregisterTopLevelWindowProcDelegate(window_proc_delegate_id_);
    window_proc_delegate_id_ = -1;
  }
  DestroyPlayer();
  event_bridge_.Shutdown();
  if (compositor_) {
    compositor_->Stop();
  }
}

std::optional<LRESULT> VideoRendererPlugin::HandleTopLevelWindowProc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;
  switch (message) {
  case WM_ERASEBKGND: {
    RECT client = {};
    GetClientRect(hwnd, &client);
    HBRUSH brush = CreateSolidBrush(viewport_background_color_);
    if (brush) {
      FillRect(reinterpret_cast<HDC>(wparam), &client, brush);
      DeleteObject(brush);
      return 1;
    }
    break;
  }
  case WM_DISPLAYCHANGE:
  case WM_SETTINGCHANGE:
  case WM_MOVE:
  case WM_EXITSIZEMOVE:
  case WM_DPICHANGED:
    SchedulePresentationPolicyRefresh();
    break;
  case WM_TIMER:
    if (wparam == kPresentationPolicyRefreshTimer) {
      KillTimer(window_handle_, kPresentationPolicyRefreshTimer);
      std::string error;
      if (!RefreshPresentationPolicy("window-display-change", error)) {
        spdlog::warn("[WindowsPresentation] display refresh failed: {}", error);
        FailClosedPresentation(std::move(error));
      }
    }
    break;
  default:
    break;
  }
  return std::nullopt;
}

void VideoRendererPlugin::ApplyViewportBackgroundColor(uint32_t color) {
  const uint8_t red = static_cast<uint8_t>((color >> 16u) & 0xFFu);
  const uint8_t green = static_cast<uint8_t>((color >> 8u) & 0xFFu);
  const uint8_t blue = static_cast<uint8_t>(color & 0xFFu);
  const uint8_t alpha = static_cast<uint8_t>((color >> 24u) & 0xFFu);
  viewport_background_color_ = RGB(red, green, blue);
  const float scale = 1.0f / 255.0f;
  if (player_) {
    player_->set_background_color(red * scale, green * scale, blue * scale,
                                  alpha * scale);
  }
  if (compositor_) {
    compositor_->SetBackgroundColor(red * scale, green * scale, blue * scale,
                                    alpha * scale);
  }
}

void VideoRendererPlugin::SchedulePresentationPolicyRefresh() {
  if (window_handle_) {
    SetTimer(window_handle_, kPresentationPolicyRefreshTimer,
             kPresentationPolicyRefreshDelayMs, nullptr);
  }
}

void VideoRendererPlugin::FailClosedPresentation(std::string failure) {
  {
    std::lock_guard<std::mutex> lock(presentation_state_mutex_);
    first_frame_activation_gate_.cancel_session();
    presentation_session_ = 0;
  }
  if (player_) {
    player_->pause();
  }
  if (compositor_) {
    compositor_->ClearVideoTargetRing();
  }
  presentation_policy_.mode = "unavailable";
  presentation_policy_.reason = "presentation-fail-closed";
  presentation_policy_.fallback_reason = std::move(failure);
  spdlog::error("[WindowsPresentation] fail closed: {}",
                presentation_policy_.fallback_reason);
  event_bridge_.QueueNativeCompositorState(
      false, true, presentation_policy_.mode, presentation_policy_.reason,
      "windows-native-d3d11", presentation_policy_.fallback_reason);
}

void VideoRendererPlugin::SetEventSink(
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink) {
  event_bridge_.SetSink(std::move(sink));
  if (player_) {
    QueueCurrentNativeCompositorState();
  }
}

void VideoRendererPlugin::ClearEventSink() {
  event_bridge_.ClearSink();
}

void VideoRendererPlugin::DestroyPlayer() {
  {
    std::lock_guard<std::mutex> lock(presentation_state_mutex_);
    first_frame_activation_gate_.cancel_session();
    presentation_session_ = 0;
  }
  if (viewport_presentation_controller_) {
    viewport_presentation_controller_->DetachPlayerAndWait();
  }
  auto player = std::move(player_);
  if (player) {
    player->set_frame_callback({});
    player->set_frame_failure_callback({});
    player->set_event_callback({});
    player->shutdown();
    target_release_queue_.Drain();
  }
  if (compositor_) {
    compositor_->ClearVideoTargetRing();
  }
  video_target_width_ = 0;
  video_target_height_ = 0;
  player_id_ = 0;
}

bool VideoRendererPlugin::RefreshPresentationPolicy(const char* reason,
                                                    std::string& error) {
  error.clear();
  if (!compositor_ || !compositor_started_) {
    error = "Windows native compositor is unavailable";
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (compositor_->device() &&
      SUCCEEDED(
          compositor_->device()->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
      dxgi_device) {
    (void)dxgi_device->GetAdapter(&adapter);
  }
  display_probe_ = display_resolver_.Probe(window_handle_, adapter.Get());
  const std::string requested_mode =
      vr::win_utf8::get_env_utf8(L"VOIDPLAYER_WINDOWS_PRESENTATION_MODE");
  const bool has_hdr_track =
      player_ && vr::windows_tracks_have_hdr_transfer(player_->tracks());
  auto policy = vr::resolve_windows_presentation_policy(
      requested_mode.empty() ? "auto" : requested_mode, has_hdr_track,
      display_probe_);
  if (!policy.supported) {
    error = policy.reason;
    return false;
  }
  return ApplyPresentationPolicy(std::move(policy), display_probe_, reason,
                                 error);
}

bool VideoRendererPlugin::ApplyPresentationPolicy(
    vr::WindowsPresentationPolicy policy,
    const vr::WindowsDisplayProbeResult& display,
    const char* reason,
    std::string& error) {
  error.clear();
  const double white_nits =
      display.sdr_white_level_milli_nits > 0
          ? static_cast<double>(display.sdr_white_level_milli_nits) / 1000.0
          : 80.0;
  WindowsNativeCompositorOutputConfig compositor_config;
  compositor_config.linear_scrgb =
      policy.output_target == vr::ColorOutputTarget::kWindowsLinearScRGB;
  compositor_config.sdr_white_level_nits = white_nits;
  std::string compositor_error;
  if (!compositor_->ConfigureOutput(compositor_config, compositor_error)) {
    if (!compositor_config.linear_scrgb) {
      error = compositor_error;
      return false;
    }
    policy.mode = "native-compositor-sdr";
    policy.reason = "scrgb-target-failed-native-sdr";
    policy.fallback_reason = compositor_error.empty()
                                 ? "scrgb-target-configuration-failed"
                                 : compositor_error;
    policy.output_target = vr::ColorOutputTarget::kSDRToneMappedBT709;
    policy.hdr_output_requested = false;
    compositor_config.linear_scrgb = false;
    if (!compositor_->ConfigureOutput(compositor_config, compositor_error)) {
      error = "scRGB and native SDR compositor configuration failed: " +
              compositor_error;
      return false;
    }
  }

  const DXGI_FORMAT target_format = compositor_config.linear_scrgb
                                        ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                        : DXGI_FORMAT_B8G8R8A8_UNORM;
  const std::string desired_target_format =
      compositor_config.linear_scrgb ? "rgba16f" : "bgra8";
  const auto compositor_diagnostics = compositor_->diagnostics();
  const bool target_format_changed =
      compositor_diagnostics.video_target_format != desired_target_format;
  if (player_ && player_->initialized() && target_format_changed) {
    std::vector<void*> textures;
    if (!compositor_->CreateVideoTargetRing(
            static_cast<uint32_t>(video_target_width_),
            static_cast<uint32_t>(video_target_height_), target_format, 6,
            textures) ||
        !player_->install_target_ring(
            reinterpret_cast<const void* const*>(textures.data()),
            textures.size(), nullptr, nullptr, video_target_width_,
            video_target_height_, 4)) {
      if (compositor_config.linear_scrgb) {
        policy.mode = "native-compositor-sdr";
        policy.reason = "scrgb-ring-failed-native-sdr";
        policy.fallback_reason = player_->presentation_error();
        policy.output_target = vr::ColorOutputTarget::kSDRToneMappedBT709;
        policy.hdr_output_requested = false;
        compositor_config.linear_scrgb = false;
        std::string fallback_error;
        std::vector<void*> fallback_textures;
        if (!compositor_->ConfigureOutput(compositor_config, fallback_error) ||
            !compositor_->CreateVideoTargetRing(
                static_cast<uint32_t>(video_target_width_),
                static_cast<uint32_t>(video_target_height_),
                DXGI_FORMAT_B8G8R8A8_UNORM, 6, fallback_textures) ||
            !player_->install_target_ring(
                reinterpret_cast<const void* const*>(fallback_textures.data()),
                fallback_textures.size(), nullptr, nullptr, video_target_width_,
                video_target_height_, 4)) {
          error = "scRGB target ring failed and native SDR fallback failed";
          return false;
        }
      } else {
        error = player_->presentation_error();
        return false;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(presentation_state_mutex_);
    presentation_policy_ = std::move(policy);
    presentation_sdr_white_level_nits_ = white_nits;
    if (presentation_session_ != 0) {
      (void)first_frame_activation_gate_.mark_policy_ready(
          presentation_session_);
    }
  }
  if (player_ && player_->initialized()) {
    (void)player_->update_presentation_sdr_white_level(white_nits);
    player_->request_frame_refresh(reason ? reason : "windows-output-policy");
  }
  spdlog::info(
      "[WindowsPresentation] request={} mode={} reason={} hdr_track={} "
      "display_status={} display_hdr={} adapter_match={} target={} "
      "sdr_white_nits={:.3f} white_status={} fallback={}",
      presentation_policy_.request, presentation_policy_.mode,
      presentation_policy_.reason, presentation_policy_.has_hdr_track,
      display.status, display.hdr_active, display.matches_presentation_adapter,
      compositor_config.linear_scrgb ? "rgba16f-linear-scrgb" : "bgra8-sdr",
      white_nits, display.sdr_white_level_status,
      presentation_policy_.fallback_reason);
  QueueCurrentNativeCompositorState();
  return true;
}

bool VideoRendererPlugin::ResizeVideoTargets(int width,
                                             int height,
                                             std::string& error) {
  error.clear();
  if (!compositor_ || !compositor_started_ || width <= 0 || height <= 0) {
    error = "Windows native compositor is unavailable";
    return false;
  }
  if (width == video_target_width_ && height == video_target_height_) {
    return true;
  }
  std::vector<void*> textures;
  const DXGI_FORMAT target_format =
      presentation_policy_.output_target ==
              vr::ColorOutputTarget::kWindowsLinearScRGB
          ? DXGI_FORMAT_R16G16B16A16_FLOAT
          : DXGI_FORMAT_B8G8R8A8_UNORM;
  if (!compositor_->CreateVideoTargetRing(
          static_cast<uint32_t>(width), static_cast<uint32_t>(height),
          target_format, 6, textures)) {
    error = compositor_->diagnostics().last_error;
    return false;
  }
  if (player_ && player_->initialized() &&
      !player_->install_target_ring(
          reinterpret_cast<const void* const*>(textures.data()),
          textures.size(), nullptr, nullptr, width, height, 4)) {
    error = player_->presentation_error();
    return false;
  }
  video_target_width_ = width;
  video_target_height_ = height;
  return true;
}

void VideoRendererPlugin::QueueCurrentNativeCompositorState(
    vr::WindowsFirstFrameActivationGate::Session expected_session) {
  std::lock_guard<std::mutex> lock(presentation_state_mutex_);
  if (expected_session != 0 && expected_session != presentation_session_) {
    return;
  }
  const bool active =
      first_frame_activation_gate_.active(presentation_session_);
  event_bridge_.QueueNativeCompositorState(
      active, true, presentation_policy_.mode, presentation_policy_.reason,
      "windows-native-d3d11",
      active && presentation_policy_.fallback_reason != "none"
          ? presentation_policy_.fallback_reason
          : "");
}

void VideoRendererPlugin::OnFrameAvailable(
    const std::weak_ptr<vr::WindowsNativePlayer>& weak_player,
    vr::WindowsFirstFrameActivationGate::Session presentation_session,
    const vr::PresentationBackendFrameInfo* frame_info) {
  auto player = weak_player.lock();
  if (!player || !compositor_) {
    return;
  }
  vr::PresentationBackendFrameInfo copied = {};
  if (!frame_info) {
    if (!player->copy_last_frame_info(&copied)) {
      return;
    }
    frame_info = &copied;
  }
  auto* target = reinterpret_cast<ID3D11Texture2D*>(
      frame_info->target_pixel_buffer_address);
  if (!target) {
    return;
  }
  // The shared renderer invokes this callback synchronously from interaction,
  // preview, playback and step paths, some of which still own facade/lifecycle
  // locks. The compositor has waited for its GPU copy; return every completed
  // target through one native queue so no callback path can silently skip the
  // same release protocol or depend on the Win32 UI message pump.
  const bool presented = compositor_->PresentVideoTarget(target);
  const bool current_target = compositor_->IsCurrentVideoTarget(target);
  if (first_frame_activation_gate_.accept_present(
          presentation_session, presented, current_target)) {
    spdlog::info(
        "[WindowsPresentation] first-frame activation session={} "
        "current_ring=true",
        presentation_session);
    QueueCurrentNativeCompositorState(presentation_session);
  }
  target_release_queue_.Enqueue(player, target);
}

void VideoRendererPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<MethodResult> result) {
  const std::string& method = call.method_name();
  const auto* arguments = call.arguments();

  if (method == "initLogging") {
    const std::string logs_dir = ReadString(arguments, "logsDir");
    const std::string file_name = ReadString(arguments, "logFileName");
    vr::LogConfig config;
    if (!logs_dir.empty()) {
      config.file_path = logs_dir + "/" +
          (file_name.empty() ? "native_main.log" : file_name);
    }
    const std::string level = ReadString(arguments, "logLevel");
    config.level = spdlog::level::from_str(level.empty() ? "info" : level);
    config.max_files = 5;
    config.use_environment_level_override = true;
    config.manage_global_flush = true;
    vr::configure_logging(config);
    spdlog::info("[WindowsNative] logging configured: {}", config.file_path);
    result->Success();
    return;
  }
  if (method == "setNativeCompositorViewportRect") {
    if (compositor_) {
      compositor_->SetVideoViewportRect(
          static_cast<int>(ReadInt(arguments, "left")),
          static_cast<int>(ReadInt(arguments, "top")),
          static_cast<int>(ReadInt(arguments, "width")),
          static_cast<int>(ReadInt(arguments, "height")),
          static_cast<int>(ReadInt(arguments, "surfaceWidth")),
          static_cast<int>(ReadInt(arguments, "surfaceHeight")));
    }
    result->Success();
    return;
  }
  if (method == "requestNativeCompositorFlutterFrame") {
    // The final compositor is passive: Flutter's ordinary scheduler publishes
    // UI changes. Native video presentation must never pump Flutter frames.
    spdlog::debug("[WindowsCompositor] passive Flutter frame hint reason={}",
                  ReadString(arguments, "reason"));
    result->Success();
    return;
  }
  if (method == "setNativeAnalysisOverlay") {
    const auto* tracks_value = Find(arguments, "tracks");
    const auto* tracks = tracks_value
        ? std::get_if<flutter::EncodableList>(tracks_value)
        : nullptr;
    if (!tracks) {
      result->Error("BAD_ARGS", "tracks must be a list");
      return;
    }

    naki_analysis_clear_overlay_tracks();
    size_t loaded_track_count = 0;
    for (const auto& entry : *tracks) {
      const auto* track = std::get_if<EncodableMap>(&entry);
      if (!track) {
        naki_analysis_clear_overlay_tracks();
        result->Error("BAD_ARGS", "analysis overlay track must be a map");
        return;
      }
      const auto file = track->find(EncodableValue("fileId"));
      const auto path = track->find(EncodableValue("analysisPath"));
      int64_t file_id = -1;
      if (file != track->end()) {
        if (const auto* value = std::get_if<int32_t>(&file->second)) {
          file_id = *value;
        } else if (const auto* value = std::get_if<int64_t>(&file->second)) {
          file_id = *value;
        }
      }
      const auto* analysis_path = path != track->end()
                                      ? std::get_if<std::string>(&path->second)
                                      : nullptr;
      if (file_id < 0 || !analysis_path || analysis_path->empty() ||
          naki_analysis_set_overlay_track(
              static_cast<int32_t>(file_id), analysis_path->c_str()) == 0) {
        char message[256] = {};
        naki_analysis_last_error(message, sizeof(message));
        naki_analysis_clear_overlay_tracks();
        result->Error("ANALYSIS_OVERLAY_TRACK",
                      message[0] != '\0'
                          ? message
                          : "failed to load analysis overlay track");
        return;
      }
      ++loaded_track_count;
    }

    NakiOverlayState state{};
    state.show_cu_grid = ReadBool(arguments, "showCuGrid") ? 1 : 0;
    state.show_pred_mode = ReadBool(arguments, "showPredMode") ? 1 : 0;
    state.show_qp_heatmap = ReadBool(arguments, "showQpHeatmap") ? 1 : 0;
    state.show_pred_lines = ReadBool(arguments, "showPredLines") ? 1 : 0;
    state.show_cu_bit_cost_heatmap =
        ReadBool(arguments, "showCuBitCostHeatmap") ? 1 : 0;
    state.opacity_permille = static_cast<int32_t>(
        std::clamp<int64_t>(ReadInt(arguments, "opacityPermille", 550),
                            0, 1000));
    state.mode = static_cast<int32_t>(
        std::max<int64_t>(0, ReadInt(arguments, "mode")));
    state.track_file_id = static_cast<int32_t>(
        ReadInt(arguments, "trackFileId", -1));
    naki_analysis_set_overlay(&state);
    if (player_) {
      player_->request_frame_refresh("windows-analysis-overlay-state");
    }
    spdlog::info(
        "[WindowsAnalysisOverlay] synchronized tracks={} cu={} pred={} qp={} "
        "motion={} bit_cost={} mode={} opacity={}",
        loaded_track_count, state.show_cu_grid, state.show_pred_mode,
        state.show_qp_heatmap, state.show_pred_lines,
        state.show_cu_bit_cost_heatmap, state.mode, state.opacity_permille);
    result->Success();
    return;
  }
  if (method == "resetNativePerfCounters") {
    if (compositor_) {
      compositor_->ResetFlutterPublishSample();
    }
    result->Success();
    return;
  }
  if (method == "prewarmNativePresentationTargetSize" ||
      method == "boostNativeCompositorFlutterInteraction" ||
      method == "ackNativeCompositorFlutterState") {
    result->Success();
    return;
  }
  if (method == "pickFiles") {
    flutter::EncodableList files;
    for (const auto& path : file_picker_.PickVideoFiles(
             ReadBool(arguments, "allowMultiple", true), window_handle_)) {
      files.emplace_back(path);
    }
    result->Success(EncodableValue(std::move(files)));
    return;
  }
  if (method == "createPlayer") {
    DestroyPlayer();
    const auto paths = ReadStringList(arguments, "videoPaths");
    const int width = static_cast<int>(ReadInt(arguments, "width", 1920));
    const int height = static_cast<int>(ReadInt(arguments, "height", 1080));
    if (paths.empty()) {
      result->Error("BAD_ARGS", "createPlayer requires at least one path");
      return;
    }
    if (!compositor_ || !compositor_started_ || width <= 0 || height <= 0) {
      result->Error("TARGET_UNAVAILABLE",
                    "Windows native compositor is unavailable");
      return;
    }
    std::vector<void*> textures;
    if (!compositor_->CreateVideoTargetRing(
            static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            DXGI_FORMAT_B8G8R8A8_UNORM, 4, textures)) {
      result->Error("TARGET_UNAVAILABLE", compositor_->diagnostics().last_error);
      return;
    }
    video_target_width_ = width;
    video_target_height_ = height;
    vr::WindowsD3D11TargetRingInstall install;
    install.textures = reinterpret_cast<const void* const*>(textures.data());
    install.texture_count = textures.size();
    install.width = width;
    install.height = height;
    install.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    vr::RendererConfig config;
    config.video_paths = paths;
    config.width = width;
    config.height = height;
    config.use_hardware_decode =
        ReadBool(arguments, "useHardwareDecode", true);
    config.initial_file_id = 0;
    config.offscreen = true;
    config.backend.type = vr::RendererBackendType::NativeD3D11;
    config.backend.output = &install;
    config.backend.max_track_slots = 4;
    config.backend.output_target =
        vr::ColorOutputTarget::kSDRToneMappedBT709;
    auto player = std::make_shared<vr::WindowsNativePlayer>();
    if (!player->initialize(config)) {
      compositor_->ClearVideoTargetRing();
      result->Error("NATIVE_INIT_FAILED",
                    "shared Windows renderer failed to initialize");
      return;
    }
    player_ = player;
    ApplyViewportBackgroundColor(
        static_cast<uint32_t>(ReadInt(arguments, "color", 0xFF000000)));
    player_id_ = next_player_id_.fetch_add(1);
    vr::WindowsFirstFrameActivationGate::Session presentation_session = 0;
    {
      std::lock_guard<std::mutex> lock(presentation_state_mutex_);
      presentation_session_ = first_frame_activation_gate_.begin_session();
      presentation_session = presentation_session_;
    }
    const std::weak_ptr<vr::WindowsNativePlayer> weak_player = player;
    player->set_frame_callback(
        [this, weak_player, presentation_session](
            const vr::PresentationBackendFrameInfo* frame) {
          OnFrameAvailable(weak_player, presentation_session, frame);
        });
    player->set_frame_failure_callback([](const char*) {});
    player->set_event_callback(
        [this](const vr::RendererEvent& event) { event_bridge_.Queue(event); });
    if (viewport_presentation_controller_) {
      viewport_presentation_controller_->AttachPlayer(player);
    }
    std::string policy_error;
    if (!RefreshPresentationPolicy("windows-runner-create", policy_error)) {
      DestroyPlayer();
      result->Error("PRESENTATION_POLICY_FAILED", policy_error);
      return;
    }
    EncodableMap payload = {
        {EncodableValue("playerId"), EncodableValue(player_id_)},
        {EncodableValue("tracks"), EncodableValue(TrackList(player->tracks()))},
    };
    result->Success(EncodableValue(std::move(payload)));
    return;
  }
  if (method == "destroyPlayer") {
    DestroyPlayer();
    result->Success();
    return;
  }
  if (method == "debugNativeCompositor") {
    EncodableMap diagnostics;
    AddCompositorDiagnostics(diagnostics, compositor_.get(), compositor_started_,
                             viewport_presentation_controller_.get());
    AddPresentationPolicyDiagnostics(diagnostics, presentation_policy_,
                                     display_probe_,
                                     presentation_sdr_white_level_nits_);
    result->Success(EncodableValue(std::move(diagnostics)));
    return;
  }
  if (method == "getDiagnostics") {
    const ProcessMemorySnapshot process_memory = QueryProcessMemory();
    EncodableMap diagnostics = {
        {EncodableValue("available"), EncodableValue(player_ != nullptr)},
        {EncodableValue("presentationBackend"),
         EncodableValue("windows-native-d3d11")},
        {EncodableValue("windowsBackendRebuildRequired"),
         EncodableValue(false)},
        {EncodableValue("trackCount"),
         EncodableValue(player_ ? static_cast<int64_t>(player_->tracks().size())
                                : int64_t{0})},
        {EncodableValue("isPlaying"),
         EncodableValue(player_ && player_->is_playing())},
        {EncodableValue("hardwareDecodeActive"), EncodableValue(false)},
        {EncodableValue("hardwareDecodeDownloadsToCpu"),
         EncodableValue(false)},
        {EncodableValue("processRssBytes"),
         EncodableValue(SaturatingInt64(process_memory.rss_bytes))},
        {EncodableValue("processPrivateBytes"),
         EncodableValue(SaturatingInt64(process_memory.private_bytes))},
        {EncodableValue("dedicatedGpuUsageBytes"), EncodableValue(int64_t{0})},
        {EncodableValue("cpuFrameMemoryBytes"), EncodableValue(int64_t{0})},
        {EncodableValue("packetQueueMemoryBytes"), EncodableValue(int64_t{0})},
        {EncodableValue("nativeTrackDiagnostics"),
         EncodableValue(flutter::EncodableList{})},
        {EncodableValue("nativeTrackDiagnosticCount"),
         EncodableValue(int64_t{0})},
    };
    AddCompositorDiagnostics(diagnostics, compositor_.get(),
                             compositor_started_,
                             viewport_presentation_controller_.get());
    AddPresentationPolicyDiagnostics(diagnostics, presentation_policy_,
                                     display_probe_,
                                     presentation_sdr_white_level_nits_);
    if (player_) {
      const auto tracks = player_->tracks();
      const auto track_perf = player_->track_perf_stats();
      const auto memory = player_->gpu_memory_stats();
      const auto renderer_metrics = player_->presentation_metrics();
      const auto stats = player_->presentation_stats();
      const auto backend = player_->presentation_diagnostics();
      const bool hardware_decode_active =
          std::any_of(memory.tracks.begin(), memory.tracks.end(),
                      [](const auto& track) { return track.hardware_enabled; });
      const bool hardware_decode_downloads_to_cpu =
          std::any_of(memory.tracks.begin(), memory.tracks.end(),
                      [](const auto& track) {
                        return track.hardware_enabled &&
                               track.hardware_download_to_cpu;
                      });
      auto track_diagnostics =
          TrackDiagnosticList(*player_, tracks, track_perf, memory);
      uint64_t dedicated_gpu_usage =
          QueryDedicatedGpuUsage(compositor_ ? compositor_->device() : nullptr);
      if (dedicated_gpu_usage == 0) {
        dedicated_gpu_usage =
            memory.decoder_pool_bytes + memory.presenter_texture_bytes +
            memory.fp16_target_bytes + memory.analysis_overlay_bytes;
      }
      diagnostics[EncodableValue("nativeTrackDiagnostics")] =
          EncodableValue(std::move(track_diagnostics));
      diagnostics[EncodableValue("nativeTrackDiagnosticCount")] =
          EncodableValue(static_cast<int64_t>(track_perf.size()));
      diagnostics[EncodableValue("hardwareDecodeActive")] =
          EncodableValue(hardware_decode_active);
      diagnostics[EncodableValue("hardwareDecodeDownloadsToCpu")] =
          EncodableValue(hardware_decode_downloads_to_cpu);
      diagnostics[EncodableValue("dedicatedGpuUsageBytes")] =
          EncodableValue(SaturatingInt64(dedicated_gpu_usage));
      diagnostics[EncodableValue("cpuFrameMemoryBytes")] =
          EncodableValue(SaturatingInt64(memory.cpu_frame_bytes));
      diagnostics[EncodableValue("nativeCpuFrameMemoryBytes")] =
          EncodableValue(SaturatingInt64(memory.cpu_frame_bytes));
      diagnostics[EncodableValue("packetQueueMemoryBytes")] =
          EncodableValue(SaturatingInt64(memory.packet_queue_bytes));
      diagnostics[EncodableValue("nativePacketQueueMemoryBytes")] =
          EncodableValue(SaturatingInt64(memory.packet_queue_bytes));
      diagnostics[EncodableValue("nativeRendererDrawP95Us")] =
          EncodableValue(SaturatingInt64(renderer_metrics.draw_p95_us));
      diagnostics[EncodableValue("nativeRendererDrawWorkP95Us")] =
          EncodableValue(SaturatingInt64(renderer_metrics.draw_work_p95_us));
      diagnostics[EncodableValue("nativeRendererDrawCallbackP95Us")] =
          EncodableValue(
              SaturatingInt64(renderer_metrics.draw_callback_p95_us));
      diagnostics[EncodableValue("nativeRendererDrawBlockingWaitP95Us")] =
          EncodableValue(
              SaturatingInt64(renderer_metrics.draw_blocking_wait_p95_us));
      diagnostics[EncodableValue("nativeRendererDrawBackendP95Us")] =
          EncodableValue(SaturatingInt64(renderer_metrics.draw_backend_p95_us));
      diagnostics[EncodableValue("nativeRendererDrawBackendWorkP95Us")] =
          EncodableValue(
              SaturatingInt64(renderer_metrics.draw_backend_work_p95_us));
      diagnostics[EncodableValue("nativeTargetStagingAllocationCount")] =
          EncodableValue(
              SaturatingInt64(stats.staging_allocation_count));
      diagnostics[EncodableValue("nativeTargetStagingReuseCount")] =
          EncodableValue(SaturatingInt64(stats.staging_reuse_count));
      diagnostics[EncodableValue("nativeTargetStagingMaxBytes")] =
          EncodableValue(SaturatingInt64(stats.staging_max_bytes));
      diagnostics[EncodableValue("nativeTargetRendererInitialized")] =
          EncodableValue(stats.backend_available != 0);
      diagnostics[EncodableValue("nativeTargetLastDrawSucceeded")] =
          EncodableValue(stats.last_draw_succeeded != 0);
      diagnostics[EncodableValue("nativeFramePresentationCount")] =
          EncodableValue(static_cast<int64_t>(stats.viewport_composite_count));
      diagnostics[EncodableValue("videoSourceUpdateCount")] =
          EncodableValue(static_cast<int64_t>(stats.video_source_update_count));
      diagnostics[EncodableValue("sourceFrameCacheHitCount")] = EncodableValue(
          static_cast<int64_t>(stats.source_frame_cache_hit_count));
      diagnostics[EncodableValue("sourceFrameCacheMissCount")] = EncodableValue(
          static_cast<int64_t>(stats.source_frame_cache_miss_count));
      diagnostics[EncodableValue("nativeTargetOverlayLastExpected")] =
          EncodableValue(stats.overlay_last_expected != 0);
      diagnostics[EncodableValue("nativeTargetOverlayLastApplied")] =
          EncodableValue(stats.overlay_last_applied != 0);
      diagnostics[EncodableValue("nativeTargetOverlayLastFillRectCount")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_last_fill_rect_count));
      diagnostics[EncodableValue("nativeTargetOverlayLastLineRectCount")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_last_line_rect_count));
      diagnostics[EncodableValue("nativeTargetOverlayExpectedCount")] =
          EncodableValue(static_cast<int64_t>(stats.overlay_expected_count));
      diagnostics[EncodableValue("nativeTargetOverlayAppliedCount")] =
          EncodableValue(static_cast<int64_t>(stats.overlay_applied_count));
      diagnostics[EncodableValue("nativeTargetOverlayMissedCount")] =
          EncodableValue(static_cast<int64_t>(stats.overlay_missed_count));
      diagnostics[EncodableValue("nativeTargetOverlayGpuSuccessCount")] =
          EncodableValue(static_cast<int64_t>(stats.overlay_gpu_success_count));
      diagnostics[EncodableValue("nativeTargetOverlayGpuFailureCount")] =
          EncodableValue(static_cast<int64_t>(stats.overlay_gpu_failure_count));
      diagnostics[EncodableValue("nativeTargetOverlayCpuFallbackCount")] =
          EncodableValue(
              static_cast<int64_t>(stats.overlay_cpu_fallback_count));
      diagnostics[EncodableValue("nativeTargetOverlaySourceCacheHitCount")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_source_cache_hit_count));
      diagnostics[EncodableValue("nativeTargetOverlaySourceCacheMissCount")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_source_cache_miss_count));
      diagnostics[EncodableValue("nativeTargetOverlayGpuUploadCount")] =
          EncodableValue(
              static_cast<int64_t>(stats.overlay_gpu_upload_count));
      diagnostics[EncodableValue("nativeTargetOverlayGpuBufferReuseCount")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_gpu_buffer_reuse_count));
      diagnostics[EncodableValue("nativeTargetOverlayGpuUploadBytes")] =
          EncodableValue(
              static_cast<int64_t>(stats.overlay_gpu_upload_bytes));
      diagnostics[EncodableValue("nativeTargetOverlayLastSourceGeneration")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_last_source_generation));
      diagnostics[EncodableValue("nativeTargetOverlayLastLookupUs")] =
          EncodableValue(
              static_cast<int64_t>(stats.overlay_last_lookup_us));
      diagnostics[EncodableValue("nativeTargetOverlayLastUploadUs")] =
          EncodableValue(
              static_cast<int64_t>(stats.overlay_last_upload_us));
      diagnostics[EncodableValue("nativeTargetOverlayLastCpuSubmitUs")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_last_cpu_submit_us));
      diagnostics[EncodableValue("nativeTargetOverlayMaxCpuSubmitUs")] =
          EncodableValue(static_cast<int64_t>(
              stats.overlay_max_cpu_submit_us));
      diagnostics[EncodableValue("nativeTargetConsecutiveDrawFailures")] =
          EncodableValue(static_cast<int64_t>(stats.consecutive_draw_failures));
      diagnostics[EncodableValue("nativeTargetBackendName")] =
          EncodableValue(backend.backend);
      diagnostics[EncodableValue("presentationFallbackReason")] =
          EncodableValue(backend.fallback_reason);
      diagnostics[EncodableValue("textureWidth")] =
          EncodableValue(backend.width);
      diagnostics[EncodableValue("textureHeight")] =
          EncodableValue(backend.height);
    }
    result->Success(EncodableValue(std::move(diagnostics)));
    return;
  }
  if (!player_) {
    if (method == "getTracks") {
      result->Success(EncodableValue(flutter::EncodableList{}));
    } else {
      result->Error("NO_PLAYER", "createPlayer must be called first");
    }
    return;
  }
  if (method == "play") {
    player_->play();
    result->Success();
  } else if (method == "pause") {
    player_->pause();
    result->Success();
  } else if (method == "seek") {
    player_->seek(ReadInt(arguments, "ptsUs"),
                  ReadInt(arguments, "requestId", -1));
    result->Success();
  } else if (method == "stepForward") {
    player_->step_forward();
    result->Success();
  } else if (method == "stepBackward") {
    player_->step_backward();
    result->Success();
  } else if (method == "setSpeed") {
    player_->set_speed(ReadDouble(arguments, "speed", 1.0));
    result->Success();
  } else if (method == "setLoopRange") {
    player_->set_loop_range(ReadBool(arguments, "enabled"),
                            ReadInt(arguments, "startUs"),
                            ReadInt(arguments, "endUs"));
    result->Success();
  } else if (method == "setAudibleTrack") {
    player_->set_audible_track(static_cast<int>(ReadInt(arguments, "fileId", -1)));
    result->Success();
  } else if (method == "setTrackOffset") {
    player_->set_track_offset(
        static_cast<int>(ReadInt(arguments, "fileId", -1)),
        ReadInt(arguments, "offsetUs"));
    result->Success();
  } else if (method == "setViewportBackgroundColor") {
    const uint32_t color = static_cast<uint32_t>(ReadInt(arguments, "color"));
    ApplyViewportBackgroundColor(color);
    result->Success();
  } else if (method == "resize") {
    std::string error;
    if (!ResizeVideoTargets(static_cast<int>(ReadInt(arguments, "width")),
                            static_cast<int>(ReadInt(arguments, "height")),
                            error)) {
      result->Error("RESIZE_FAILED", error);
    } else {
      vr::WindowsFirstFrameActivationGate::Session presentation_session = 0;
      {
        std::lock_guard<std::mutex> lock(presentation_state_mutex_);
        presentation_session = presentation_session_;
      }
      (void)first_frame_activation_gate_.commit_initial_viewport(
          presentation_session);
      player_->request_frame_refresh("windows-runner-resize");
      result->Success();
    }
  } else if (method == "applyLayout") {
    const vr::LayoutState layout = ReadLayout(arguments);
    const uint64_t count = ++layout_apply_count_;
    if (count <= 12 || count % 60 == 0) {
      spdlog::info(
          "[WindowsLayout] runner intent={} playing={} mode={} zoom={:.4f} "
          "offset=({:.1f},{:.1f}) split={:.4f} pixel_mode={}",
          count, player_->is_playing(), layout.mode, layout.zoom_ratio,
          layout.view_offset[0], layout.view_offset[1], layout.split_pos,
          layout.pixel_size_mode);
    }
    player_->apply_interaction_layout(layout);
    bool native_viewport_active = false;
    {
      std::lock_guard<std::mutex> lock(presentation_state_mutex_);
      native_viewport_active =
          first_frame_activation_gate_.active(presentation_session_);
    }
    if (viewport_presentation_controller_ && native_viewport_active) {
      // Before first-frame activation there is no cached source frame to
      // reproject. The shared renderer has already retained this layout and
      // will use it for the initial preview; submitting an interaction draw
      // here only records a synthetic backend failure during startup.
      viewport_presentation_controller_->RequestLayoutFrame();
    }
    result->Success();
  } else if (method == "getLayout") {
    result->Success(EncodableValue(LayoutMap(player_->layout())));
  } else if (method == "getTracks") {
    result->Success(EncodableValue(TrackList(player_->tracks())));
  } else if (method == "addTrack") {
    const int file_id =
        player_->add_track(ReadString(arguments, "path"),
                           ReadBool(arguments, "useHardwareDecode", true));
    const auto tracks = player_->tracks();
    const auto found = std::find_if(
        tracks.begin(), tracks.end(),
        [file_id](const auto& track) { return track.file_id == file_id; });
    if (file_id < 0 || found == tracks.end()) {
      result->Error("ADD_TRACK_FAILED", "native renderer rejected the track");
    } else {
      std::string policy_error;
      if (!RefreshPresentationPolicy("windows-add-track", policy_error)) {
        player_->remove_track(file_id);
        std::string restore_error;
        if (!RefreshPresentationPolicy(
                "windows-add-track-rollback", restore_error)) {
          FailClosedPresentation(std::move(restore_error));
        }
        result->Error("PRESENTATION_POLICY_FAILED", policy_error);
        return;
      }
      result->Success(EncodableValue(TrackMap(*found)));
    }
  } else if (method == "removeTrack") {
    player_->remove_track(static_cast<int>(ReadInt(arguments, "fileId", -1)));
    std::string policy_error;
    if (!RefreshPresentationPolicy("windows-remove-track", policy_error)) {
      spdlog::warn(
          "[WindowsPresentation] remove-track policy refresh failed: {}",
          policy_error);
    }
    result->Success();
  } else if (method == "currentPts") {
    result->Success(EncodableValue(player_->current_pts_us()));
  } else if (method == "duration") {
    result->Success(EncodableValue(player_->duration_us()));
  } else if (method == "isPlaying") {
    result->Success(EncodableValue(player_->is_playing()));
  } else if (method == "currentPresentedFrame") {
    vr::PresentationBackendFrameInfo frame = {};
    if (!player_->copy_last_frame_info(&frame)) {
      result->Success();
    } else {
      result->Success(EncodableValue(FrameMap(
          static_cast<int>(ReadInt(arguments, "fileId", -1)), frame)));
    }
  } else if (method == "getPlaybackSnapshot") {
    EncodableMap snapshot = {
        {EncodableValue("currentPtsUs"),
         EncodableValue(player_->current_pts_us())},
        {EncodableValue("durationUs"), EncodableValue(player_->duration_us())},
        {EncodableValue("isPlaying"), EncodableValue(player_->is_playing())},
    };
    if (ReadBool(arguments, "includePresentedFrames")) {
      flutter::EncodableList frames;
      vr::PresentationBackendFrameInfo frame = {};
      const auto tracks = player_->tracks();
      if (!tracks.empty() && player_->copy_last_frame_info(&frame)) {
        frames.emplace_back(FrameMap(tracks.front().file_id, frame));
      }
      snapshot[EncodableValue("presentedFrames")] =
          EncodableValue(std::move(frames));
    }
    result->Success(EncodableValue(std::move(snapshot)));
  } else if (method == "captureViewport") {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!player_->capture_front_buffer(bgra, width, height)) {
      result->Error("CAPTURE_FAILED", "native viewport capture failed");
    } else {
      result->Success(EncodableValue(CaptureMap(
          bgra, width, height, ReadString(arguments, "outputPath"))));
    }
  } else if (method == "captureViewportRegion") {
    const int requested_width =
        static_cast<int>(ReadInt(arguments, "width"));
    const int requested_height =
        static_cast<int>(ReadInt(arguments, "height"));
    if (requested_width <= 0 || requested_height <= 0) {
      result->Error("BAD_ARGS", "width and height must be positive");
      return;
    }
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!player_->capture_front_buffer_region(
            static_cast<int>(ReadInt(arguments, "x")),
            static_cast<int>(ReadInt(arguments, "y")),
            requested_width, requested_height, bgra, width, height)) {
      result->Error("CAPTURE_FAILED", "native viewport region capture failed");
    } else {
      ScaleBgraToMaxSize(bgra, width, height,
                         static_cast<int>(ReadInt(arguments, "maxSize")));
      result->Success(EncodableValue(CaptureMap(
          bgra, width, height, ReadString(arguments, "outputPath"))));
    }
  } else {
    result->NotImplemented();
  }
}
