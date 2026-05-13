#pragma once
// Packed binary structs matching on-disk analysis cache layouts.

#include <cstddef>
#include <cstdint>

#pragma pack(push, 1)

// ===========================================================================
// VACHUNK overlay records
// ===========================================================================

struct VachunkFrameSummary {
    int32_t  poc;
    uint32_t coded_order;
    uint32_t vcl_unit_index;
    uint32_t flags;
    uint8_t  temporal_id;
    uint8_t  slice_type;     // 0=B, 1=P, 2=I
    uint8_t  nal_unit_type;
    uint8_t  avg_qp;
    uint8_t  num_ref_l0;     // 0-15
    uint8_t  num_ref_l1;
    uint8_t  qp_min;
    uint8_t  qp_max;
    int32_t  ref_pocs_l0[15];
    int32_t  ref_pocs_l1[15];
    uint32_t num_cus;
    uint32_t cu_index_entry;
    uint32_t reserved[2];
};
static_assert(sizeof(VachunkFrameSummary) == 160);

struct VachunkCuCommon {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    uint8_t  depth;
    uint8_t  qp;
    uint8_t  pred_mode;      // 0=inter, 1=intra, 2=ibc, 3=plt
    uint32_t bit_count;      // coded syntax bits attributed to this CU/MB.
};
static_assert(sizeof(VachunkCuCommon) == 13);

struct VachunkCuIntra {
    uint8_t intra_mode;
    uint8_t mip_flag;
    uint8_t isp_mode;
};
static_assert(sizeof(VachunkCuIntra) == 3);

struct VachunkCuInter {
    uint8_t  skip;
    uint8_t  merge_flag;
    uint8_t  inter_dir;
    int16_t  mv_l0_x;
    int16_t  mv_l0_y;
    int16_t  mv_l1_x;
    int16_t  mv_l1_y;
    int8_t   ref_l0;
    int8_t   ref_l1;
};
static_assert(sizeof(VachunkCuInter) == 13);

// CU record size by prediction mode
inline constexpr size_t VACHUNK_CU_SIZE_INTER = sizeof(VachunkCuCommon) + sizeof(VachunkCuInter);  // 26
inline constexpr size_t VACHUNK_CU_SIZE_INTRA = sizeof(VachunkCuCommon) + sizeof(VachunkCuIntra);  // 16

// ===========================================================================
// VAC2 scanner records
// ===========================================================================

enum class AnalysisCodec : uint16_t {
    Unknown = 0,
    H264    = 1,
    HEVC    = 2,
    VVC     = 3,
    AV1     = 4,
    VP9     = 5,
    MPEG2   = 6,
};

enum class AnalysisUnitKind : uint16_t {
    Unknown   = 0,
    Nalu      = 1,
    Obu       = 2,
    StartCode = 3,
    Packet    = 4,
};

struct AnalysisUnitScanEntry {
    uint64_t offset;         // byte offset of start code in source file
    uint32_t size;           // bytes from start code to next start code
    uint8_t  nal_type;       // codec-specific unit type
    uint8_t  temporal_id;
    uint8_t  layer_id;
    uint8_t  flags;          // bit0: isVCL/coded unit, bit1: isSlice, bit2: isKeyframe
};
static_assert(sizeof(AnalysisUnitScanEntry) == 16);

// Scanner unit flags
inline constexpr uint8_t ANALYSIS_UNIT_FLAG_IS_VCL      = 0x01;
inline constexpr uint8_t ANALYSIS_UNIT_FLAG_IS_SLICE    = 0x02;
inline constexpr uint8_t ANALYSIS_UNIT_FLAG_IS_KEYFRAME = 0x04;

struct AnalysisPacketScanEntry {
    int64_t  pts;
    int64_t  dts;
    int32_t  poc;            // decode order index
    uint32_t size;           // packet bytes
    uint32_t duration;       // in time_base units
    uint8_t  flags;          // bit0: keyframe
    uint8_t  reserved[3];
};
static_assert(sizeof(AnalysisPacketScanEntry) == 32);

