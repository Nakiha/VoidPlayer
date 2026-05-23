#pragma once

#include <cstdint>

#if defined(_WIN32)
#define NAKI_ANALYSIS_FFI_EXPORT __declspec(dllexport)
#else
#define NAKI_ANALYSIS_FFI_EXPORT __attribute__((visibility("default")))
#endif

static constexpr int32_t NAKI_ANALYSIS_ABI_VERSION = 1;

enum NakiAnalysisStatus {
    NAKI_ANALYSIS_OK = 0,
    NAKI_ANALYSIS_ERR_INVALID_ARGUMENT = 1,
    NAKI_ANALYSIS_ERR_OPEN_FAILED = 2,
    NAKI_ANALYSIS_ERR_CLOSED = 3,
    NAKI_ANALYSIS_ERR_UNSUPPORTED = 4,
    NAKI_ANALYSIS_ERR_INTERNAL = 1000,
};

struct NakiAnalysisStructHeader {
    uint32_t size;
    uint32_t abi_version;
};

struct NakiAnalysisSummary {
    int32_t loaded;
    int32_t frame_count;
    int32_t packet_count;
    int32_t nalu_count;
    int32_t video_width;
    int32_t video_height;
    int32_t time_base_num;
    int32_t time_base_den;
    int32_t current_frame_idx;
    int32_t codec;
    int32_t _reserved[6];
};

struct NakiFrameInfo {
    int32_t poc;
    int32_t temporal_id;
    int32_t slice_type;
    int32_t nal_type;
    int32_t avg_qp;
    int32_t num_ref_l0;
    int32_t num_ref_l1;
    int32_t ref_pocs_l0[15];
    int32_t ref_pocs_l1[15];
    int64_t pts;
    int64_t dts;
    int32_t packet_size;
    int32_t keyframe;
    int32_t _reserved[2];
};

struct NakiNaluInfo {
    uint64_t offset;
    uint32_t size;
    uint8_t nal_type;
    uint8_t temporal_id;
    uint8_t layer_id;
    uint8_t flags;
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
using NakiAnalysisPtsCallback = int64_t (*)();

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_abi_version();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_last_error(char* buf, int32_t cap);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_summary();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_info();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_nalu_info();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_bucket();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_overlay_state();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_summary_v2();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_info_v2();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_nalu_info_v2();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_bucket_v2();
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_overlay_state_v2();

extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_set_overlay(const NakiOverlayState* state);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_set_overlay_track(int32_t track_file_id, const char* analysis_path);
extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_clear_overlay_tracks();

extern "C" NAKI_ANALYSIS_FFI_EXPORT NakiAnalysisHandle naki_analysis_open(const char* analysis_path);
extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_close(NakiAnalysisHandle handle);
extern "C" NAKI_ANALYSIS_FFI_EXPORT const NakiAnalysisSummary* naki_analysis_handle_get_summary(NakiAnalysisHandle handle);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_frame_index_for_timestamp(NakiAnalysisHandle handle, int64_t pts_us, int64_t dts_us);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_frames(NakiAnalysisHandle handle, NakiFrameInfo* out, int32_t max_count);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_frames_range(NakiAnalysisHandle handle, int32_t start, NakiFrameInfo* out, int32_t max_count);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_nalus(NakiAnalysisHandle handle, NakiNaluInfo* out, int32_t max_count);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_nalus_range(NakiAnalysisHandle handle, int32_t start, NakiNaluInfo* out, int32_t max_count);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_frame_to_nalu(NakiAnalysisHandle handle, int32_t frame_index);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_nalu_to_frame(NakiAnalysisHandle handle, int32_t nalu_index);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_frame_buckets(NakiAnalysisHandle handle, int32_t start, int32_t bucket_size, NakiFrameBucket* out, int32_t max_count);

void naki_analysis_register_pts_callback(NakiAnalysisPtsCallback cb);
void naki_analysis_register_pts_callback_for_owner(const void* owner, NakiAnalysisPtsCallback cb);
void naki_analysis_clear_pts_callback_for_owner(const void* owner);

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_generate_vac2_base(const char* video_path, const char* hash, const char* cache_root, int64_t max_cache_bytes);
extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_generate_vac2_overlay_chunk(const char* video_path, const char* hash, const char* cache_root, int32_t start_frame, int32_t end_frame, int64_t max_cache_bytes);
