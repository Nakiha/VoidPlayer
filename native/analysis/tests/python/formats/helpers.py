"""Binary readers used by analysis format tests."""

import struct


def read_vbs2_header(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        width, height = struct.unpack("<HH", f.read(4))
        num_frames, index_offset = struct.unpack("<II", f.read(8))
    return magic, width, height, num_frames, index_offset


def read_vbs2_index(path, num_frames, index_offset):
    entries = []
    with open(path, "rb") as f:
        f.seek(index_offset)
        for _ in range(num_frames):
            off, nc = struct.unpack("<II", f.read(8))
            entries.append((off, nc))
    return entries


def read_vbs2_frame(path, offset):
    with open(path, "rb") as f:
        f.seek(offset)
        raw = f.read(134)
    poc, num_cus = struct.unpack("<ii", raw[0:8])
    tid, stype, nal_type, avg_qp, n_l0, n_l1 = struct.unpack("<BBBBBB", raw[8:14])
    ref_l0 = list(struct.unpack("<15i", raw[14:74]))
    ref_l1 = list(struct.unpack("<15i", raw[74:134]))
    return {
        "poc": poc,
        "num_cus": num_cus,
        "temporal_id": tid,
        "slice_type": stype,
        "nal_unit_type": nal_type,
        "avg_qp": avg_qp,
        "num_ref_l0": n_l0,
        "num_ref_l1": n_l1,
        "ref_pocs_l0": ref_l0[:n_l0],
        "ref_pocs_l1": ref_l1[:n_l1],
    }


def read_vbs3_header(path):
    with open(path, "rb") as f:
        raw = f.read(64)
    fields = struct.unpack("<4sHHHHIIIIIQQQQ", raw)
    return {
        "magic": fields[0],
        "version_major": fields[1],
        "version_minor": fields[2],
        "header_size": fields[3],
        "section_entry_size": fields[4],
        "flags": fields[5],
        "width": fields[6],
        "height": fields[7],
        "frame_count": fields[8],
        "section_count": fields[9],
        "section_table_offset": fields[10],
        "file_size": fields[11],
        "content_revision": fields[12],
        "reserved": fields[13],
    }


def read_vbs3_sections(path, header):
    sections = {}
    with open(path, "rb") as f:
        f.seek(header["section_table_offset"])
        for _ in range(header["section_count"]):
            raw = f.read(header["section_entry_size"])
            fields = struct.unpack("<4sIQQIIQQ", raw)
            name = fields[0].decode("ascii")
            sections[name] = {
                "type": fields[0],
                "flags": fields[1],
                "offset": fields[2],
                "size": fields[3],
                "entry_size": fields[4],
                "entry_count": fields[5],
                "checksum": fields[6],
                "reserved": fields[7],
            }
    return sections


def read_vbs3_frame_summaries(path, section, limit=None):
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


def read_vbs3_cu_index(path, section, limit=None):
    count = section["entry_count"] if limit is None else min(limit, section["entry_count"])
    entries = []
    with open(path, "rb") as f:
        f.seek(section["offset"])
        for _ in range(count):
            raw = f.read(section["entry_size"])
            offset, byte_size, cu_count, flags = struct.unpack("<QQII", raw)
            entries.append({
                "offset": offset,
                "byte_size": byte_size,
                "cu_count": cu_count,
                "flags": flags,
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
