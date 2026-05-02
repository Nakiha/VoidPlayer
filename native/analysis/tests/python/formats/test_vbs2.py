from .helpers import read_vbs2_frame, read_vbs2_header, read_vbs2_index


def test_vbs2_header_magic(analysis_paths):
    magic, _, _, _, _ = read_vbs2_header(analysis_paths["vbs2"])
    assert magic == b"VBS2"


def test_vbs2_header_dimensions(analysis_paths):
    _, width, height, _, _ = read_vbs2_header(analysis_paths["vbs2"])
    assert width == 1920
    assert height == 1080


def test_vbs2_frame_count_reasonable(analysis_paths):
    _, _, _, frame_count, _ = read_vbs2_header(analysis_paths["vbs2"])
    assert frame_count >= 100


def test_vbs2_index_entries_valid(analysis_paths):
    _, _, _, frame_count, index_offset = read_vbs2_header(analysis_paths["vbs2"])
    entries = read_vbs2_index(analysis_paths["vbs2"], frame_count, index_offset)
    file_size = analysis_paths["vbs2"].stat().st_size
    for i, (offset, cu_count) in enumerate(entries):
        assert offset >= 16, f"Entry {i}: offset {offset} before header"
        assert offset + 134 + cu_count * 9 <= file_size + cu_count * 13, (
            f"Entry {i}: offset+data exceeds file"
        )


def test_vbs2_first_frame_is_idr(analysis_paths):
    _, _, _, frame_count, index_offset = read_vbs2_header(analysis_paths["vbs2"])
    entries = read_vbs2_index(analysis_paths["vbs2"], frame_count, index_offset)
    frame = read_vbs2_frame(analysis_paths["vbs2"], entries[0][0])
    assert frame["slice_type"] == 2
    assert frame["num_ref_l0"] == 0
    assert frame["num_ref_l1"] == 0


def test_vbs2_inter_frames_have_refs(analysis_paths):
    _, _, _, frame_count, index_offset = read_vbs2_header(analysis_paths["vbs2"])
    entries = read_vbs2_index(analysis_paths["vbs2"], frame_count, index_offset)
    inter_with_refs = 0
    for offset, _ in entries[1:50]:
        frame = read_vbs2_frame(analysis_paths["vbs2"], offset)
        if frame["slice_type"] != 2:
            if frame["num_ref_l0"] > 0 or frame["num_ref_l1"] > 0:
                inter_with_refs += 1
    assert inter_with_refs > 0


def test_vbs2_avg_qp_reasonable(analysis_paths):
    _, _, _, frame_count, index_offset = read_vbs2_header(analysis_paths["vbs2"])
    entries = read_vbs2_index(analysis_paths["vbs2"], frame_count, index_offset)
    for offset, _ in entries[:20]:
        frame = read_vbs2_frame(analysis_paths["vbs2"], offset)
        assert 0 <= frame["avg_qp"] <= 63


def test_vbs2_temporal_id_range(analysis_paths):
    _, _, _, frame_count, index_offset = read_vbs2_header(analysis_paths["vbs2"])
    entries = read_vbs2_index(analysis_paths["vbs2"], frame_count, index_offset)
    for offset, _ in entries[:30]:
        frame = read_vbs2_frame(analysis_paths["vbs2"], offset)
        assert 0 <= frame["temporal_id"] <= 6
