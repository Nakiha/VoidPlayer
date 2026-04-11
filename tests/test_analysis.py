"""Tests for VBS2, VBI, and VBT binary format parsers.

Run: python -m pytest tests/test_analysis.py -v

These tests validate the binary output files produced by:
  python dev.py vtm analyze resources/video/h266_10s_1920x1080.mp4
"""
import struct
import pytest
from pathlib import Path
from collections import Counter

# Test video paths
VIDEO_DIR = Path(__file__).parent.parent / "resources" / "video"
TEST_VIDEO = VIDEO_DIR / "h266_10s_1920x1080.mp4"
VBS2_FILE = VIDEO_DIR / "h266_10s_1920x1080.vbs2"
VBI_FILE = VIDEO_DIR / "h266_10s_1920x1080.vbi"
VBT_FILE = VIDEO_DIR / "h266_10s_1920x1080.vbt"

# Mark all tests as requiring the test video files
pytestmark = pytest.mark.skipif(
    not VBS2_FILE.exists() or not VBI_FILE.exists() or not VBT_FILE.exists(),
    reason="Analysis files not found. Run: python dev.py vtm analyze resources/video/h266_10s_1920x1080.mp4"
)


# ==========================================================================
# VBS2 tests
# ==========================================================================

def read_vbs2_header(path):
    """Read VBS2 file header."""
    with open(path, "rb") as f:
        magic = f.read(4)
        width, height = struct.unpack("<HH", f.read(4))
        num_frames, index_offset = struct.unpack("<II", f.read(8))
    return magic, width, height, num_frames, index_offset


def read_vbs2_index(path, num_frames, index_offset):
    """Read VBS2 frame index."""
    entries = []
    with open(path, "rb") as f:
        f.seek(index_offset)
        for _ in range(num_frames):
            off, nc = struct.unpack("<II", f.read(8))
            entries.append((off, nc))
    return entries


def read_vbs2_frame(path, offset):
    """Read a single Vbs2FrameHeader at given offset."""
    with open(path, "rb") as f:
        f.seek(offset)
        raw = f.read(134)
    poc, num_cus = struct.unpack("<ii", raw[0:8])
    tid, stype, nal_type, avg_qp, nL0, nL1 = struct.unpack("<BBBBBB", raw[8:14])
    ref_l0 = list(struct.unpack("<15i", raw[14:74]))
    ref_l1 = list(struct.unpack("<15i", raw[74:134]))
    return {
        "poc": poc, "num_cus": num_cus, "temporal_id": tid,
        "slice_type": stype, "nal_unit_type": nal_type, "avg_qp": avg_qp,
        "num_ref_l0": nL0, "num_ref_l1": nL1,
        "ref_pocs_l0": ref_l0[:nL0], "ref_pocs_l1": ref_l1[:nL1],
    }


