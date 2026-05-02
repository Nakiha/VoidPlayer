from .helpers import read_vbt_entries, read_vbt_header


def test_vbt_header_magic(analysis_paths):
    magic, _, _, _ = read_vbt_header(analysis_paths["vbt"])
    assert magic == b"VBT1"


def test_vbt_packet_count(analysis_paths):
    _, packet_count, _, _ = read_vbt_header(analysis_paths["vbt"])
    assert packet_count == 600


def test_vbt_time_base(analysis_paths):
    _, _, tb_num, tb_den = read_vbt_header(analysis_paths["vbt"])
    assert tb_num == 1
    assert tb_den == 60


def test_vbt_first_packet_is_keyframe(analysis_paths):
    _, packet_count, _, _ = read_vbt_header(analysis_paths["vbt"])
    entries = read_vbt_entries(analysis_paths["vbt"], packet_count)
    assert entries[0]["flags"] & 0x01


def test_vbt_first_packet_pts_zero(analysis_paths):
    _, packet_count, _, _ = read_vbt_header(analysis_paths["vbt"])
    entries = read_vbt_entries(analysis_paths["vbt"], packet_count)
    assert entries[0]["pts"] == 0


def test_vbt_sizes_positive(analysis_paths):
    _, packet_count, _, _ = read_vbt_header(analysis_paths["vbt"])
    entries = read_vbt_entries(analysis_paths["vbt"], packet_count)
    for i, entry in enumerate(entries):
        assert entry["size"] > 0, f"Packet {i} has zero size"


def test_vbt_keyframe_count(analysis_paths):
    _, packet_count, _, _ = read_vbt_header(analysis_paths["vbt"])
    entries = read_vbt_entries(analysis_paths["vbt"], packet_count)
    keyframe_count = sum(1 for entry in entries if entry["flags"] & 0x01)
    assert keyframe_count == 10


def test_vbt_total_duration(analysis_paths):
    _, packet_count, _, tb_den = read_vbt_header(analysis_paths["vbt"])
    entries = read_vbt_entries(analysis_paths["vbt"], packet_count)
    last_pts_time = entries[-1]["pts"] / tb_den
    assert 9.0 <= last_pts_time <= 10.5