// Scanner packet flags
inline constexpr uint8_t ANALYSIS_PACKET_FLAG_KEYFRAME = 0x01;

// ===========================================================================
// VAC2 — progressive analysis base index
// ===========================================================================

inline constexpr uint16_t kVac2VersionMajor = 2;
inline constexpr uint16_t kVac2VersionMinor = 0;

struct Vac2Header {
    char     magic[4];       // "VAC2"
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint32_t section_count;
    uint32_t flags;
    uint16_t codec;          // AnalysisCodec
    uint16_t track_index;
    int32_t  time_base_num;
    int32_t  time_base_den;
    uint32_t packet_count;
    uint32_t unit_count;
    uint32_t au_count;
    uint32_t width;
    uint32_t height;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t source_size;
    int64_t  source_mtime_unix_ms;
    uint64_t content_revision;
    uint64_t reserved[4];
};
static_assert(sizeof(Vac2Header) == 124);

struct Vac2SectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;     // 0 for variable payloads
    uint32_t entry_count;    // 0 for variable payloads
    uint64_t checksum;
    uint64_t reserved[2];
};
static_assert(sizeof(Vac2SectionEntry) == 56);

struct Vac2PacketEntry {
    int64_t  pts;
    int64_t  dts;
    uint32_t duration;
    uint32_t size;
    uint16_t stream_index;
    uint16_t flags;
    uint64_t file_offset;    // UINT64_MAX when unknown
    uint64_t format_offset;  // UINT64_MAX when unknown
    uint32_t first_unit;
    uint32_t unit_count;
    uint32_t au_index;       // UINT32_MAX when unknown
    uint32_t reserved;
};
static_assert(sizeof(Vac2PacketEntry) == 60);

struct Vac2BitstreamUnitEntry {
    uint32_t packet_index;   // UINT32_MAX when unknown
    uint32_t au_index;       // UINT32_MAX when unknown
    uint64_t offset;
    uint32_t size;
    uint32_t payload_offset;
    uint8_t  nal_type;
    uint8_t  temporal_id;
    uint8_t  layer_id;
    uint8_t  unit_kind;      // AnalysisUnitKind
    uint16_t flags;
    uint16_t pset_snapshot;  // UINT16_MAX when unknown
    uint32_t detail_hint;
};
static_assert(sizeof(Vac2BitstreamUnitEntry) == 36);

struct Vac2FrameEntry {
    uint32_t first_packet;
    uint32_t packet_count;
    uint32_t first_unit;
    uint32_t unit_count;
    int64_t  pts;
    int64_t  dts;
    uint32_t duration;
    uint32_t coded_order;
    int32_t  display_order;
    int32_t  poc;
    uint32_t frame_size;
    uint32_t rap_distance;
    uint32_t flags;
};
static_assert(sizeof(Vac2FrameEntry) == 60);

struct Vac2FrameSummaryEntry {
    int32_t  poc;
    uint32_t coded_order;
    uint32_t first_vcl_unit;
    uint32_t flags;
    uint8_t  temporal_id;
    uint8_t  slice_type;     // 0=B, 1=P, 2=I, 255=unknown
    uint8_t  nal_type;
    uint8_t  qp_kind;        // 0=unknown, 1=slice, 2=base, 3=estimated, 4=exact
    uint8_t  qp_avg;
    uint8_t  qp_min;
    uint8_t  qp_max;
    uint8_t  num_ref_l0;
    uint8_t  num_ref_l1;
    uint8_t  reserved0[3];
    int32_t  ref_pocs_l0[15];
    int32_t  ref_pocs_l1[15];
    uint64_t summary_chunk_id;
    uint64_t reserved1[2];
};
static_assert(sizeof(Vac2FrameSummaryEntry) == 172);

