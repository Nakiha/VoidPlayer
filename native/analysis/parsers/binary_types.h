#pragma once
// Packed binary structs matching on-disk layout for VAC/VBS4/VBI/VBT files.

#include <cstddef>
#include <cstdint>

#pragma pack(push, 1)

// ===========================================================================
// VAC1 — Void analysis container
// ===========================================================================

inline constexpr uint16_t kAnalysisContainerVersion = 1;

struct AnalysisContainerHeader {
    char     magic[4];       // "VAC1"
    uint16_t version;        // kAnalysisContainerVersion
    uint16_t header_size;    // sizeof(AnalysisContainerHeader)
    uint16_t section_entry_size;
    uint16_t section_count;
    uint32_t flags;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t reserved[4];
};
static_assert(sizeof(AnalysisContainerHeader) == 64);

struct AnalysisContainerSectionEntry {
    char     type[4];        // "VBS4", "VBI2", "VBT1"
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint64_t checksum;
    uint64_t reserved[2];
};
static_assert(sizeof(AnalysisContainerSectionEntry) == 48);

// ===========================================================================
// VBS4 — decoder-derived block statistics (summaries + compressed blocks)
// ===========================================================================

struct Vbs4Header {
    char     magic[4];       // "VBS4"
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint16_t codec;          // VbiCodec
    uint16_t profile;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint32_t block_count;
    uint32_t section_count;
    uint32_t reserved0;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t content_revision;
    uint64_t reserved1;
    uint32_t reserved2;
};
static_assert(sizeof(Vbs4Header) == 80);

struct Vbs4SectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint64_t checksum;
    uint64_t reserved0;
    uint64_t reserved1;
};
static_assert(sizeof(Vbs4SectionEntry) == 56);

