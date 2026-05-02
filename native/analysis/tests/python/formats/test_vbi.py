from .helpers import read_vbi_entries, read_vbi_header


def test_vbi_header_magic(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    assert header["magic"] == b"VBI2"
    assert header["version"] == 2
    assert header["codec"] == 3
    assert header["unit_kind"] == 1


def test_vbi_nalu_count_reasonable(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    assert header["num_units"] >= 600


def test_vbi_offsets_strictly_increasing(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    entries = read_vbi_entries(analysis_paths["vbi"], header)
    for i in range(1, len(entries)):
        assert entries[i]["offset"] > entries[i - 1]["offset"]


def test_vbi_sizes_positive(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    entries = read_vbi_entries(analysis_paths["vbi"], header)
    for i, entry in enumerate(entries):
        assert entry["size"] > 0, f"Entry {i} has zero size"


def test_vbi_first_nalu_type_is_valid(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    entries = read_vbi_entries(analysis_paths["vbi"], header)
    assert 0 <= entries[0]["nal_type"] <= 31


def test_vbi_vcl_count_matches_frames(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    entries = read_vbi_entries(analysis_paths["vbi"], header)
    vcl_count = sum(1 for entry in entries if entry["flags"] & 0x01)
    assert vcl_count == 600


def test_vbi_keyframe_count(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    entries = read_vbi_entries(analysis_paths["vbi"], header)
    keyframe_count = sum(1 for entry in entries if entry["flags"] & 0x04)
    assert keyframe_count >= 1


def test_vbi_virtual_offsets_cover_source(analysis_paths):
    header = read_vbi_header(analysis_paths["vbi"])
    entries = read_vbi_entries(analysis_paths["vbi"], header)
    last = entries[-1]
    assert last["offset"] + last["size"] == header["source_size"]
