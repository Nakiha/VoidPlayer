#pragma once
// Packed binary structs matching on-disk layout for VBS2/VBI/VBT files.
// These mirror the Python tools (vvc_nalu_indexer.py, vvc_timestamp_extractor.py)
// and VTM C++ (dtrace_blockstatistics.cpp) definitions exactly.

#include <cstdint>

#pragma pack(push, 1)

// ===========================================================================
// VBS2 — VTM block statistics (frame headers + CU records + index)
// ===========================================================================

struct Vbs2Header {
    char     magic[4];       // "VBS2"
    uint16_t width;
    uint16_t height;
    uint32_t num_frames;
    uint32_t index_offset;
};
static_assert(sizeof(Vbs2Header) == 16);

struct Vbs2FrameHeader {
    int32_t  poc;            // -1 = sentinel
    int32_t  num_cus;
    uint8_t  temporal_id;
    uint8_t  slice_type;     // 0=B, 1=P, 2=I
    uint8_t  nal_unit_type;
    uint8_t  avg_qp;
    uint8_t  num_ref_l0;     // 0-15
    uint8_t  num_ref_l1;
    int32_t  ref_pocs_l0[15];
    int32_t  ref_pocs_l1[15];
};
static_assert(sizeof(Vbs2FrameHeader) == 134);

struct Vbs2CuCommon {
    uint16_t x;
    uint16_t y;
    uint8_t  w;
    uint8_t  h;
    uint8_t  depth;
    uint8_t  qp;
    uint8_t  pred_mode;      // 0=inter, 1=intra, 2=ibc, 3=plt
};
static_assert(sizeof(Vbs2CuCommon) == 9);

struct Vbs2CuIntra {
    uint8_t intra_mode;
    uint8_t mip_flag;
    uint8_t isp_mode;
};
static_assert(sizeof(Vbs2CuIntra) == 3);

struct Vbs2CuInter {
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
static_assert(sizeof(Vbs2CuInter) == 13);

struct Vbs2IndexEntry {
    uint32_t offset;         // file offset of Vbs2FrameHeader
    uint32_t num_cus;
};
static_assert(sizeof(Vbs2IndexEntry) == 8);

// CU record size by prediction mode
inline constexpr size_t VBS2_CU_SIZE_INTER = sizeof(Vbs2CuCommon) + sizeof(Vbs2CuInter);  // 22
inline constexpr size_t VBS2_CU_SIZE_INTRA = sizeof(Vbs2CuCommon) + sizeof(Vbs2CuIntra);  // 12

// ===========================================================================
// VBI — NALU index
// ===========================================================================

struct VbiHeader {
    char     magic[4];       // "VBI1"
    uint32_t num_nalus;
    uint32_t source_size;
    uint32_t reserved;
};
static_assert(sizeof(VbiHeader) == 16);

struct VbiEntry {
    uint64_t offset;         // byte offset of start code in source file
    uint32_t size;           // bytes from start code to next start code
    uint8_t  nal_type;       // 5-bit NalUnitType
    uint8_t  temporal_id;
    uint8_t  layer_id;
    uint8_t  flags;          // bit0: isVCL, bit1: isSlice, bit2: isKeyframe
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