class TestVBS2:
    def test_header_magic(self):
        magic, w, h, nf, idx = read_vbs2_header(VBS2_FILE)
        assert magic == b"VBS2"

    def test_header_dimensions(self):
        magic, w, h, nf, idx = read_vbs2_header(VBS2_FILE)
        assert w == 1920
        assert h == 1080

    def test_frame_count_reasonable(self):
        """Should have at least 100 frames for a 10s video."""
        _, _, _, nf, _ = read_vbs2_header(VBS2_FILE)
        assert nf >= 100

    def test_index_entries_valid(self):
        """All index offsets should point to valid frame headers."""
        _, _, _, nf, idx_off = read_vbs2_header(VBS2_FILE)
        entries = read_vbs2_index(VBS2_FILE, nf, idx_off)
        file_size = VBS2_FILE.stat().st_size
        for i, (off, nc) in enumerate(entries):
            assert off >= 16, f"Entry {i}: offset {off} before header"
            assert off + 134 + nc * 9 <= file_size + nc * 13, \
                f"Entry {i}: offset+data exceeds file"

    def test_first_frame_is_idr(self):
        """First frame should be an I-slice (IDR)."""
        _, _, _, nf, _ = read_vbs2_header(VBS2_FILE)
        entries = read_vbs2_index(VBS2_FILE, nf, _)
        frame = read_vbs2_frame(VBS2_FILE, entries[0][0])
        assert frame["slice_type"] == 2, "First frame should be I-slice"
        assert frame["num_ref_l0"] == 0, "I-frame should have 0 L0 refs"
        assert frame["num_ref_l1"] == 0, "I-frame should have 0 L1 refs"

    def test_inter_frames_have_refs(self):
        """B/P frames should reference valid POCs."""
        _, _, _, nf, _ = read_vbs2_header(VBS2_FILE)
        entries = read_vbs2_index(VBS2_FILE, nf, _)
        inter_with_refs = 0
        for off, nc in entries[1:50]:  # check first 50 non-IDR frames
            frame = read_vbs2_frame(VBS2_FILE, off)
            if frame["slice_type"] != 2:  # not I-slice
                if frame["num_ref_l0"] > 0 or frame["num_ref_l1"] > 0:
                    inter_with_refs += 1
        assert inter_with_refs > 0, "Some inter frames should have references"

    def test_avg_qp_reasonable(self):
        """Average QP should be in 0-63 range (VVC supports QP 0-63)."""
        _, _, _, nf, _ = read_vbs2_header(VBS2_FILE)
        entries = read_vbs2_index(VBS2_FILE, nf, _)
        for off, nc in entries[:20]:
            frame = read_vbs2_frame(VBS2_FILE, off)
            assert 0 <= frame["avg_qp"] <= 63, f"QP {frame['avg_qp']} out of range"

    def test_temporal_id_range(self):
        """Temporal ID should be 0-6 for reasonable encodes."""
        _, _, _, nf, _ = read_vbs2_header(VBS2_FILE)
        entries = read_vbs2_index(VBS2_FILE, nf, _)
        for off, nc in entries[:30]:
            frame = read_vbs2_frame(VBS2_FILE, off)
            assert 0 <= frame["temporal_id"] <= 6, \
                f"TId {frame['temporal_id']} out of range for POC {frame['poc']}"


# ==========================================================================
# VBI tests
# ==========================================================================

