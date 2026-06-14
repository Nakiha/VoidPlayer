#include "native_diagnostics_provider.h"

#include "windows/player/native_player.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

namespace {

flutter::EncodableMap make_gpu_breakdown_map(const vr::RendererGpuMemoryStats& stats) {
    flutter::EncodableMap map;
    map[flutter::EncodableValue("totalEstimatedBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.total_estimated_bytes));
    map[flutter::EncodableValue("decoderPoolBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.decoder_pool_bytes));
    map[flutter::EncodableValue("exactSeekSnapshotBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.exact_seek_snapshot_bytes));
    map[flutter::EncodableValue("presenterTextureBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.presenter_texture_bytes));
    map[flutter::EncodableValue("headlessOutputBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.headless_output_bytes));
    map[flutter::EncodableValue("fp16TargetBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.fp16_target_bytes));
    map[flutter::EncodableValue("analysisOverlayBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.analysis_overlay_bytes));
    map[flutter::EncodableValue("cpuFrameBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.cpu_frame_bytes));
    map[flutter::EncodableValue("trackBufferCpuBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.track_buffer_cpu_bytes));
    map[flutter::EncodableValue("packetQueueBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.packet_queue_bytes));
    map[flutter::EncodableValue("exactSeekCandidateCpuBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.exact_seek_candidate_cpu_bytes));
    map[flutter::EncodableValue("exactSeekStableCpuBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.exact_seek_stable_cpu_bytes));
    map[flutter::EncodableValue("exactSeekBudgetDropCount")] =
        flutter::EncodableValue(static_cast<int64_t>(stats.exact_seek_budget_drop_count));
    map[flutter::EncodableValue("headlessWidth")] =
        flutter::EncodableValue(stats.headless_width);
    map[flutter::EncodableValue("headlessHeight")] =
        flutter::EncodableValue(stats.headless_height);
    map[flutter::EncodableValue("headlessBufferCount")] =
        flutter::EncodableValue(stats.headless_buffer_count);
    map[flutter::EncodableValue("analysisOverlayWidth")] =
        flutter::EncodableValue(stats.analysis_overlay_width);
    map[flutter::EncodableValue("analysisOverlayHeight")] =
        flutter::EncodableValue(stats.analysis_overlay_height);

    flutter::EncodableList tracks;
    for (const auto& track : stats.tracks) {
        flutter::EncodableMap tm;
        tm[flutter::EncodableValue("slot")] = flutter::EncodableValue(track.slot);
        tm[flutter::EncodableValue("fileId")] = flutter::EncodableValue(track.file_id);
        tm[flutter::EncodableValue("hardwareEnabled")] =
            flutter::EncodableValue(track.hardware_enabled);
        tm[flutter::EncodableValue("hardwareDownloadToCpu")] =
            flutter::EncodableValue(track.hardware_download_to_cpu);
        tm[flutter::EncodableValue("hwFormat")] = flutter::EncodableValue(track.hw_format);
        tm[flutter::EncodableValue("swFormat")] = flutter::EncodableValue(track.sw_format);
        tm[flutter::EncodableValue("hwWidth")] = flutter::EncodableValue(track.hw_width);
        tm[flutter::EncodableValue("hwHeight")] = flutter::EncodableValue(track.hw_height);
        tm[flutter::EncodableValue("hwInitialPoolSize")] =
            flutter::EncodableValue(track.hw_initial_pool_size);
        tm[flutter::EncodableValue("extraHwFrames")] =
            flutter::EncodableValue(track.extra_hw_frames);
        tm[flutter::EncodableValue("decoderFrameBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.decoder_frame_bytes));
        tm[flutter::EncodableValue("decoderPoolBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.decoder_pool_bytes));
        tm[flutter::EncodableValue("exactSeekSnapshotBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_snapshot_bytes));
        tm[flutter::EncodableValue("presenterCopyTextureBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.presenter_copy_texture_bytes));
        tm[flutter::EncodableValue("trackBufferCpuBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.track_buffer_cpu_bytes));
        tm[flutter::EncodableValue("packetQueueBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.packet_queue_bytes));
        tm[flutter::EncodableValue("exactSeekCandidateCpuBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_candidate_cpu_bytes));
        tm[flutter::EncodableValue("exactSeekStableCpuBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_stable_cpu_bytes));
        tm[flutter::EncodableValue("totalCpuFrameBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(track.total_cpu_frame_bytes));
        tm[flutter::EncodableValue("exactSeekReorderCount")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_reorder_count));
        tm[flutter::EncodableValue("exactSeekPendingCount")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_pending_count));
        tm[flutter::EncodableValue("exactSeekStableFrameCount")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_stable_frame_count));
        tm[flutter::EncodableValue("exactSeekBudgetDropCount")] =
            flutter::EncodableValue(static_cast<int64_t>(track.exact_seek_budget_drop_count));
        tm[flutter::EncodableValue("bufferCount")] =
            flutter::EncodableValue(static_cast<int64_t>(track.buffer_count));
        tm[flutter::EncodableValue("bufferCapacity")] =
            flutter::EncodableValue(static_cast<int64_t>(track.buffer_capacity));
        tracks.push_back(flutter::EncodableValue(tm));
    }
    map[flutter::EncodableValue("tracks")] = flutter::EncodableValue(tracks);
    return map;
}

std::string feature_level_name(int feature_level) {
    switch (static_cast<D3D_FEATURE_LEVEL>(feature_level)) {
    case D3D_FEATURE_LEVEL_11_0:
        return "11.0";
    case D3D_FEATURE_LEVEL_10_1:
        return "10.1";
    case D3D_FEATURE_LEVEL_10_0:
        return "10.0";
    default:
        return "unknown";
    }
}

} // namespace

ProcessMemoryUsage NativeDiagnosticsProvider::QueryProcessMemoryUsage() const {
    ProcessMemoryUsage usage;
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        usage.working_set_bytes = static_cast<uint64_t>(counters.WorkingSetSize);
        usage.private_bytes = static_cast<uint64_t>(counters.PrivateUsage);
    }
    return usage;
}

ProcessHeapUsage NativeDiagnosticsProvider::QueryProcessHeapUsage() const {
    ProcessHeapUsage usage;
    DWORD count = GetProcessHeaps(0, nullptr);
    if (count == 0) {
        return usage;
    }

    std::vector<HANDLE> heaps(count);
    DWORD written = GetProcessHeaps(count, heaps.data());
    if (written == 0) {
        return usage;
    }
    if (written > count) {
        heaps.resize(written);
        written = GetProcessHeaps(written, heaps.data());
        if (written == 0) {
            return usage;
        }
    }

    usage.heap_count = written;
    for (DWORD i = 0; i < written; ++i) {
        HEAP_SUMMARY summary = {};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heaps[i], 0, &summary)) {
            continue;
        }
        usage.allocated_bytes += static_cast<uint64_t>(summary.cbAllocated);
        usage.committed_bytes += static_cast<uint64_t>(summary.cbCommitted);
        usage.reserved_bytes += static_cast<uint64_t>(summary.cbReserved);
    }
    return usage;
}

uint64_t NativeDiagnosticsProvider::QueryDedicatedVideoMemoryUsage() const {
    using Clock = std::chrono::steady_clock;
    static std::mutex cache_mutex;
    static Clock::time_point last_query{};
    static uint64_t cached_usage = 0;
    static constexpr auto kCacheTtl = std::chrono::seconds(2);

    const auto now = Clock::now();
    {
        std::lock_guard lock(cache_mutex);
        if (last_query != Clock::time_point{} && now - last_query < kCacheTtl) {
            return cached_usage;
        }
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        std::lock_guard lock(cache_mutex);
        last_query = now;
        cached_usage = 0;
        return cached_usage;
    }

    uint64_t total_usage = 0;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter.As(&adapter3)) || !adapter3) {
            continue;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            total_usage += static_cast<uint64_t>(info.CurrentUsage);
        }
    }
    std::lock_guard lock(cache_mutex);
    last_query = now;
    cached_usage = total_usage;
    return cached_usage;
}

flutter::EncodableMap NativeDiagnosticsProvider::BuildMethodChannelDiagnostics(
    const std::shared_ptr<vr::NativePlayer>& active_player,
    const vr::WindowsDisplayProbeSnapshot& display,
    const vr::WindowsPresentationPolicy& presentation_policy,
    const std::string& presentation_sdr_white_level_status) const {
    flutter::EncodableMap map;
    const auto process_memory = QueryProcessMemoryUsage();
    const auto process_heap = QueryProcessHeapUsage();
    map[flutter::EncodableValue("processRssBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(process_memory.working_set_bytes));
    map[flutter::EncodableValue("processPrivateBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(process_memory.private_bytes));
    map[flutter::EncodableValue("processHeapAllocatedBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(process_heap.allocated_bytes));
    map[flutter::EncodableValue("processHeapCommittedBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(process_heap.committed_bytes));
    map[flutter::EncodableValue("processHeapReservedBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(process_heap.reserved_bytes));
    map[flutter::EncodableValue("processHeapCount")] =
        flutter::EncodableValue(static_cast<int64_t>(process_heap.heap_count));
    map[flutter::EncodableValue("dedicatedGpuUsageBytes")] =
        flutter::EncodableValue(static_cast<int64_t>(QueryDedicatedVideoMemoryUsage()));
    const auto& probe = display.probe;
    map[flutter::EncodableValue("windowsDisplayProbeStatus")] =
        flutter::EncodableValue(probe.status);
    map[flutter::EncodableValue("windowsDisplayOutputResolved")] =
        flutter::EncodableValue(probe.output_resolved);
    map[flutter::EncodableValue("windowsDisplayColorMetadataAvailable")] =
        flutter::EncodableValue(probe.color_metadata_available);
    map[flutter::EncodableValue("windowsDisplaySelectionReason")] =
        flutter::EncodableValue(probe.selection_reason);
    map[flutter::EncodableValue("windowsDisplayDeviceName")] =
        flutter::EncodableValue(probe.device_name);
    map[flutter::EncodableValue("windowsDisplayAdapterDescription")] =
        flutter::EncodableValue(probe.adapter_description);
    map[flutter::EncodableValue("windowsDisplayAdapterLuid")] =
        flutter::EncodableValue(
            std::to_string(probe.adapter_luid_high) + ":" +
            std::to_string(probe.adapter_luid_low));
    map[flutter::EncodableValue("windowsDisplayMatchesPresentationAdapter")] =
        flutter::EncodableValue(probe.matches_presentation_adapter);
    map[flutter::EncodableValue("windowsDisplayDesktopLeft")] =
        flutter::EncodableValue(probe.desktop_left);
    map[flutter::EncodableValue("windowsDisplayDesktopTop")] =
        flutter::EncodableValue(probe.desktop_top);
    map[flutter::EncodableValue("windowsDisplayDesktopWidth")] =
        flutter::EncodableValue(probe.desktop_width);
    map[flutter::EncodableValue("windowsDisplayDesktopHeight")] =
        flutter::EncodableValue(probe.desktop_height);
    map[flutter::EncodableValue("windowsDisplayIntersectionArea")] =
        flutter::EncodableValue(probe.intersection_area);
    map[flutter::EncodableValue("windowsDisplayRotation")] =
        flutter::EncodableValue(probe.rotation);
    map[flutter::EncodableValue("windowsDisplayBitsPerColor")] =
        flutter::EncodableValue(probe.bits_per_color);
    map[flutter::EncodableValue("windowsDisplayColorSpace")] =
        flutter::EncodableValue(probe.color_space);
    map[flutter::EncodableValue("windowsDisplayAdvancedColorState")] =
        flutter::EncodableValue(probe.advanced_color_state);
    map[flutter::EncodableValue("windowsDisplayHDRActive")] =
        flutter::EncodableValue(probe.hdr_active);
    map[flutter::EncodableValue("windowsDisplayMinLuminanceMilliNits")] =
        flutter::EncodableValue(probe.min_luminance_milli_nits);
    map[flutter::EncodableValue("windowsDisplayMaxLuminanceMilliNits")] =
        flutter::EncodableValue(probe.max_luminance_milli_nits);
    map[flutter::EncodableValue("windowsDisplayMaxFullFrameLuminanceMilliNits")] =
        flutter::EncodableValue(
            probe.max_full_frame_luminance_milli_nits);
    map[flutter::EncodableValue("windowsDisplaySDRWhiteLevelStatus")] =
        flutter::EncodableValue(probe.sdr_white_level_status);
    map[flutter::EncodableValue("windowsDisplaySDRWhiteLevelMilliNits")] =
        flutter::EncodableValue(probe.sdr_white_level_milli_nits);
    map[flutter::EncodableValue("windowsDisplayProbeGeneration")] =
        flutter::EncodableValue(static_cast<int64_t>(display.generation));
    map[flutter::EncodableValue("windowsDisplayChangeCount")] =
        flutter::EncodableValue(static_cast<int64_t>(display.change_count));
    map[flutter::EncodableValue("windowsDisplayLastChangeReason")] =
        flutter::EncodableValue(display.last_change_reason);

    if (!active_player) {
        return map;
    }

    map[flutter::EncodableValue("d3dDeviceLost")] =
        flutter::EncodableValue(active_player->d3d_device_lost());
    map[flutter::EncodableValue("d3dDeviceRemovedReason")] =
        flutter::EncodableValue(static_cast<int64_t>(active_player->d3d_device_removed_reason()));
    const auto presentation =
        active_player->presentation_backend_diagnostics();
    map[flutter::EncodableValue("windowsPresentationRequest")] =
        flutter::EncodableValue(presentation_policy.request);
    map[flutter::EncodableValue("windowsPresentationMode")] =
        flutter::EncodableValue(
            presentation.fp16_target_active
                ? presentation_policy.mode
                : (presentation.headless
                       ? "flutter-texture-sdr"
                       : "swap-chain-sdr"));
    map[flutter::EncodableValue("windowsPresentationReason")] =
        flutter::EncodableValue(presentation_policy.reason);
    map[flutter::EncodableValue("windowsPresentationBackend")] =
        flutter::EncodableValue(presentation.backend);
    map[flutter::EncodableValue("windowsPresentationTargetFormat")] =
        flutter::EncodableValue(presentation.target_format);
    map[flutter::EncodableValue("windowsPresentationRenderTargetFormat")] =
        flutter::EncodableValue(presentation.render_target_format);
    map[flutter::EncodableValue("windowsPresentationRenderColorSpace")] =
        flutter::EncodableValue(presentation.render_color_space);
    map[flutter::EncodableValue("windowsPresentationFP16TargetActive")] =
        flutter::EncodableValue(presentation.fp16_target_active);
    map[flutter::EncodableValue("windowsPresentationFP16TargetWidth")] =
        flutter::EncodableValue(presentation.fp16_target_width);
    map[flutter::EncodableValue("windowsPresentationFP16TargetHeight")] =
        flutter::EncodableValue(presentation.fp16_target_height);
    map[flutter::EncodableValue("windowsPresentationFP16TargetBufferCount")] =
        flutter::EncodableValue(presentation.fp16_target_buffer_count);
    map[flutter::EncodableValue("windowsPresentationSDRCompatibilityPass")] =
        flutter::EncodableValue(presentation.sdr_compatibility_pass);
    map[flutter::EncodableValue("windowsPresentationSDRWhiteLevelStatus")] =
        flutter::EncodableValue(presentation_sdr_white_level_status);
    map[flutter::EncodableValue("windowsPresentationSDRWhiteLevelMilliNits")] =
        flutter::EncodableValue(presentation.sdr_white_level_milli_nits);
    map[flutter::EncodableValue("windowsPresentationSDRWhiteScaleX1000")] =
        flutter::EncodableValue(presentation.sdr_white_scale_x1000);
    map[flutter::EncodableValue("windowsPresentationFP16DrawCount")] =
        flutter::EncodableValue(
            static_cast<int64_t>(presentation.fp16_draw_count));
    map[flutter::EncodableValue(
        "windowsPresentationSDRCompatibilityDrawCount")] =
        flutter::EncodableValue(static_cast<int64_t>(
            presentation.sdr_compatibility_draw_count));
    const std::string fallback_reason =
        presentation.fallback_reason != "none"
            ? presentation.fallback_reason
            : presentation_policy.fallback_reason;
    map[flutter::EncodableValue("windowsPresentationFallbackReason")] =
        flutter::EncodableValue(fallback_reason);
    map[flutter::EncodableValue("windowsPresentationWidth")] =
        flutter::EncodableValue(presentation.width);
    map[flutter::EncodableValue("windowsPresentationHeight")] =
        flutter::EncodableValue(presentation.height);
    map[flutter::EncodableValue("windowsPresentationBufferCount")] =
        flutter::EncodableValue(presentation.buffer_count);
    map[flutter::EncodableValue("windowsPresentationHeadless")] =
        flutter::EncodableValue(presentation.headless);
    map[flutter::EncodableValue("windowsPresentationCompositorActive")] =
        flutter::EncodableValue(false);
    map[flutter::EncodableValue("windowsD3DAdapterDescription")] =
        flutter::EncodableValue(presentation.adapter_description);
    map[flutter::EncodableValue("windowsD3DAdapterVendorId")] =
        flutter::EncodableValue(presentation.adapter_vendor_id);
    map[flutter::EncodableValue("windowsD3DAdapterDeviceId")] =
        flutter::EncodableValue(presentation.adapter_device_id);
    map[flutter::EncodableValue("windowsD3DAdapterLuid")] =
        flutter::EncodableValue(
            std::to_string(presentation.adapter_luid_high) + ":" +
            std::to_string(presentation.adapter_luid_low));
    map[flutter::EncodableValue("windowsD3DFeatureLevel")] =
        flutter::EncodableValue(feature_level_name(presentation.feature_level));
    map[flutter::EncodableValue("windowsD3DDriverType")] =
        flutter::EncodableValue(presentation.driver_type);
    map[flutter::EncodableValue("windowsD3DWarp")] =
        flutter::EncodableValue(presentation.warp);
    map[flutter::EncodableValue("playbackTime")] =
        flutter::EncodableValue(static_cast<double>(active_player->current_pts_us()) / 1e6);
    map[flutter::EncodableValue("isPlaying")] =
        flutter::EncodableValue(active_player->is_playing());
    const auto audio_stats = active_player->audio_output_stats();
    map[flutter::EncodableValue("audioAvailable")] =
        flutter::EncodableValue(active_player->renderer().has_audio());
    map[flutter::EncodableValue("audioSampleRate")] =
        flutter::EncodableValue(active_player->renderer().audio_sample_rate());
    map[flutter::EncodableValue("audioChannels")] =
        flutter::EncodableValue(active_player->renderer().audio_channels());
    map[flutter::EncodableValue("activeAudioTrack")] =
        flutter::EncodableValue(active_player->audible_track());
    map[flutter::EncodableValue("audioOutputDeviceInitialized")] =
        flutter::EncodableValue(audio_stats.device_initialized);
    map[flutter::EncodableValue("audioOutputPlaying")] =
        flutter::EncodableValue(audio_stats.playing);
    map[flutter::EncodableValue("audioOutputActiveTrack")] =
        flutter::EncodableValue(audio_stats.active_track);
    map[flutter::EncodableValue("audioOutputSampleRate")] =
        flutter::EncodableValue(audio_stats.output_sample_rate);
    map[flutter::EncodableValue("audioOutputChannels")] =
        flutter::EncodableValue(audio_stats.output_channels);
    map[flutter::EncodableValue("audioOutputRegisteredTrackCount")] =
        flutter::EncodableValue(static_cast<int64_t>(audio_stats.registered_track_count));
    map[flutter::EncodableValue("audioOutputActiveTrackRegistered")] =
        flutter::EncodableValue(audio_stats.active_track_registered);
    map[flutter::EncodableValue("audioOutputQueuedFrames")] =
        flutter::EncodableValue(static_cast<int64_t>(audio_stats.active_track_queued_frames));
    map[flutter::EncodableValue("audioOutputQueuedDurationUs")] =
        flutter::EncodableValue(static_cast<int64_t>(audio_stats.active_track_queued_duration_us));
    map[flutter::EncodableValue("audioOutputUnderrunFrames")] =
        flutter::EncodableValue(static_cast<int64_t>(audio_stats.active_track_underrun_frames));
    map[flutter::EncodableValue("audioOutputDiscardedFrames")] =
        flutter::EncodableValue(static_cast<int64_t>(audio_stats.active_track_discarded_frames));
    map[flutter::EncodableValue("audioOutputSeekTrimmedFrames")] =
        flutter::EncodableValue(static_cast<int64_t>(audio_stats.active_track_seek_trimmed_frames));

    flutter::EncodableList tracks_list;
    for (const auto& ts : active_player->track_perf_stats()) {
        flutter::EncodableMap tm;
        tm[flutter::EncodableValue("slot")] = flutter::EncodableValue(ts.slot);
        tm[flutter::EncodableValue("fileId")] = flutter::EncodableValue(ts.file_id);
        tm[flutter::EncodableValue("fps")] = flutter::EncodableValue(ts.fps);
        tm[flutter::EncodableValue("avgDecodeMs")] =
            flutter::EncodableValue(ts.avg_decode_ms);
        tm[flutter::EncodableValue("maxDecodeMs")] =
            flutter::EncodableValue(ts.max_decode_ms);
        tm[flutter::EncodableValue("bufferCount")] =
            flutter::EncodableValue(static_cast<int>(ts.buffer_count));
        tm[flutter::EncodableValue("bufferCapacity")] =
            flutter::EncodableValue(static_cast<int>(ts.buffer_capacity));
        tm[flutter::EncodableValue("bufferState")] =
            flutter::EncodableValue(static_cast<int>(ts.buffer_state));
        tracks_list.push_back(flutter::EncodableValue(tm));
    }
    map[flutter::EncodableValue("tracks")] = flutter::EncodableValue(tracks_list);
    map[flutter::EncodableValue("gpuBreakdown")] =
        flutter::EncodableValue(make_gpu_breakdown_map(active_player->gpu_memory_stats()));
    return map;
}

void NativeDiagnosticsProvider::FillFfiDiagnostics(
    NakiVrDiagnostics& out,
    const std::shared_ptr<vr::NativePlayer>& active_player) const {
    std::memset(&out, 0, sizeof(out));
    const auto process_memory = QueryProcessMemoryUsage();
    out.process_working_set_bytes = process_memory.working_set_bytes;
    out.process_private_bytes = process_memory.private_bytes;
    out.dedicated_video_memory_bytes = QueryDedicatedVideoMemoryUsage();

    if (!active_player) {
        return;
    }

    out.d3d_device_lost = active_player->d3d_device_lost() ? 1 : 0;
    out.d3d_device_removed_reason =
        static_cast<int64_t>(active_player->d3d_device_removed_reason());
    out.playback_time_s = static_cast<double>(active_player->current_pts_us()) / 1e6;
    out.is_playing = active_player->is_playing() ? 1 : 0;

    const auto memory_stats = active_player->gpu_memory_stats();
    out.cpu_frame_memory_bytes = memory_stats.cpu_frame_bytes;
    out.packet_queue_memory_bytes = memory_stats.packet_queue_bytes;
    auto stats = active_player->track_perf_stats();
    out.track_count = static_cast<int32_t>(stats.size());
    for (int i = 0; i < kMaxTracksFFI && i < static_cast<int>(stats.size()); ++i) {
        const auto& s = stats[i];
        out.tracks[i].slot = s.slot;
        out.tracks[i].file_id = s.file_id;
        out.tracks[i].fps = s.fps;
        out.tracks[i].avg_decode_ms = s.avg_decode_ms;
        out.tracks[i].max_decode_ms = s.max_decode_ms;
        out.tracks[i].buffer_count = static_cast<int32_t>(s.buffer_count);
        out.tracks[i].buffer_capacity = static_cast<int32_t>(s.buffer_capacity);
        out.tracks[i].buffer_state = static_cast<int32_t>(s.buffer_state);
        for (const auto& m : memory_stats.tracks) {
            if (m.slot == s.slot && m.file_id == s.file_id) {
                out.tracks[i].cpu_frame_memory_bytes = m.total_cpu_frame_bytes;
                out.tracks[i].packet_queue_memory_bytes = m.packet_queue_bytes;
                break;
            }
        }
        out.tracks[i].current_pts_us = s.current_pts_us;
        out.tracks[i].current_dts_us = s.current_dts_us;
    }
    for (int i = static_cast<int>(stats.size()); i < kMaxTracksFFI; ++i) {
        out.tracks[i].slot = -1;
    }
}
