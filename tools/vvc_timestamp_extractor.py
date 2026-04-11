"""VVC Timestamp Extractor — generates .vbt binary index from video containers.

Usage:
  python tools/vvc_timestamp_extractor.py input.mp4 [-o output.vbt]

Uses ffprobe to extract per-packet PTS/DTS/size/keyframe info, then writes
a compact binary .vbt file for efficient seeking and analysis.
"""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Binary format constants
# ---------------------------------------------------------------------------
VBT_MAGIC = b"VBT1"
VBT_HEADER_SIZE = 32   # magic(4) + num_packets(4) + time_base_num(4) + time_base_den(4) + reserved(16)
VBT_ENTRY_SIZE = 32    # pts(8) + dts(8) + poc(4) + size(4) + duration(4) + flags(1) + reserved(3)

# Flags bitmask
FLAG_KEYFRAME = 0x01


def _run_ffprobe(input_path: str) -> dict:
    """Run ffprobe to get stream info and per-packet data."""
    # Get stream info (time_base, codec, etc.)
    cmd_stream = [
        "ffprobe", "-v", "quiet",
        "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,time_base,r_frame_rate,nb_frames",
        "-of", "json",
        str(input_path),
    ]
    result = subprocess.run(cmd_stream, capture_output=True, text=True, check=True)
    stream_info = json.loads(result.stdout)

    # Get per-packet info
    cmd_packets = [
        "ffprobe", "-v", "quiet",
        "-select_streams", "v:0",
        "-show_packets",
        "-show_entries", "packet=pts_time,dts_time,pts,dts,size,duration,flags",
        "-of", "json",
        str(input_path),
    ]
    result = subprocess.run(cmd_packets, capture_output=True, text=True, check=True)
    packet_info = json.loads(result.stdout)

    return {
        "stream": stream_info.get("streams", [{}])[0] if stream_info.get("streams") else {},
        "packets": packet_info.get("packets", []),
    }


def write_vbt(packets: list, time_base_num: int, time_base_den: int,
              output_path: str):
    """Write .vbt binary timestamp file."""
    with open(output_path, "wb") as f:
        # Header
        f.write(VBT_MAGIC)
        f.write(struct.pack("<I", len(packets)))
        f.write(struct.pack("<i", time_base_num))
        f.write(struct.pack("<i", time_base_den))
        f.write(b"\x00" * 16)  # reserved

        # Entries
        for i, pkt in enumerate(packets):
            pts = pkt.get("pts_val", 0)
            dts = pkt.get("dts_val", 0)
            poc = i  # sequential decode order index
            size = pkt.get("size", 0)
            duration = pkt.get("duration_val", 0)
            flags = 0
            if pkt.get("keyframe", False):
                flags |= FLAG_KEYFRAME

            f.write(struct.pack("<q", pts))
            f.write(struct.pack("<q", dts))
            f.write(struct.pack("<i", poc))
            f.write(struct.pack("<I", size))
            f.write(struct.pack("<I", duration))
            f.write(struct.pack("<B", flags))
            f.write(b"\x00" * 3)  # reserved


def extract_timestamps(input_path: str, output_path: str = None,
                       verbose: bool = False):
    """Extract per-packet timestamps and write .vbt file.

    Args:
        input_path: Path to video container (.mp4, .mkv, etc.).
        output_path: Path to output .vbt file. Auto-derived if None.
        verbose: Print summary to stdout.
    """
    input_path = Path(input_path)
    if output_path is None:
        output_path = input_path.with_suffix(".vbt")

    if not input_path.exists():
        print(f"ERROR: input not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    if verbose:
        print(f"Extracting timestamps from {input_path}...")

    probe_data = _run_ffprobe(str(input_path))
    stream = probe_data["stream"]
    raw_packets = probe_data["packets"]

    # Parse time_base
    tb_str = stream.get("time_base", "1/60")
    if "/" in tb_str:
        time_base_num, time_base_den = map(int, tb_str.split("/"))
    else:
        time_base_num, time_base_den = 1, int(float(tb_str))

    # Convert packet fields
    packets = []
    for pkt in raw_packets:
        # Parse PTS/DTS as integers
        pts_str = pkt.get("pts", "N/A")
        dts_str = pkt.get("dts", "N/A")
        pts_val = int(pts_str) if pts_str != "N/A" else 0
        dts_val = int(dts_str) if dts_str != "N/A" else 0

        # Parse size
        size_val = int(pkt.get("size", 0))

        # Parse duration (in time_base units)
        dur_str = pkt.get("duration", "N/A")
        if dur_str != "N/A" and dur_str is not None:
            # ffprobe returns duration as a string that might be float
            try:
                dur_val = int(float(dur_str))
            except (ValueError, TypeError):
                dur_val = 0
        else:
            dur_val = 0

        # Keyframe flag
        flags_str = pkt.get("flags", "")
        keyframe = "K" in flags_str

        packets.append({
            "pts_val": pts_val,
            "dts_val": dts_val,
            "size": size_val,
            "duration_val": dur_val,
            "keyframe": keyframe,
            "pts_time": pkt.get("pts_time", "N/A"),
            "dts_time": pkt.get("dts_time", "N/A"),
        })

    write_vbt(packets, time_base_num, time_base_den, str(output_path))

    if verbose:
        print(f"Found {len(packets)} packets → {output_path}")
        print(f"Time base: {time_base_num}/{time_base_den}")
        keyframes = sum(1 for p in packets if p["keyframe"])
        print(f"Keyframes: {keyframes}")
        # Print first 10 packets
        for i, pkt in enumerate(packets[:10]):
            kf = "KEY" if pkt["keyframe"] else ""
            print(f"  [{i:4d}] PTS={pkt['pts_time']:>12s} DTS={pkt['dts_time']:>12s} "
                  f"size={pkt['size']:6d} dur={pkt['duration_val']:3d} {kf}")
        if len(packets) > 10:
            print(f"  ... ({len(packets) - 10} more)")

    return str(output_path)


def main():
    parser = argparse.ArgumentParser(
        description="VVC Timestamp Extractor — generate .vbt from video container",
    )
    parser.add_argument("input", help="Video container file (.mp4, .mkv, etc.)")
    parser.add_argument("-o", "--output", help="Output .vbt file (auto-derived if omitted)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print summary")
    args = parser.parse_args()

    extract_timestamps(args.input, args.output, verbose=True)


if __name__ == "__main__":
    main()
