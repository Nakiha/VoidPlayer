# 码流分析模块 (Analysis)

## 概述

独立的 C++ 静态库 (`analysis_lib`)，提供 VAC2 base 索引、VACHUNK 派生 chunk 和 overlay 查询能力。不依赖 `video_renderer_lib`，仅依赖 FFmpeg（avformat/avcodec）和 spdlog。

## 目录结构

```
analysis/
├── CMakeLists.txt
├── analysis_manager.h/cpp      # 单例管理器：加载/查询分析数据
├── parsers/                    # 二进制文件解析器（只读）
│   ├── binary_types.h          # VAC2/VACHUNK packed 结构体定义
│   ├── vac2_parser.h/cpp       # VAC2 base index
│   ├── vachunk_parser.h/cpp    # VACHUNK derived chunks
├── generators/                 # 二进制文件生成器
│   ├── analysis_generator.h    # VAC2 base 生成接口
│   └── analysis_generator.cpp  # FFmpeg 单趟实现
└── vendor/ffmpeg/              # FFmpeg analyzer fork，按需生成 VACHUNK overlay chunk
```

## 二进制格式

Analysis 使用三类自定义二进制格式，均为小端序，结构体使用
`#pragma pack(push, 1)` 紧凑排列。结构体定义及 `static_assert` 尺寸校验见
`analysis/parsers/binary_types.h`。

独立格式文档：

- [VAC2](formats/VAC2.md) — 当前 base index 容器；只保存可快速生成的码流地图和轻量展示统计
- [VACHUNK](formats/VACHUNK.md) — 当前按需派生分析 chunk；保存 NAL detail、exact frame summary、overlay 数据

VAC2 / VACHUNK 的总体设计见
[Analysis Cache V2 Design](ANALYSIS_CACHE_V2_DESIGN.md)。当前 runtime cache
使用 VAC2 base + VACHUNK；runtime overlay 直接消费 overlay VACHUNK。

## 生成管线

入口：

- `naki_analysis_generate_vac2_base(video_path, hash, cache_root, max_cache_bytes)`
- `naki_analysis_generate_vac2_overlay_chunk(video_path, hash, cache_root, start_frame, end_frame, max_cache_bytes)`

VAC2 base 由 FFmpeg 单趟遍历视频文件生成，直接写入
`cache/<hash>/base.vac`：

```
avformat_open_input → avformat_find_stream_info → av_read_frame 循环
  │
  ├── PKT2/frames: 从 AVPacket 提取 pts/dts/size/duration/keyframe
  └── BSU2: 按 codec 解析 bitstream unit
       ├── H.264/HEVC/VVC: 优先通过 FFmpeg Annex-B bitstream filter 稳定化 packet
       ├── AV1/VP9/MPEG2: 按对应 bitstream unit 规则索引
       └── 写入 codec/unit_kind/offset/size/type/flags
```

Overlay VACHUNK 生成由 codec-specific decoder/analyzer 外部进程完成，通过
`analysis_ffi.cpp` 调度：

- VVC/H.266、HEVC/H.265 与 H.264/AVC: instrumented FFmpeg analyzer
  `void_ffmpeg_analyzer.exe`，安装到 `tools/ffmpeg-analysis/`

FFmpeg analyzer 自行 demux/decode，并直接写入未压缩临时 `.vck`。runner 验证
VACHUNK header/key 后，通过统一 VACHUNK writer 重新写出并原子发布到
`cache/<hash>/chunks/overlay/*.vck`；发布步骤会按 section 判断是否使用 zstd
压缩，临时文件不进入 runtime cache。

Overlay chunk 当前按 64 帧对齐窗口生成。Dart 侧在 seek settle 后用 renderer
实际 presented PTS+DTS 解析目标帧；命中窗口边界前后 1/4 区域时，会把相邻窗口
一起排队生成，以避免 native 暂停帧和 Dart timestamp lookup 在边界差一帧时
overlay redraw 读不到实际显示帧。

