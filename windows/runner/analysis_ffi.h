#pragma once

#include <cstdint>

/// ---- dart:ffi flat structs for bitstream analysis ----

static constexpr int32_t NAKI_ANALYSIS_ABI_VERSION = 1;

enum NakiAnalysisStatus {
    NAKI_ANALYSIS_OK = 0,
    NAKI_ANALYSIS_ERR_INVALID_ARGUMENT = 1,
    NAKI_ANALYSIS_ERR_OPEN_FAILED = 2,
    NAKI_ANALYSIS_ERR_CLOSED = 3,
    NAKI_ANALYSIS_ERR_INTERNAL = 1000,
};

struct NakiAnalysisStructHeader {
    uint32_t size;
    uint32_t abi_version;
};

struct NakiAnalysisSummary {
    int32_t loaded;             // 0 or 1
    int32_t frame_count;        // VBS4 when present
    int32_t packet_count;       // VBT
    int32_t nalu_count;         // VBI
    int32_t video_width;
    int32_t video_height;
    int32_t time_base_num;
    int32_t time_base_den;
    int32_t current_frame_idx;  // derived from current PTS via VBT
    int32_t codec;              // VbiCodec
    int32_t _reserved[6];
};

// Merged frame info: VBS4 frame summary + VBT packet data
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

struct NakiFrameBucket {
    int32_t start_frame;
    int32_t frame_count;
    int32_t packet_size_min;
    int32_t packet_size_max;
    int64_t packet_size_sum;
    int32_t qp_min;
    int32_t qp_max;
    int64_t qp_sum;
    int32_t keyframe_count;
    int32_t _reserved[3];
};

struct NakiOverlayState {
    int32_t show_cu_grid;
    int32_t show_pred_mode;
    int32_t show_qp_heatmap;
    int32_t show_pred_lines;
    int32_t show_cu_bit_cost_heatmap;
    int32_t opacity_permille;
    int32_t mode;
    int32_t track_file_id;
    int32_t _reserved;
};

struct NakiAnalysisSummaryV2 {
    NakiAnalysisStructHeader header;
    NakiAnalysisSummary value;
};

struct NakiFrameInfoV2 {
    NakiAnalysisStructHeader header;
    NakiFrameInfo value;
};

struct NakiNaluInfoV2 {
    NakiAnalysisStructHeader header;
    NakiNaluInfo value;
};

struct NakiFrameBucketV2 {
    NakiAnalysisStructHeader header;
    NakiFrameBucket value;
};

struct NakiOverlayStateV2 {
    NakiAnalysisStructHeader header;
    NakiOverlayState value;
};

using NakiAnalysisHandle = void*;

extern "C" __declspec(dllexport)
int32_t naki_analysis_abi_version();

extern "C" __declspec(dllexport)
int32_t naki_analysis_last_error(char* buf, int32_t cap);

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_summary();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_frame_info();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_nalu_info();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_frame_bucket();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_overlay_state();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_summary_v2();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_frame_info_v2();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_nalu_info_v2();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_frame_bucket_v2();

extern "C" __declspec(dllexport)
int32_t naki_analysis_sizeof_overlay_state_v2();

extern "C" __declspec(dllexport)
int32_t naki_analysis_load(const char* analysis_path);

extern "C" __declspec(dllexport)
void naki_analysis_unload();

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_get_summary();

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames(NakiFrameInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames_range(int32_t start, NakiFrameInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus(NakiNaluInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus_range(int32_t start, NakiNaluInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_frame_to_nalu(int32_t frame_index);

extern "C" __declspec(dllexport)
int32_t naki_analysis_nalu_to_frame(int32_t nalu_index);

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frame_buckets(int32_t start, int32_t bucket_size, NakiFrameBucket* out, int32_t max_count);

extern "C" __declspec(dllexport)
void naki_analysis_set_overlay(const NakiOverlayState* state);

extern "C" __declspec(dllexport)
int32_t naki_analysis_set_overlay_track(int32_t track_file_id, const char* analysis_path);

extern "C" __declspec(dllexport)
void naki_analysis_clear_overlay_tracks();

extern "C" __declspec(dllexport)
NakiAnalysisHandle naki_analysis_open(const char* analysis_path);

extern "C" __declspec(dllexport)
void naki_analysis_close(NakiAnalysisHandle handle);

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_handle_get_summary(NakiAnalysisHandle handle);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames(NakiAnalysisHandle handle, NakiFrameInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames_range(NakiAnalysisHandle handle, int32_t start, NakiFrameInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus(NakiAnalysisHandle handle, NakiNaluInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus_range(NakiAnalysisHandle handle, int32_t start, NakiNaluInfo* out, int32_t max_count);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_frame_to_nalu(NakiAnalysisHandle handle, int32_t frame_index);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_nalu_to_frame(NakiAnalysisHandle handle, int32_t nalu_index);

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frame_buckets(NakiAnalysisHandle handle, int32_t start, int32_t bucket_size, NakiFrameBucket* out, int32_t max_count);

// Register a callback that returns the current playback PTS in microseconds.
// Called by video_renderer_plugin during initialization.
void naki_analysis_register_pts_callback(int64_t (*cb)());

/// Generate an analysis container for a video file.
/// Writes to <cache_dir>/<hash>.vac.
/// Returns 1 on success, 0 on failure (unsupported codec, tool not found, etc.)
extern "C" __declspec(dllexport)
int32_t naki_analysis_generate(const char* video_path,
                               const char* hash,
                               const char* cache_dir,
                               int64_t max_cache_bytes);

/// Generate a progressive VAC2 base cache for a video file.
/// Writes to <cache_root>/<hash>/base.vac.
/// Returns 1 on success, 0 on failure.
extern "C" __declspec(dllexport)
int32_t naki_analysis_generate_vac2_base(const char* video_path,
                                         const char* hash,
                                         const char* cache_root,
                                         int64_t max_cache_bytes);

/// Generate an overlay VACHUNK for an inclusive frame range.
/// Writes to <cache_root>/<hash>/chunks/overlay/*.vck.
/// Returns 1 on success, 0 on failure.
extern "C" __declspec(dllexport)
int32_t naki_analysis_generate_vac2_overlay_chunk(const char* video_path,
                                                  const char* hash,
                                                  const char* cache_root,
                                                  int32_t start_frame,
                                                  int32_t end_frame,
                                                  int64_t max_cache_bytes);