def read_vbi_header(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        num_nalus, source_size, reserved = struct.unpack("<III", f.read(12))
    return magic, num_nalus, source_size


def read_vbi_entries(path, num_nalus):
    entries = []
    with open(path, "rb") as f:
        f.seek(16)
        for _ in range(num_nalus):
            raw = f.read(16)
            offset, size = struct.unpack("<QI", raw[0:12])
            nal_type, tid, layer_id, flags = struct.unpack("<BBBB", raw[12:16])
            entries.append({
                "offset": offset, "size": size, "nal_type": nal_type,
                "temporal_id": tid, "layer_id": layer_id, "flags": flags,
            })
    return entries


class TestVBI:
    def test_header_magic(self):
        magic, _, _ = read_vbi_header(VBI_FILE)
        assert magic == b"VBI1"

    def test_nalu_count_reasonable(self):
        """Should have at least as many NALUs as video frames."""
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        assert num_nalus >= 600  # 10s * 60fps

    def test_offsets_strictly_increasing(self):
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        entries = read_vbi_entries(VBI_FILE, num_nalus)
        for i in range(1, len(entries)):
            assert entries[i]["offset"] > entries[i - 1]["offset"], \
                f"Entry {i} offset not > entry {i-1}"

    def test_sizes_positive(self):
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        entries = read_vbi_entries(VBI_FILE, num_nalus)
        for i, e in enumerate(entries):
            assert e["size"] > 0, f"Entry {i} has zero size"

    def test_first_nalu_is_parameter_set(self):
        """First NALU should be SPS, VPS, or PPS."""
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        entries = read_vbi_entries(VBI_FILE, num_nalus)
        assert entries[0]["nal_type"] in {14, 15, 16}, \
            f"First NALU type {entries[0]['nal_type']} is not a parameter set"

    def test_vcl_count_matches_frames(self):
        """Number of VCL NALUs should match video frame count (600)."""
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        entries = read_vbi_entries(VBI_FILE, num_nalus)
        vcl_count = sum(1 for e in entries if e["flags"] & 0x01)
        assert vcl_count == 600, f"Expected 600 VCL NALUs, got {vcl_count}"

    def test_keyframe_count(self):
        """Should have keyframe NALUs."""
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        entries = read_vbi_entries(VBI_FILE, num_nalus)
        kf_count = sum(1 for e in entries if e["flags"] & 0x04)
        assert kf_count >= 1, "Should have at least one keyframe"

    def test_start_code_at_offset(self):
        """Verify start code bytes exist at reported offsets."""
        _, num_nalus, _ = read_vbi_header(VBI_FILE)
        entries = read_vbi_entries(VBI_FILE, num_nalus)
        raw_data = open(VBI_FILE.with_suffix(".vvc"), "rb").read()
        for i, e in enumerate(entries[:10]):
            off = e["offset"]
            assert raw_data[off:off + 3] == b"\x00\x00\x01" or \
                   raw_data[off:off + 4] == b"\x00\x00\x00\x01", \
                f"Entry {i}: no start code at offset {off}"


# ==========================================================================
# VBT tests
# ==========================================================================

def read_vbt_header(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        num_pkts = struct.unpack("<I", f.read(4))[0]
        tb_num = struct.unpack("<i", f.read(4))[0]
        tb_den = struct.unpack("<i", f.read(4))[0]
    return magic, num_pkts, tb_num, tb_den


def read_vbt_entries(path, num_pkts):
    entries = []
    with open(path, "rb") as f:
        f.seek(32)
        for _ in range(num_pkts):
            raw = f.read(32)
            pts, dts = struct.unpack("<qq", raw[0:16])
            poc, size, dur = struct.unpack("<iII", raw[16:28])
            flags = raw[28]
            entries.append({
                "pts": pts, "dts": dts, "poc": poc,
                "size": size, "duration": dur, "flags": flags,
            })
    return entries


class TestVBT:
    def test_header_magic(self):
        magic, _, _, _ = read_vbt_header(VBT_FILE)
        assert magic == b"VBT1"

    def test_packet_count(self):
        """Should have exactly 600 packets for 10s@60fps."""
        _, num_pkts, _, _ = read_vbt_header(VBT_FILE)
        assert num_pkts == 600

    def test_time_base(self):
        _, _, tb_num, tb_den = read_vbt_header(VBT_FILE)
        assert tb_num == 1
        assert tb_den == 60

    def test_first_packet_is_keyframe(self):
        _, num_pkts, _, tb_den = read_vbt_header(VBT_FILE)
        entries = read_vbt_entries(VBT_FILE, num_pkts)
        assert entries[0]["flags"] & 0x01, "First packet should be keyframe"

    def test_first_packet_pts_zero(self):
        _, num_pkts, _, tb_den = read_vbt_header(VBT_FILE)
        entries = read_vbt_entries(VBT_FILE, num_pkts)
        assert entries[0]["pts"] == 0, "First packet PTS should be 0"

    def test_sizes_positive(self):
        _, num_pkts, _, _ = read_vbt_header(VBT_FILE)
        entries = read_vbt_entries(VBT_FILE, num_pkts)
        for i, e in enumerate(entries):
            assert e["size"] > 0, f"Packet {i} has zero size"

    def test_keyframe_count(self):
        _, num_pkts, _, _ = read_vbt_header(VBT_FILE)
        entries = read_vbt_entries(VBT_FILE, num_pkts)
        kf_count = sum(1 for e in entries if e["flags"] & 0x01)
        assert kf_count == 10, f"Expected 10 keyframes, got {kf_count}"

    def test_total_duration(self):
        """Total duration should be approximately 10 seconds."""
        _, num_pkts, _, tb_den = read_vbt_header(VBT_FILE)
        entries = read_vbt_entries(VBT_FILE, num_pkts)
        # Last packet PTS + duration should be ~10s
        last_pts_time = entries[-1]["pts"] / tb_den
        assert 9.0 <= last_pts_time <= 10.5, \
            f"Last PTS {last_pts_time:.2f}s outside expected range"
