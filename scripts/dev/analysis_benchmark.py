"""Benchmark VAC2/VACHUNK generation and cache footprint."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

from .native import build_macos_analysis_cli, ensure_ffmpeg_analyzer_tool
from .paths import ROOT, find_ffmpeg_analyzer, find_voidplayer_cli
from .process import header, run


DEFAULT_SAMPLES = [
    ("h264", ROOT / "resources" / "video" / "h264_9s_1920x1080.mp4"),
    ("hevc", ROOT / "resources" / "video" / "h265_10s_1920x1080.mp4"),
    ("vvc", ROOT / "resources" / "video" / "h266_10s_1920x1080.mp4"),
]


def cmd_analysis_benchmark(args) -> None:
    """Run end-to-end analysis cache benchmarks for bundled samples."""
    ensure_ffmpeg_analyzer_tool()

    if args.build:
        if sys.platform == "win32":
            header("Build Flutter release for VoidPlayerCli")
            run(["flutter", "build", "windows", "--release"], cwd=str(ROOT))
        elif sys.platform == "darwin":
            build_macos_analysis_cli()

    cli = _find_cli()
    analyzer = find_ffmpeg_analyzer()
    if not cli.exists():
        print(f"ERROR: VoidPlayerCli not found: {cli}")
        print("Run: python dev.py analysis-benchmark --build")
        sys.exit(1)
    if not analyzer.exists():
        print(f"ERROR: void_ffmpeg_analyzer not found: {analyzer}")
        sys.exit(1)

    out_dir = Path(args.output_dir).resolve() if args.output_dir else ROOT / "build" / "analysis-benchmark"
    cache_root = Path(args.cache_root).resolve() if args.cache_root else out_dir / "cache"
    if cache_root.exists() and not args.keep_cache:
        shutil.rmtree(cache_root)
    cache_root.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    samples = _selected_samples(args.samples)
    results = []
    for codec, video in samples:
        if not video.exists():
            print(f"SKIP missing sample: {video}")
            continue
        results.append(_benchmark_sample(
            cli=cli,
            analyzer=analyzer,
            codec=codec,
            video=video,
            cache_root=cache_root,
        ))

    report = {
        "schema": "voidplayer-analysis-benchmark-v1",
        "cli": str(cli),
        "analyzer": str(analyzer),
        "cacheRoot": str(cache_root),
        "samples": results,
    }
    json_path = out_dir / "analysis_benchmark.json"
    md_path = out_dir / "analysis_benchmark.md"
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    md_path.write_text(_format_markdown(report), encoding="utf-8")
    print(f"\nBenchmark JSON: {json_path}")
    print(f"Benchmark report: {md_path}")


def _find_cli() -> Path:
    return find_voidplayer_cli()


def _selected_samples(names: list[str] | None) -> list[tuple[str, Path]]:
    if not names:
        return DEFAULT_SAMPLES
    aliases = {
        "h265": "hevc",
        "h266": "vvc",
    }
    wanted = {aliases.get(name.lower(), name.lower()) for name in names}
    return [(codec, path) for codec, path in DEFAULT_SAMPLES if codec in wanted or path.name.lower() in wanted]


def _hash_for_sample(codec: str, video: Path) -> str:
    stat = video.stat()
    text = f"{codec}:{video.name}:{stat.st_size}:{int(stat.st_mtime_ns)}"
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _run_json(command: list[str]) -> tuple[dict, float]:
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        cwd=str(ROOT),
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(
            "command failed:\n"
            f"  {' '.join(command)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    lines = [line for line in proc.stdout.splitlines() if line.strip().startswith("{")]
    if not lines:
        raise RuntimeError(f"command produced no JSON: {' '.join(command)}\n{proc.stdout}")
    return json.loads(lines[-1]), elapsed


def _benchmark_sample(
    *,
    cli: Path,
    analyzer: Path,
    codec: str,
    video: Path,
    cache_root: Path,
) -> dict:
    print(f"\nBenchmark {codec}: {video.name}")
    sample_hash = _hash_for_sample(codec, video)

    base, base_seconds = _run_json([
        str(cli),
        "generate-base",
        "--input", str(video),
        "--cache-root", str(cache_root),
        "--hash", sample_hash,
        "--json",
    ])
    frames = int(base["frames"])
    chunk, chunk_seconds = _run_json([
        str(cli),
        "generate-overlay",
        "--input", str(video),
        "--cache-root", str(cache_root),
        "--hash", sample_hash,
        "--start-frame", "0",
        "--end-frame", str(frames - 1),
        "--codec", codec,
        "--analyzer", str(analyzer),
        "--json",
    ])
    base_path = cache_root / sample_hash / "base.vac"
    chunk_path = Path(chunk["path"])
    chunk_inspect, _ = _run_json([str(cli), "inspect", str(chunk_path), "--json", "--limit", "0"])

    section_bytes = 0
    section_decoded = 0
    compressed_sections = 0
    for section in chunk_inspect.get("sections", []):
        section_bytes += int(section.get("size", 0))
        section_decoded += int(section.get("decodedSize", 0))
        if section.get("compressed"):
            compressed_sections += 1

    video_bytes = video.stat().st_size
    base_bytes = base_path.stat().st_size
    chunk_bytes = chunk_path.stat().st_size
    result = {
        "codec": codec,
        "video": str(video),
        "videoBytes": video_bytes,
        "hash": sample_hash,
        "frames": frames,
        "packets": int(base["packets"]),
        "units": int(base["units"]),
        "baseSeconds": base_seconds,
        "overlaySeconds": chunk_seconds,
        "totalSeconds": base_seconds + chunk_seconds,
        "baseBytes": base_bytes,
        "overlayBytes": chunk_bytes,
        "totalCacheBytes": base_bytes + chunk_bytes,
        "cacheToVideoRatio": (base_bytes + chunk_bytes) / video_bytes if video_bytes else 0,
        "overlayCompression": chunk_inspect.get("compression", "unknown"),
        "overlaySectionBytes": section_bytes,
        "overlaySectionDecodedBytes": section_decoded,
        "overlayCompressedSections": compressed_sections,
        "overlaySectionSavedBytes": max(0, section_decoded - section_bytes),
        "overlayPath": str(chunk_path),
    }
    print(
        f"  frames={frames} base={base_seconds:.3f}s overlay={chunk_seconds:.3f}s "
        f"cache={_fmt_bytes(base_bytes + chunk_bytes)} ratio={result['cacheToVideoRatio']:.2%} "
        f"zstd_saved={_fmt_bytes(result['overlaySectionSavedBytes'])}"
    )
    return result


def _fmt_bytes(value: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    number = float(value)
    for unit in units:
        if number < 1024 or unit == units[-1]:
            return f"{number:.1f} {unit}" if unit != "B" else f"{int(number)} B"
        number /= 1024
    return f"{value} B"


def _format_markdown(report: dict) -> str:
    lines = [
        "# Analysis Benchmark",
        "",
        f"- CLI: `{report['cli']}`",
        f"- Analyzer: `{report['analyzer']}`",
        f"- Cache root: `{report['cacheRoot']}`",
        "",
        "| Codec | Frames | VAC2 | VACHUNK | Total Cache | Cache / Video | Base Time | Chunk Time | Zstd Saved |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for item in report["samples"]:
        lines.append(
            "| {codec} | {frames} | {base} | {chunk} | {total} | {ratio:.2%} | {base_s:.3f}s | {chunk_s:.3f}s | {saved} |".format(
                codec=item["codec"],
                frames=item["frames"],
                base=_fmt_bytes(item["baseBytes"]),
                chunk=_fmt_bytes(item["overlayBytes"]),
                total=_fmt_bytes(item["totalCacheBytes"]),
                ratio=item["cacheToVideoRatio"],
                base_s=item["baseSeconds"],
                chunk_s=item["overlaySeconds"],
                saved=_fmt_bytes(item["overlaySectionSavedBytes"]),
            )
        )
    lines.append("")
    lines.append("Notes:")
    lines.append("- VACHUNK values cover one full-file overlay chunk per sample.")
    lines.append("- `Zstd Saved` compares decoded section bytes with on-disk section bytes.")
    return "\n".join(lines) + "\n"
