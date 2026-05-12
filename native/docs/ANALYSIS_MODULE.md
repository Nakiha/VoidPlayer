# 码流分析模块 (Analysis)

## 概述

独立的 C++ 静态库 (`analysis_lib`)，提供 H.266/VVC 码流分析能力：NALU 索引、时间戳提取、帧级统计。不依赖 `video_renderer_lib`，仅依赖 FFmpeg（avformat/avcodec）和 spdlog。

## 目录结构

```
analysis/
├── CMakeLists.txt
├── analysis_manager.h/cpp      # 单例管理器：加载/查询分析数据
├── parsers/                    # 二进制文件解析器（只读）
│   ├── binary_types.h          # VAC2/VACHUNK/VBS4/VBI/VBT packed 结构体定义
│   ├── vac2_parser.h/cpp       # VAC2 base index
│   ├── vachunk_parser.h/cpp    # VACHUNK derived chunks
│   ├── vbs4_parser.h/cpp       # VBS4 — VTM 帧级/CU 统计
│   ├── vbi_parser.h/cpp        # VBI  — NALU 索引
│   └── vbt_parser.h/cpp        # VBT  — 时间戳/关键帧
├── generators/                 # 二进制文件生成器
│   ├── analysis_generator.h    # VBI+VBT 生成接口
│   └── analysis_generator.cpp  # FFmpeg 单趟实现
├── tools/
│   └── analysis_generate.cpp   # AnalysisGenerator 命令行入口
├── tests/python/               # Python 落盘格式回归
└── vendor/vtm/                 # 第三方 VTM 子仓库，生成 VBS4
```

## 二进制格式

Analysis 使用三类自定义二进制格式，均为小端序，结构体使用
`#pragma pack(push, 1)` 紧凑排列。结构体定义及 `static_assert` 尺寸校验见
`analysis/parsers/binary_types.h`。

独立格式文档：

- [VAC2](formats/VAC2.md) — 当前 base index 容器；只保存可快速生成的码流地图和轻量展示统计
- [VACHUNK](formats/VACHUNK.md) — 当前按需派生分析 chunk；保存 NAL detail、exact frame summary、overlay 数据
- [VBT](formats/VBT.md) — packet 时间戳/关键帧元数据 section，当前 magic `VBT1`
- [VBI](formats/VBI.md) — bitstream unit 索引 section，当前写入格式为 `VBI2`，兼容读取 legacy `VBI1`
- [VBS4](formats/VBS4.md) — 压缩/分块读取 block statistics section，当前 magic `VBS4`
- [VBS legacy](formats/VBS.md) — `.vbs2`，旧版 VBS2 说明；native runtime 不再读取

VAC2 / VACHUNK 的总体设计见
[Analysis Cache V2 Design](ANALYSIS_CACHE_V2_DESIGN.md)。当前 runtime cache
使用 VAC2 base + VACHUNK；VBS4 仍作为 codec-specific analyzer 的临时输入，
用于生成 overlay chunks。

## 生成管线

入口：

- `naki_analysis_generate_vac2_base(video_path, hash, cache_root, max_cache_bytes)`
- `naki_analysis_generate_vac2_overlay_chunk(video_path, hash, cache_root, start_frame, end_frame, max_cache_bytes)`

VAC2 base 由 FFmpeg 单趟遍历视频文件生成，直接写入
`cache/<hash>/base.vac`：

```
avformat_open_input → avformat_find_stream_info → av_read_frame 循环
  │
  ├── VBT: 从 AVPacket 提取 pts/dts/size/duration/keyframe
  └── VBI: 按 codec 解析 bitstream unit
       ├── H.264/HEVC/VVC: 优先通过 FFmpeg Annex-B bitstream filter 稳定化 packet
       ├── AV1/VP9/MPEG2: 按对应 bitstream unit 规则索引
       └── 写入 codec/unit_kind/offset/size/type/flags
```

VBS4 生成由 codec-specific decoder/analyzer 外部进程完成，通过
`analysis_ffi.cpp` 调度：

- VVC/H.266: instrumented VTM `DecoderApp`，安装到 `tools/vtm/`
- HEVC/H.265 与 H.264/AVC: instrumented FFmpeg analyzer
  `void_ffmpeg_analyzer.exe`，安装到 `tools/ffmpeg-analysis/`

VVC 当前优先通过 stdin 喂给 VTM，失败时生成临时 Annex-B `.tmp.vvc`。
HEVC/H.265 由 FFmpeg analyzer 自行 demux/decode 并写入 VBS4。overlay chunk
生成会读取临时 VBS4，并发布到 `cache/<hash>/chunks/overlay/*.vck`；临时文件
不进入 runtime cache。

## 解析器

每个解析器对应一个 `*File` 类，`open()` 读取 header 和索引，后续按需读取单帧数据：

- **Vac2BaseFile**: VAC2 section directory + base tables → frames/NALU/packet/summary fast path
- **VachunkFile**: VACHUNK section directory → overlay/exact summary/detail chunks
- **Vbs4File**: section directory + frame summaries + CU index → `read_frame(idx)` 返回 summary + CU records
- **VbiFile**: NALU 数组 → `find_vcl_nalus()` / `find_keyframes()` 筛选
- **VbtFile**: packet 数组 → `packet_at_pts()` 二分查找、`keyframe_indices()`

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
| `naki_analysis_set_overlay_track` | 将 track file id 绑定到 VAC2/VACache overlay 数据 |

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

- `test_analysis_parsers.cpp` — VBT/VBI/VBS4 解析器测试
- `test_analysis_generator.cpp` — VBI+VBT 生成测试（从 H.266 MP4 实际生成并验证）

Python 格式回归测试位于 `native/analysis/tests/python/formats/`，用于生成并校验 VBS4/VBI/VBT 文件结构：

- `analysis_generate.exe` 生成 VBI/VBT
- `python dev.py vtm analyze <video>` 生成 VBS4/VVC。`resources/` 是只读 fixture 区；直接分析 `resources/video/...` 时，生成物写入 `build/vtm_analysis/<视频名>/`。
- pytest 解析文件并校验 header、索引、NALU、帧统计等格式约束

运行：`python dev.py test`
