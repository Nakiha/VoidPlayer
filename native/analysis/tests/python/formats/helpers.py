"""Binary readers used by analysis format tests."""

import struct


def read_vbs4_header(path):
    with open(path, "rb") as f:
        raw = f.read(80)
    fields = struct.unpack("<4s6H7I4QI", raw)
    return {
        "magic": fields[0],
        "version_major": fields[1],
        "version_minor": fields[2],
        "header_size": fields[3],
        "section_entry_size": fields[4],
        "codec": fields[5],
        "profile": fields[6],
        "flags": fields[7],
        "width": fields[8],
        "height": fields[9],
        "frame_count": fields[10],
        "block_count": fields[11],
        "section_count": fields[12],
        "reserved0": fields[13],
        "section_table_offset": fields[14],
        "file_size": fields[15],
        "content_revision": fields[16],
        "reserved1": fields[17],
        "reserved2": fields[18],
    }


def read_vbs4_sections(path, header):
    sections = {}
    with open(path, "rb") as f:
        f.seek(header["section_table_offset"])
        for _ in range(header["section_count"]):
            raw = f.read(header["section_entry_size"])
            fields = struct.unpack("<4sIQQIIQQQ", raw)
            name = fields[0].decode("ascii")
            sections[name] = {
                "type": fields[0],
                "flags": fields[1],
                "offset": fields[2],
                "size": fields[3],
                "entry_size": fields[4],
                "entry_count": fields[5],
                "checksum": fields[6],
                "reserved0": fields[7],
                "reserved1": fields[8],
            }
    return sections


def read_vbs4_frame_summaries(path, section, limit=None):
    count = section["entry_count"] if limit is None else min(limit, section["entry_count"])
    entries = []
    with open(path, "rb") as f:
        f.seek(section["offset"])
        for _ in range(count):
            raw = f.read(section["entry_size"])
            fields = struct.unpack("<iIII8B15i15iII2I", raw)
            entries.append({
                "poc": fields[0],
                "coded_order": fields[1],
                "vcl_nalu_index": fields[2],
                "flags": fields[3],
                "temporal_id": fields[4],
                "slice_type": fields[5],
                "nal_unit_type": fields[6],
                "avg_qp": fields[7],
                "num_ref_l0": fields[8],
                "num_ref_l1": fields[9],
                "qp_min": fields[10],
                "qp_max": fields[11],
                "ref_pocs_l0": list(fields[12:27])[:fields[8]],
                "ref_pocs_l1": list(fields[27:42])[:fields[9]],
                "num_cus": fields[42],
                "cu_index_entry": fields[43],
            })
    return entries


def read_vbs4_block_index(path, section, limit=None):
    count = section["entry_count"] if limit is None else min(limit, section["entry_count"])
    entries = []
    with open(path, "rb") as f:
        f.seek(section["offset"])
        for _ in range(count):
            raw = f.read(section["entry_size"])
            fields = struct.unpack("<4I3Q2HIQQ", raw)
            entries.append({
                "first_frame": fields[0],
                "frame_count": fields[1],
                "first_record": fields[2],
                "record_count": fields[3],
                "payload_offset": fields[4],
                "payload_size": fields[5],
                "decoded_size": fields[6],
                "codec_profile": fields[7],
                "compression": fields[8],
                "flags": fields[9],
                "checksum": fields[10],
                "reserved": fields[11],
            })
    return entries


def read_vbi_header(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic == b"VBI1":
            num_units, source_size, _ = struct.unpack("<III", f.read(12))
            return {
                "magic": magic,
                "version": 1,
                "codec": 3,
                "unit_kind": 1,
                "header_size": 16,
                "num_units": num_units,
                "source_size": source_size,
            }
        if magic == b"VBI2":
            version, codec, unit_kind, header_size, num_units, source_size = struct.unpack(
                "<HHHHIQ", f.read(20)
            )
            return {
                "magic": magic,
                "version": version,
                "codec": codec,
                "unit_kind": unit_kind,
                "header_size": header_size,
                "num_units": num_units,
                "source_size": source_size,
            }
    raise AssertionError(f"Unsupported VBI magic: {magic!r}")


def read_vbi_entries(path, header):
    entries = []
    with open(path, "rb") as f:
        f.seek(header["header_size"])
        for _ in range(header["num_units"]):
            raw = f.read(16)
            offset, size = struct.unpack("<QI", raw[0:12])
            nal_type, tid, layer_id, flags = struct.unpack("<BBBB", raw[12:16])
            entries.append({
                "offset": offset,
                "size": size,
                "nal_type": nal_type,
                "temporal_id": tid,
                "layer_id": layer_id,
                "flags": flags,
            })
    return entries


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
                "pts": pts,
                "dts": dts,
                "poc": poc,
                "size": size,
                "duration": dur,
                "flags": flags,
            })
    return entries
