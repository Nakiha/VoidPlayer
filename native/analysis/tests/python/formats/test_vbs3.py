from .helpers import (
    read_vbs2_frame,
    read_vbs2_header,
    read_vbs2_index,
    read_vbs3_cu_index,
    read_vbs3_frame_summaries,
    read_vbs3_header,
    read_vbs3_sections,
)


def test_vbs3_header(analysis_paths):
    header = read_vbs3_header(analysis_paths["vbs3"])
    assert header["magic"] == b"VBS3"
    assert header["version_major"] == 3
    assert header["version_minor"] == 0
    assert header["header_size"] == 64
    assert header["section_entry_size"] == 48
    assert header["width"] == 1920
    assert header["height"] == 1080
    assert header["frame_count"] >= 100
    assert header["section_count"] == 3
    assert header["file_size"] == analysis_paths["vbs3"].stat().st_size


def test_vbs3_sections(analysis_paths):
    header = read_vbs3_header(analysis_paths["vbs3"])
    sections = read_vbs3_sections(analysis_paths["vbs3"], header)
    assert set(sections) == {"FSUM", "CUID", "CUBL"}

    file_size = analysis_paths["vbs3"].stat().st_size
    for name, section in sections.items():
        assert section["offset"] >= header["header_size"], name
        assert section["offset"] + section["size"] <= file_size, name
        assert section["reserved"] == 0, name

    assert sections["FSUM"]["entry_size"] == 160
    assert sections["FSUM"]["entry_count"] == header["frame_count"]
    assert sections["CUID"]["entry_size"] == 24
    assert sections["CUID"]["entry_count"] == header["frame_count"]
    assert sections["CUBL"]["entry_size"] == 0
    assert sections["CUBL"]["entry_count"] == header["frame_count"]


def test_vbs3_frame_summaries(analysis_paths):
    header = read_vbs3_header(analysis_paths["vbs3"])
    sections = read_vbs3_sections(analysis_paths["vbs3"], header)
    summaries = read_vbs3_frame_summaries(analysis_paths["vbs3"], sections["FSUM"], 30)

    first = summaries[0]
    assert first["coded_order"] == 0
    assert first["slice_type"] == 2
    assert first["num_ref_l0"] == 0
    assert first["num_ref_l1"] == 0

    for i, summary in enumerate(summaries):
        assert summary["coded_order"] == i
        assert summary["cu_index_entry"] == i
        assert summary["num_cus"] > 0
        assert 0 <= summary["avg_qp"] <= 63
        assert 0 <= summary["qp_min"] <= summary["qp_max"] <= 63
        assert 0 <= summary["temporal_id"] <= 6


def test_vbs3_cu_index_bounds(analysis_paths):
    header = read_vbs3_header(analysis_paths["vbs3"])
    sections = read_vbs3_sections(analysis_paths["vbs3"], header)
    summaries = read_vbs3_frame_summaries(analysis_paths["vbs3"], sections["FSUM"], 50)
    cu_index = read_vbs3_cu_index(analysis_paths["vbs3"], sections["CUID"], 50)
    cubl_size = sections["CUBL"]["size"]

    for i, entry in enumerate(cu_index):
        assert entry["offset"] + entry["byte_size"] <= cubl_size
        assert entry["byte_size"] >= entry["cu_count"] * 9
        assert entry["cu_count"] == summaries[i]["num_cus"]
        assert entry["flags"] == 0


def test_vbs3_matches_vbs2_frame_headers(analysis_paths):
    _, _, _, vbs2_count, vbs2_index_offset = read_vbs2_header(analysis_paths["vbs2"])
    vbs2_index = read_vbs2_index(analysis_paths["vbs2"], vbs2_count, vbs2_index_offset)
    header = read_vbs3_header(analysis_paths["vbs3"])
    sections = read_vbs3_sections(analysis_paths["vbs3"], header)
    summaries = read_vbs3_frame_summaries(analysis_paths["vbs3"], sections["FSUM"], 30)

    assert header["frame_count"] == vbs2_count
    for i, summary in enumerate(summaries):
        vbs2_frame = read_vbs2_frame(analysis_paths["vbs2"], vbs2_index[i][0])
        assert summary["poc"] == vbs2_frame["poc"]
        assert summary["num_cus"] == vbs2_frame["num_cus"]
        assert summary["temporal_id"] == vbs2_frame["temporal_id"]
        assert summary["slice_type"] == vbs2_frame["slice_type"]
        assert summary["nal_unit_type"] == vbs2_frame["nal_unit_type"]
        assert summary["avg_qp"] == vbs2_frame["avg_qp"]
        assert summary["num_ref_l0"] == vbs2_frame["num_ref_l0"]
        assert summary["num_ref_l1"] == vbs2_frame["num_ref_l1"]
        assert summary["ref_pocs_l0"] == vbs2_frame["ref_pocs_l0"]
        assert summary["ref_pocs_l1"] == vbs2_frame["ref_pocs_l1"]
