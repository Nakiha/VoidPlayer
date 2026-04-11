"""VVC NALU Indexer — generates .vbi binary index from raw Annex B bitstreams.

Usage:
  python tools/vvc_nalu_indexer.py input.vvc [-o output.vbi]

The output .vbi file contains:
  - 16-byte header (magic, num_nalus, source_size, reserved)
  - Fixed-size entries (16B each): offset, size, nal_type, temporal_id, layer_id, flags

Algorithm: scan for Annex B start codes (00 00 01 or 00 00 00 01), then read the
2-byte VVC NALU header to extract type, TId, and layer ID.
"""
import argparse
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# VVC NALU type names (from H.266/VVC spec Table 7-1)
# ---------------------------------------------------------------------------
VVC_NALU_TYPES = {
    0: "TRAIL",
    1: "STSA",
    2: "RADL",
    3: "RASL",
    4: "RESERVED_VCL_4",
    5: "RESERVED_VCL_5",
    6: "RESERVED_VCL_6",
    7: "IDR_W_RADL",
    8: "IDR_N_LP",
    9: "CRA",
    10: "GDR",
    11: "RESERVED_IRAP_VCL_11",
    12: "OPI",
    13: "DCI",
    14: "VPS",
    15: "SPS",
    16: "PPS",
    17: "APS_PREFIX",
    18: "APS_SUFFIX",
    19: "PH",
    20: "AUD",
    21: "EOS",
    22: "EOB",
    23: "SEI_PREFIX",
    24: "SEI_SUFFIX",
    25: "FD",
    26: "RESERVED_NVCL_26",
    27: "RESERVED_NVCL_27",
    28: "UNSPECIFIED_28",
    29: "UNSPECIFIED_29",
    30: "UNSPECIFIED_30",
    31: "UNSPECIFIED_31",
}

# VCL NALU types (contain coded slice data)
VCL_NALU_TYPES = set(range(0, 12))  # 0-11 are VCL

# Slice NALU types (contain actual coded video data)
SLICE_NALU_TYPES = {0, 1, 2, 3, 7, 8, 9, 10}

# Keyframe NALU types (IDR or CRA)
KEYFRAME_NALU_TYPES = {7, 8, 9}  # IDR_W_RADL, IDR_N_LP, CRA

# ---------------------------------------------------------------------------
# Binary format constants
# ---------------------------------------------------------------------------
VBI_MAGIC = b"VBI1"
VBI_HEADER_SIZE = 16  # magic(4) + num_nalus(4) + source_size(4) + reserved(4)
VBI_ENTRY_SIZE = 16   # offset(8) + size(4) + nal_type(1) + tid(1) + layer_id(1) + flags(1)

# Flags bitmask
FLAG_IS_VCL = 0x01
FLAG_IS_SLICE = 0x02
FLAG_IS_KEYFRAME = 0x04

# ---------------------------------------------------------------------------
# Annex B start code scanning
# ---------------------------------------------------------------------------
START_CODE_3 = b"\x00\x00\x01"
START_CODE_4 = b"\x00\x00\x00\x01"


def _parse_nalu_header(header_bytes: bytes) -> tuple:
    """Parse 2-byte VVC NALU header.

    Returns (nal_type, temporal_id, nuh_layer_id).
    VVC NALU header layout (16 bits):
      forbidden_zero_bit    (1)  = header_bytes[0] >> 7
      nuh_reserved_zero_bit (1)  = (header_bytes[0] >> 6) & 1
      nuh_layer_id          (6)  = header_bytes[0] & 0x3F
      nal_unit_type         (5)  = (header_bytes[1] >> 3) & 0x1F
      nuh_temporal_id_plus1 (3)  = header_bytes[1] & 0x07
    """
    if len(header_bytes) < 2:
        return (31, 0, 0)
    b0, b1 = header_bytes[0], header_bytes[1]
    nuh_layer_id = b0 & 0x3F
    nal_type = (b1 >> 3) & 0x1F
    tid_plus1 = b1 & 0x07
    temporal_id = max(0, tid_plus1 - 1)  # TId = nuh_temporal_id_plus1 - 1 (0 for non-VCL)
    return (nal_type, temporal_id, nuh_layer_id)