struct Vbs4FrameSummary {
    int32_t  poc;
    uint32_t coded_order;
    uint32_t vcl_nalu_index;
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
static_assert(sizeof(Vbs4FrameSummary) == 160);

struct Vbs4FrameIndexEntry {
    uint32_t block_index;
    uint32_t local_frame;
    uint32_t first_record;
    uint32_t record_count;
    uint32_t flags;
    uint32_t reserved;
};
static_assert(sizeof(Vbs4FrameIndexEntry) == 24);

struct Vbs4BlockIndexEntry {
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t first_record;
    uint32_t record_count;
    uint64_t payload_offset;
    uint64_t payload_size;
    uint64_t decoded_size;
    uint16_t codec_profile;
    uint16_t compression;    // 0=none, 1=zstd
    uint32_t flags;
    uint64_t checksum;
    uint64_t reserved;
};
static_assert(sizeof(Vbs4BlockIndexEntry) == 64);

struct Vbs4DecodedBlockHeader {
    char     magic[4];       // "BLK4"
    uint16_t header_size;
    uint16_t stream_entry_size;
    uint16_t codec_profile;
    uint16_t stream_count;
    uint32_t frame_count;
    uint32_t record_count;
    uint32_t flags;
    uint64_t reserved;
};
static_assert(sizeof(Vbs4DecodedBlockHeader) == 32);

struct Vbs4StreamEntry {
    uint16_t stream_id;
    uint16_t encoding;       // 0=raw, 1=bitset, 2=uleb128, 3=sleb128 zigzag, 7=frame prefix u32
    uint32_t offset;
    uint32_t size;
    uint32_t value_count;
    uint32_t flags;
};
static_assert(sizeof(Vbs4StreamEntry) == 20);

inline constexpr uint16_t VBS4_COMPRESSION_NONE = 0;
inline constexpr uint16_t VBS4_COMPRESSION_ZSTD = 1;

inline constexpr uint16_t VBS4_ENCODING_RAW = 0;
inline constexpr uint16_t VBS4_ENCODING_BITSET = 1;
inline constexpr uint16_t VBS4_ENCODING_ULEB128 = 2;
inline constexpr uint16_t VBS4_ENCODING_SLEB128_ZIGZAG = 3;
inline constexpr uint16_t VBS4_ENCODING_FRAME_PREFIX_U32 = 7;

inline constexpr uint16_t VBS4_STREAM_FRAME_PREFIX = 1;

inline constexpr uint16_t VBS4_HEVC_X = 2;
inline constexpr uint16_t VBS4_HEVC_Y = 3;
inline constexpr uint16_t VBS4_HEVC_LOG2_W = 4;
inline constexpr uint16_t VBS4_HEVC_LOG2_H = 5;
inline constexpr uint16_t VBS4_HEVC_DEPTH = 6;
inline constexpr uint16_t VBS4_HEVC_PRED_MODE = 7;
inline constexpr uint16_t VBS4_HEVC_QP_DELTA = 8;
inline constexpr uint16_t VBS4_HEVC_INTRA_MODE = 9;
inline constexpr uint16_t VBS4_HEVC_MIP_FLAG = 10;
inline constexpr uint16_t VBS4_HEVC_ISP_MODE = 11;
inline constexpr uint16_t VBS4_HEVC_SKIP_FLAG = 12;
inline constexpr uint16_t VBS4_HEVC_MERGE_FLAG = 13;
inline constexpr uint16_t VBS4_HEVC_INTER_DIR = 14;
inline constexpr uint16_t VBS4_HEVC_MV_L0_X = 15;
inline constexpr uint16_t VBS4_HEVC_MV_L0_Y = 16;
inline constexpr uint16_t VBS4_HEVC_MV_L1_X = 17;
inline constexpr uint16_t VBS4_HEVC_MV_L1_Y = 18;
inline constexpr uint16_t VBS4_HEVC_REF_L0 = 19;
inline constexpr uint16_t VBS4_HEVC_REF_L1 = 20;

inline constexpr uint16_t VBS4_H264_IS_INTRA = 2;
inline constexpr uint16_t VBS4_H264_SKIP_FLAG = 3;
inline constexpr uint16_t VBS4_H264_MERGE_FLAG = 4;
inline constexpr uint16_t VBS4_H264_INTER_DIR = 5;
inline constexpr uint16_t VBS4_H264_QP_DELTA = 6;
inline constexpr uint16_t VBS4_H264_INTRA_MODE = 7;
inline constexpr uint16_t VBS4_H264_REF_L0 = 8;
inline constexpr uint16_t VBS4_H264_REF_L1 = 9;
inline constexpr uint16_t VBS4_H264_MV_L0_X = 10;
inline constexpr uint16_t VBS4_H264_MV_L0_Y = 11;
inline constexpr uint16_t VBS4_H264_MV_L1_X = 12;
inline constexpr uint16_t VBS4_H264_MV_L1_Y = 13;

struct VbsCuCommon {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    uint8_t  depth;
    uint8_t  qp;
    uint8_t  pred_mode;      // 0=inter, 1=intra, 2=ibc, 3=plt
};
static_assert(sizeof(VbsCuCommon) == 9);

struct VbsCuIntra {
    uint8_t intra_mode;
    uint8_t mip_flag;
    uint8_t isp_mode;
};
static_assert(sizeof(VbsCuIntra) == 3);

struct VbsCuInter {
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
static_assert(sizeof(VbsCuInter) == 13);

// CU record size by prediction mode
inline constexpr size_t VBS_CU_SIZE_INTER = sizeof(VbsCuCommon) + sizeof(VbsCuInter);  // 22
inline constexpr size_t VBS_CU_SIZE_INTRA = sizeof(VbsCuCommon) + sizeof(VbsCuIntra);  // 12

// ===========================================================================
// VBI — bitstream unit index
// ===========================================================================

enum class VbiCodec : uint16_t {
    Unknown = 0,
    H264    = 1,
    HEVC    = 2,
    VVC     = 3,
    AV1     = 4,
    VP9     = 5,
    MPEG2   = 6,
};

enum class VbiUnitKind : uint16_t {
    Unknown   = 0,
    Nalu      = 1,
    Obu       = 2,
    StartCode = 3,
    Packet    = 4,
};

struct VbiLegacyHeader {
    char     magic[4];       // "VBI1"
    uint32_t num_nalus;
    uint32_t source_size;
    uint32_t reserved;
};
static_assert(sizeof(VbiLegacyHeader) == 16);

struct VbiHeader {
    char     magic[4];       // "VBI2"
    uint16_t version;        // 2
    uint16_t codec;          // VbiCodec
    uint16_t unit_kind;      // VbiUnitKind
    uint16_t header_size;    // sizeof(VbiHeader)
    uint32_t num_units;
    uint64_t source_size;
    uint8_t  reserved[32];
};
static_assert(sizeof(VbiHeader) == 56);

struct VbiEntry {
    uint64_t offset;         // byte offset of start code in source file
    uint32_t size;           // bytes from start code to next start code
    uint8_t  nal_type;       // codec-specific unit type (kept for ABI compatibility)
    uint8_t  temporal_id;
    uint8_t  layer_id;
    uint8_t  flags;          // bit0: isVCL/coded unit, bit1: isSlice, bit2: isKeyframe
};
static_assert(sizeof(VbiEntry) == 16);

// VBI flags
inline constexpr uint8_t VBI_FLAG_IS_VCL      = 0x01;
inline constexpr uint8_t VBI_FLAG_IS_SLICE    = 0x02;
inline constexpr uint8_t VBI_FLAG_IS_KEYFRAME = 0x04;

// ===========================================================================
// VBT — Timestamps
// ===========================================================================

struct VbtHeader {
    char     magic[4];       // "VBT1"
    uint32_t num_packets;
    int32_t  time_base_num;
    int32_t  time_base_den;
    uint8_t  reserved[16];
};
static_assert(sizeof(VbtHeader) == 32);

struct VbtEntry {
    int64_t  pts;
    int64_t  dts;
    int32_t  poc;            // decode order index
    uint32_t size;           // packet bytes
    uint32_t duration;       // in time_base units
    uint8_t  flags;          // bit0: keyframe
    uint8_t  reserved[3];
};
static_assert(sizeof(VbtEntry) == 32);

// VBT flags
inline constexpr uint8_t VBT_FLAG_KEYFRAME = 0x01;

#pragma pack(pop)
