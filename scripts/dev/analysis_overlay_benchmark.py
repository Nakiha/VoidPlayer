"""Benchmark native analysis overlay rasterization."""

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


DEFAULT_SAMPLE = ("vvc", ROOT / "resources" / "video" / "h266_10s_1920x1080.mp4")


def cmd_analysis_overlay_benchmark(args) -> None:
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
        print("Run: python dev.py analysis-overlay-benchmark --build")
        sys.exit(1)
    if not analyzer.exists():
        print(f"ERROR: void_ffmpeg_analyzer not found: {analyzer}")
        sys.exit(1)

    out_dir = Path(args.output_dir).resolve() if args.output_dir else ROOT / "build" / "analysis-overlay-benchmark"
    cache_root = Path(args.cache_root).resolve() if args.cache_root else out_dir / "cache"
    if cache_root.exists() and not args.keep_cache:
        shutil.rmtree(cache_root)
    cache_root.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    codec, video = DEFAULT_SAMPLE
    if args.video:
        video = Path(args.video).resolve()
        codec = args.codec
    if not video.exists():
        print(f"ERROR: sample video not found: {video}")
        sys.exit(1)

    result = _benchmark(
        cli=cli,
        analyzer=analyzer,
        codec=codec,
        video=video,
        cache_root=cache_root,
        frame=args.frame,
        width=args.width,
        height=args.height,
        iterations=args.iterations,
        mode=args.mode,
        with_grid=args.with_grid,
        run_gpu=(sys.platform == "win32" and not args.skip_gpu),
    )
    report = {
        "schema": "voidplayer-analysis-overlay-benchmark-v1",
        "cli": str(cli),
        "analyzer": str(analyzer),
        "cacheRoot": str(cache_root),
        "result": result,
    }
    json_path = out_dir / "analysis_overlay_benchmark.json"
    md_path = out_dir / "analysis_overlay_benchmark.md"
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    md_path.write_text(_format_markdown(report), encoding="utf-8")
    print(f"\nOverlay benchmark JSON: {json_path}")
    print(f"Overlay benchmark report: {md_path}")


def _find_cli() -> Path:
    return find_voidplayer_cli()


def _hash_for_sample(codec: str, video: Path) -> str:
    stat = video.stat()
    text = f"overlay:{codec}:{video.name}:{stat.st_size}:{int(stat.st_mtime_ns)}"
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


def _benchmark(
    *,
    cli: Path,
    analyzer: Path,
    codec: str,
    video: Path,
    cache_root: Path,
    frame: int,
    width: int,
    height: int,
    iterations: int,
    mode: str,
    with_grid: bool,
    run_gpu: bool,
) -> dict:
    sample_hash = _hash_for_sample(codec, video)
    print(f"\nOverlay benchmark {codec}: {video.name} frame={frame} mode={mode}")
    base, _ = _run_json([
        str(cli),
        "generate-base",
        "--input", str(video),
        "--cache-root", str(cache_root),
        "--hash", sample_hash,
        "--json",
    ])
    frames = int(base["frames"])
    end_frame = max(frame, min(frames - 1, 63))
    chunk, overlay_seconds = _run_json([
        str(cli),
        "generate-overlay",
        "--input", str(video),
        "--cache-root", str(cache_root),
        "--hash", sample_hash,
        "--start-frame", "0",
        "--end-frame", str(end_frame),
        "--codec", codec,
        "--analyzer", str(analyzer),
        "--json",
    ])
    bench_command = [
        str(cli),
        "benchmark-overlay",
        chunk["path"],
        "--frame", str(frame),
        "--width", str(width),
        "--height", str(height),
        "--iterations", str(iterations),
        "--mode", mode,
    ]
    if with_grid:
        bench_command.append("--with-grid")
    bench_command.append("--json")
    bench, _ = _run_json(bench_command)
    gpu_bench = None
    if run_gpu:
        gpu_command = [
            str(cli),
            "benchmark-overlay-gpu",
            chunk["path"],
            "--frame", str(frame),
            "--width", str(width),
            "--height", str(height),
            "--iterations", str(iterations),
            "--mode", mode,
        ]
        if with_grid:
            gpu_command.append("--with-grid")
        gpu_command.append("--json")
        gpu_bench, _ = _run_json(gpu_command)
    bench["codec"] = codec
    bench["video"] = str(video)
    bench["frames"] = frames
    bench["overlayGenerateSeconds"] = overlay_seconds
    bench["chunkPath"] = chunk["path"]
    if gpu_bench is not None:
        bench["gpu"] = gpu_bench
    print(
        f"  avg={bench['avgMs']:.3f}ms iterations={iterations} "
        f"cus={bench['cuCount']} filled={bench['filledPixels']} "
        f"gpuUpload={bench.get('gpuEstimatedUploadBytes', 0)}"
    )
    if gpu_bench is not None:
        print(
            f"  gpu device={gpu_bench.get('deviceType', 'unknown')} "
            f"full={gpu_bench['fullOverlayGpuMs']:.4f}ms "
            f"color={gpu_bench['colorPassGpuMs']:.4f}ms "
            f"mask={gpu_bench['maskPassGpuMs']:.4f}ms "
            f"invert={gpu_bench['invertPassGpuMs']:.4f}ms "
            f"uploadCpu={gpu_bench['rectUploadCpuMs']:.4f}ms"
        )
    return bench


def _format_markdown(report: dict) -> str:
    r = report["result"]
    text = (
        "# Analysis Overlay Benchmark\n\n"
        f"- CLI: `{report['cli']}`\n"
        f"- Analyzer: `{report['analyzer']}`\n"
        f"- Video: `{r['video']}`\n"
        f"- Chunk: `{r['chunkPath']}`\n\n"
        "| Codec | Frame | Mode | Grid | Size | Iterations | CUs | Filled Pixels | CPU Upload Bytes | GPU Rect Upload Bytes | Avg Raster |\n"
        "| --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n"
        f"| {r['codec']} | {r['frame']} | {r['mode']} | {'yes' if r.get('withGrid') else 'no'} | "
        f"{r['width']}x{r['height']} | {r['iterations']} | {r['cuCount']} | "
        f"{r['filledPixels']} | {r.get('colorUploadBytes', 0) + r.get('maskUploadBytes', 0)} | "
        f"{r.get('gpuEstimatedUploadBytes', 0)} | "
        f"{r['avgMs']:.3f} ms |\n"
    )
    gpu = r.get("gpu")
    if gpu:
        text += (
            "\n## GPU Pass Timing\n\n"
            "| Device | Rect Upload CPU | Color Pass GPU | Mask Pass GPU | Invert Pass GPU | Full Overlay GPU |\n"
            "| --- | ---: | ---: | ---: | ---: | ---: |\n"
            f"| {gpu.get('deviceType', 'unknown')} | "
            f"{gpu.get('rectUploadCpuMs', 0):.4f} ms | "
            f"{gpu.get('colorPassGpuMs', 0):.4f} ms | "
            f"{gpu.get('maskPassGpuMs', 0):.4f} ms | "
            f"{gpu.get('invertPassGpuMs', 0):.4f} ms | "
            f"{gpu.get('fullOverlayGpuMs', 0):.4f} ms |\n"
        )
    return text
