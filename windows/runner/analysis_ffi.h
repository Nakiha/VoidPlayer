#pragma once

#include <cstdint>

/// ---- dart:ffi flat structs for bitstream analysis ----

struct NakiAnalysisSummary {
    int32_t loaded;             // 0 or 1
    int32_t frame_count;        // VBS2
    int32_t packet_count;       // VBT
    int32_t nalu_count;         // VBI
    int32_t video_width;
    int32_t video_height;
    int32_t time_base_num;
    int32_t time_base_den;
    int32_t current_frame_idx;  // derived from current PTS via VBT
    int32_t _reserved[7];
};

// Merged frame info: VBS2 frame header + VBT packet data
struct NakiFrameInfo {
    int32_t poc;
    int32_t temporal_id;
    int32_t slice_type;         // 0=B, 1=P, 2=I
    int32_t nal_type;
    int32_t avg_qp;
    int32_t num_ref_l0;
    int32_t num_ref_l1;
    int32_t ref_pocs_l0[15];
    int32_t ref_pocs_l1[15];
    int64_t pts;
    int64_t dts;
    int32_t packet_size;
    int32_t keyframe;           // 0 or 1
    int32_t _reserved[2];
};

struct NakiNaluInfo {
    uint64_t offset;
    uint32_t size;
    uint8_t  nal_type;
    uint8_t  temporal_id;
    uint8_t  layer_id;
    uint8_t  flags;             // bit0: VCL, bit1: Slice, bit2: Keyframe
};

struct NakiOverlayState {
    int32_t show_cu_grid;
    int32_t show_pred_mode;
    int32_t show_qp_heatmap;
    int32_t _reserved;
};

extern "C" __declspec(dllexport)
int32_t naki_analysis_load(const char* vbs2_path, const char* vbi_path, const char* vbt_path);

extern "C" __declspec(dllexport)
void naki_analysis_unload();

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_get_summary();

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames(NakiFrameInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus(NakiNaluInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
void naki_analysis_set_overlay(const NakiOverlayState* state);

// Register a callback that returns the current playback PTS in microseconds.
// Called by video_renderer_plugin during initialization.
void naki_analysis_register_pts_callback(int64_t (*cb)());
