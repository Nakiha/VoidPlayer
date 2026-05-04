from .helpers import (
    read_vbs4_block_index,
    read_vbs4_frame_summaries,
    read_vbs4_header,
    read_vbs4_sections,
)


def test_vbs4_header(analysis_paths):
    header = read_vbs4_header(analysis_paths["vbs4"])
    assert header["magic"] == b"VBS4"
    assert header["version_major"] == 4
    assert header["header_size"] == 80
    assert header["section_entry_size"] == 56
    assert header["codec"] == 3
    assert header["profile"] == 1
    assert header["width"] == 1920
    assert header["height"] == 1080
    assert header["frame_count"] > 0
    assert header["block_count"] > 0
    assert header["file_size"] == analysis_paths["vbs4"].stat().st_size


def test_vbs4_sections(analysis_paths):
    header = read_vbs4_header(analysis_paths["vbs4"])
    sections = read_vbs4_sections(analysis_paths["vbs4"], header)
    assert {"FSUM", "FIDX", "BIDX", "CPAY"}.issubset(sections.keys())
    file_size = analysis_paths["vbs4"].stat().st_size
    for section in sections.values():
        assert section["offset"] + section["size"] <= file_size


def test_vbs4_frame_summaries(analysis_paths):
    header = read_vbs4_header(analysis_paths["vbs4"])
    sections = read_vbs4_sections(analysis_paths["vbs4"], header)
    summaries = read_vbs4_frame_summaries(analysis_paths["vbs4"], sections["FSUM"], 30)
    assert len(summaries) == min(30, header["frame_count"])
    assert summaries[0]["slice_type"] == 2
    assert summaries[0]["num_cus"] > 0
    for summary in summaries:
        assert summary["temporal_id"] <= 6
        assert summary["avg_qp"] <= 63
        assert summary["num_ref_l0"] <= 15
        assert summary["num_ref_l1"] <= 15


def test_vbs4_block_index_bounds(analysis_paths):
    header = read_vbs4_header(analysis_paths["vbs4"])
    sections = read_vbs4_sections(analysis_paths["vbs4"], header)
    blocks = read_vbs4_block_index(analysis_paths["vbs4"], sections["BIDX"])
    cpay = sections["CPAY"]
    assert len(blocks) == header["block_count"]
    for block in blocks:
        assert block["payload_offset"] + block["payload_size"] <= cpay["size"]
        assert block["decoded_size"] >= block["payload_size"]
        assert block["compression"] in (0, 1)
