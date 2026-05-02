#pragma once
// Packed binary structs matching on-disk layout for VBS3/VBI/VBT files.
// These mirror the Python tools (vvc_nalu_indexer.py, vvc_timestamp_extractor.py)
// and VTM C++ (dtrace_blockstatistics.cpp) definitions exactly.

#include <cstdint>

#pragma pack(push, 1)

// ===========================================================================
// VBS3 — VTM block statistics (sectioned frame summaries + CU records)
// ===========================================================================

struct Vbs3Header {
    char     magic[4];       // "VBS3"
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
    uint32_t section_count;
    uint64_t section_table_offset;
    uint64_t file_size;
    uint64_t content_revision;
    uint64_t reserved;
};
static_assert(sizeof(Vbs3Header) == 64);

struct Vbs3SectionEntry {
    char     type[4];
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint64_t checksum;
    uint64_t reserved;
};
static_assert(sizeof(Vbs3SectionEntry) == 48);

struct Vbs3FrameSummary {
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
static_assert(sizeof(Vbs3FrameSummary) == 160);

struct Vbs3CuIndexEntry {
    uint64_t offset;         // relative to CUBL payload
    uint64_t byte_size;
    uint32_t cu_count;
    uint32_t flags;
};
static_assert(sizeof(Vbs3CuIndexEntry) == 24);

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