inline constexpr uint16_t VAC2_PACKET_FLAG_KEYFRAME = 0x0001;
inline constexpr uint16_t VAC2_UNIT_FLAG_IS_VCL = 0x0001;
inline constexpr uint16_t VAC2_UNIT_FLAG_IS_SLICE = 0x0002;
inline constexpr uint16_t VAC2_UNIT_FLAG_IS_KEYFRAME = 0x0004;
inline constexpr uint16_t VAC2_UNIT_FLAG_PARAMETER_SET = 0x0008;
inline constexpr uint32_t VAC2_FRAME_FLAG_KEYFRAME = 0x00000001;
inline constexpr uint32_t VAC2_FRAME_FLAG_RAP = 0x00000002;
inline constexpr uint32_t VAC2_FRAME_SUMMARY_FLAG_EXACT_REFS = 0x00000001;
inline constexpr uint32_t VAC2_FRAME_SUMMARY_FLAG_EXACT_QP = 0x00000002;

inline constexpr uint8_t VAC2_QP_KIND_UNKNOWN = 0;
inline constexpr uint8_t VAC2_QP_KIND_SLICE = 1;
inline constexpr uint8_t VAC2_QP_KIND_BASE = 2;
inline constexpr uint8_t VAC2_QP_KIND_ESTIMATED = 3;
inline constexpr uint8_t VAC2_QP_KIND_EXACT = 4;

// ===========================================================================
// VACHUNK — progressive analysis derived chunk
// ===========================================================================

inline constexpr uint16_t kVachunkVersionMajor = 1;
inline constexpr uint16_t kVachunkVersionMinor = 0;

enum class VachunkKind : uint16_t {
    Unknown = 0,
    NaluDetail = 1,
    FrameSummaryExact = 2,
    Overlay = 3,
    HitTest = 4,
    Export = 5,
};

struct VachunkHeader {
    char     magic[4];       // "VCK1"
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint32_t section_count;
    uint16_t kind;           // VachunkKind
    uint16_t codec;          // AnalysisCodec
    uint64_t feature_flags;
    uint64_t base_content_revision;
    uint64_t generator_revision;
    uint16_t track_index;
    uint16_t reserved0;
    uint32_t start_frame;    // UINT32_MAX when not frame-scoped
    uint32_t end_frame;      // inclusive, UINT32_MAX when not frame-scoped
    uint32_t start_packet;
    uint32_t end_packet;
    uint32_t start_unit;
    uint32_t end_unit;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t checksum;
    uint64_t reserved1[4];
};
static_assert(sizeof(VachunkHeader) == 128);

struct VachunkSectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;     // 0 for variable payloads
    uint32_t entry_count;    // 0 for variable payloads
    uint64_t decoded_size;   // decoded size if compressed, else size
    uint64_t checksum;
    uint64_t reserved;
};
static_assert(sizeof(VachunkSectionEntry) == 56);

struct VachunkOverlayFrameIndexEntry {
    uint32_t frame_index;
    uint32_t first_unit;
    uint32_t unit_count;
    uint32_t first_stream_value;
    uint32_t flags;
    uint32_t reserved;
};
static_assert(sizeof(VachunkOverlayFrameIndexEntry) == 24);

inline constexpr uint32_t VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE = 0x00000001;
inline constexpr uint32_t VACHUNK_OVERLAY_FRAME_FLAG_EXACT = 0x00000002;

inline constexpr uint64_t VACHUNK_FEATURE_CU_GEOMETRY = 1ull << 0;
inline constexpr uint64_t VACHUNK_FEATURE_QP = 1ull << 1;
inline constexpr uint64_t VACHUNK_FEATURE_PRED_MODE = 1ull << 2;
inline constexpr uint64_t VACHUNK_FEATURE_MOTION_VECTORS = 1ull << 3;
inline constexpr uint64_t VACHUNK_FEATURE_REF_INDEXES = 1ull << 4;
inline constexpr uint64_t VACHUNK_FEATURE_PU_GEOMETRY = 1ull << 5;
inline constexpr uint64_t VACHUNK_FEATURE_TU_GEOMETRY = 1ull << 6;
inline constexpr uint64_t VACHUNK_FEATURE_CODEC_TOOLS = 1ull << 7;
inline constexpr uint64_t VACHUNK_FEATURE_BIT_COST = 1ull << 8;

#pragma pack(pop)