def scan_nalus(data: bytes) -> list:
    """Scan raw Annex B bytestream and extract NALU entries.

    Returns list of (offset, size, nal_type, temporal_id, nuh_layer_id, flags).
    """
    entries = []
    data_len = len(data)
    i = 0

    # Positions of start codes
    start_positions = []

    while i < data_len - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                start_positions.append(i)
                i += 3
                continue
            elif i < data_len - 4 and data[i + 2] == 0 and data[i + 3] == 1:
                start_positions.append(i)
                i += 4
                continue
        i += 1

    # Build NALU entries from consecutive start code positions
    for idx, start_pos in enumerate(start_positions):
        # Determine start code length by checking bytes at position
        if start_pos + 4 <= data_len and data[start_pos:start_pos + 4] == START_CODE_4:
            sc_len = 4  # 00 00 00 01
        else:
            sc_len = 3  # 00 00 01

        # NALU data starts after start code
        nalu_data_offset = start_pos + sc_len

        # Size: from start code start to next start code start (or EOF)
        if idx + 1 < len(start_positions):
            size = start_positions[idx + 1] - start_pos
        else:
            size = data_len - start_pos

        # Parse NALU header (2 bytes after start code)
        if nalu_data_offset + 2 <= data_len:
            nal_type, temporal_id, nuh_layer_id = _parse_nalu_header(
                data[nalu_data_offset:nalu_data_offset + 2]
            )
        else:
            nal_type, temporal_id, nuh_layer_id = 31, 0, 0  # truncated

        # Compute flags
        flags = 0
        if nal_type in VCL_NALU_TYPES:
            flags |= FLAG_IS_VCL
        if nal_type in SLICE_NALU_TYPES:
            flags |= FLAG_IS_SLICE
        if nal_type in KEYFRAME_NALU_TYPES:
            flags |= FLAG_IS_KEYFRAME

        entries.append((start_pos, size, nal_type, temporal_id, nuh_layer_id, flags))

    return entries


def write_vbi(entries: list, source_size: int, output_path: str):
    """Write .vbi binary index file."""
    with open(output_path, "wb") as f:
        # Header
        f.write(VBI_MAGIC)
        f.write(struct.pack("<I", len(entries)))
        f.write(struct.pack("<I", source_size))
        f.write(struct.pack("<I", 0))  # reserved

        # Entries
        for offset, size, nal_type, tid, layer_id, flags in entries:
            f.write(struct.pack("<Q", offset))
            f.write(struct.pack("<I", size))
            f.write(struct.pack("<BBBB", nal_type, tid, layer_id, flags))


def index_nalus(input_path: str, output_path: str = None, verbose: bool = False):
    """Index NALUs in a raw Annex B VVC bitstream and write .vbi file.

    Args:
        input_path: Path to raw .vvc Annex B bitstream.
        output_path: Path to output .vbi file. Auto-derived if None.
        verbose: Print summary to stdout.
    """
    input_path = Path(input_path)
    if output_path is None:
        output_path = input_path.with_suffix(".vbi")

    if not input_path.exists():
        print(f"ERROR: input not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    if verbose:
        print(f"Scanning {input_path} ({input_path.stat().st_size / 1024 / 1024:.1f} MB)...")

    data = input_path.read_bytes()
    entries = scan_nalus(data)
    write_vbi(entries, len(data), str(output_path))

    if verbose:
        print(f"Found {len(entries)} NALUs → {output_path}")
        # Print first 10 NALUs
        for i, (off, sz, nt, tid, lid, fl) in enumerate(entries[:10]):
            name = VVC_NALU_TYPES.get(nt, f"UNKNOWN({nt})")
            flags_str = []
            if fl & FLAG_IS_VCL: flags_str.append("VCL")
            if fl & FLAG_IS_SLICE: flags_str.append("SLICE")
            if fl & FLAG_IS_KEYFRAME: flags_str.append("KEY")
            print(f"  [{i:4d}] @{off:8d} size={sz:6d} {name:16s} TId={tid} Layer={lid} {','.join(flags_str)}")
        if len(entries) > 10:
            print(f"  ... ({len(entries) - 10} more)")

    return str(output_path)


def main():
    parser = argparse.ArgumentParser(
        description="VVC NALU Indexer — generate .vbi from raw Annex B bitstream",
    )
    parser.add_argument("input", help="Raw Annex B VVC bitstream (.vvc)")
    parser.add_argument("-o", "--output", help="Output .vbi file (auto-derived if omitted)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print summary")
    args = parser.parse_args()

    index_nalus(args.input, args.output, verbose=True)


if __name__ == "__main__":
    main()