## 解析器

每个解析器对应一个 `*File` 类，`open()` 读取 header 和索引，后续按需读取单帧数据：

- **Vac2BaseFile**: VAC2 section directory + base tables → frames/NALU/packet/summary fast path
- **VachunkFile**: VACHUNK section directory → overlay/exact summary/detail chunks

## FFI 桥接

`windows/runner/analysis_ffi.cpp` 将分析功能暴露给 Flutter/Dart：

| FFI 函数 | 功能 |
|----------|------|
| `naki_analysis_generate_vac2_base` | 生成 `cache/<hash>/base.vac` VAC2 base |
| `naki_analysis_generate_vac2_overlay_chunk` | 生成 `cache/<hash>/chunks/overlay/*.vck` overlay VACHUNK |
| `naki_analysis_open/close` | 打开/关闭 VAC2 base handle |
| `naki_analysis_handle_get_summary` | 返回概要（帧数/分辨率/time_base/当前帧） |
| `naki_analysis_handle_get_frames_range` | 返回 VAC2 frame summary + packet timing 数组 |
| `naki_analysis_handle_get_nalus_range` | 返回 VAC2 bitstream-unit 数组 |
| `naki_analysis_handle_get_frame_buckets` | 返回 VAC2 frame bucket 聚合 |
| `naki_analysis_set_overlay` | 设置叠加层显示状态 |
| `naki_analysis_set_overlay_track` | 将 track file id 绑定到 VAC2/VACHUNK overlay 数据 |

ABI / lifecycle notes:

- `naki_analysis_open` / `naki_analysis_close` / `naki_analysis_handle_*` are the
  preferred APIs for new code. Handle state is pinned with `shared_ptr`, so
  closing a handle is safe while readers already inside an FFI call finish.
- Flat singleton reader exports were removed from Dart and native FFI; analysis
  data reads are handle-scoped. Global functions remain only for renderer-facing
  overlay state.
- Legacy flat structs remain unchanged for Dart compatibility. V2 wrapper
  structs add `size` and `abi_version` headers for future callers without
  changing the ABI.
- `naki_analysis_last_error(buf, cap)` returns thread-local status/message for
  the most recent analysis FFI call on the same thread.
- Summary pointers point to thread-local snapshots and are valid only until the
  next analysis FFI call on that thread. Bulk data APIs use caller-provided
  output buffers.

## 测试

独立测试目标 `analysis_tests`（Catch2），位于 `native/tests/analysis/`：

- `test_analysis_parsers.cpp` — VAC2/VACHUNK parser 与 cache 测试
- `test_analysis_generator.cpp` — VAC2 base 生成测试（覆盖 H.264/HEVC/VVC/AV1/VP9/MPEG2 样本）

运行：`python dev.py test`

全片 VAC2 + VACHUNK 生成性能和落盘体积可用 benchmark 脚本覆盖：

```bash
python dev.py analysis-benchmark --build
python dev.py analysis-benchmark h264 h265 h266
```

报告默认写入 `build/analysis-benchmark/analysis_benchmark.json` 和
`build/analysis-benchmark/analysis_benchmark.md`，包含每个样片的 base/chunk
耗时、最终 cache 大小、视频大小占比、section 原始/压缩大小，以及 zstd 节省量。

Overlay heatmap CPU raster 成本可用独立 benchmark 覆盖：

```bash
python dev.py analysis-overlay-benchmark --iterations 240 --with-grid
```

该命令会生成一个临时 VAC2 base 和 overlay VACHUNK，然后调用
`VoidPlayerCli.exe benchmark-overlay` 对指定帧重复栅格化，报告写入
`build/analysis-overlay-benchmark/analysis_overlay_benchmark.json` 和 `.md`。
它用于检查 CU/MB 热力图填充和边界 mask 路径的回归；不包含 GUI 中的 D3D
texture upload 和最终窗口合成，但报告会给出 color/mask upload 字节数估算。
