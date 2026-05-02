# 码流分析模块 (Analysis)

## 概述

独立的 C++ 静态库 (`analysis_lib`)，提供 H.266/VVC 码流分析能力：NALU 索引、时间戳提取、帧级统计。不依赖 `video_renderer_lib`，仅依赖 FFmpeg（avformat/avcodec）和 spdlog。

## 目录结构

```
analysis/
├── CMakeLists.txt
├── analysis_manager.h/cpp      # 单例管理器：加载/查询分析数据
├── parsers/                    # 二进制文件解析器（只读）
│   ├── binary_types.h          # VBS2/VBI/VBT packed 结构体定义
│   ├── vbs2_parser.h/cpp       # VBS2 — VTM 帧级 CU 统计
│   ├── vbi_parser.h/cpp        # VBI  — NALU 索引
│   └── vbt_parser.h/cpp        # VBT  — 时间戳/关键帧
├── generators/                 # 二进制文件生成器
│   ├── analysis_generator.h    # VBI+VBT 生成接口
│   └── analysis_generator.cpp  # FFmpeg 单趟实现
├── tools/
│   └── analysis_generate.cpp   # AnalysisGenerator 命令行入口
├── tests/python/               # Python 落盘格式回归
└── vendor/vtm/                 # 第三方 VTM 子仓库，生成 VBS2
```

## 二进制格式

Analysis 使用三类自定义二进制格式，均为小端序，结构体使用
`#pragma pack(push, 1)` 紧凑排列。结构体定义及 `static_assert` 尺寸校验见
`analysis/parsers/binary_types.h`。

独立格式文档：

- [VBT](formats/VBT.md) — `.vbt`，packet 时间戳/关键帧元数据，当前 magic `VBT1`
- [VBI](formats/VBI.md) — `.vbi`，bitstream unit 索引；扩展名保持 `.vbi`，当前写入格式为 `VBI2`，兼容读取 legacy `VBI1`
- [VBS](formats/VBS.md) — `.vbs2`，VTM block statistics / CU 统计，当前 magic `VBS2`
- [VBS3](formats/VBS3.md) — `.vbs3`，VTM 可写的 VBS2 后继格式；目标是为大文件提供帧摘要、CU payload 索引和 64-bit offset（native reader 尚未接入）

## 生成管线

入口：`AnalysisGenerator::generate(video_path, vbi_path, vbt_path)`

单趟遍历视频文件，同时产出 VBI 和 VBT：

```
avformat_open_input → avformat_find_stream_info → av_read_frame 循环
  │
  ├── VBT: 从 AVPacket 提取 pts/dts/size/duration/keyframe
  └── VBI: 按 codec 解析 bitstream unit
       ├── H.264/HEVC/VVC: 优先通过 FFmpeg Annex-B bitstream filter 稳定化 packet
       ├── AV1/VP9/MPEG2: 按对应 bitstream unit 规则索引
       └── 写入 codec/unit_kind/offset/size/type/flags
```

VBS2 生成由 VTM DecoderApp 外部进程完成（可选），通过 `analysis_ffi.cpp` 调用。

## 解析器

每个解析器对应一个 `*File` 类，`open()` 读取 header 和索引，后续按需读取单帧数据：

- **Vbs2File**: 帧索引 → `read_frame(idx)` 返回 header + CU records
- **VbiFile**: NALU 数组 → `find_vcl_nalus()` / `find_keyframes()` 筛选
- **VbtFile**: packet 数组 → `packet_at_pts()` 二分查找、`keyframe_indices()`

## FFI 桥接

`windows/runner/analysis_ffi.cpp` 将分析功能暴露给 Flutter/Dart：

| FFI 函数 | 功能 |
|----------|------|
| `naki_analysis_generate` | 生成 VBI+VBT（C++ FFmpeg），可选 VBS2（VTM DecoderApp） |
| `naki_analysis_load/unload` | 加载/卸载分析文件到内存 |
| `naki_analysis_get_summary` | 返回概要（帧数/分辨率/time_base/当前帧） |
| `naki_analysis_get_frames` | 返回帧信息数组（VBS2+VBT 合并） |
| `naki_analysis_get_nalus` | 返回 NALU 信息数组 |
| `naki_analysis_set_overlay` | 设置叠加层显示状态 |

## 测试

独立测试目标 `analysis_tests`（Catch2），位于 `native/tests/analysis/`：

- `test_analysis_parsers.cpp` — VBT/VBI/VBS2 解析器测试
- `test_analysis_generator.cpp` — VBI+VBT 生成测试（从 H.266 MP4 实际生成并验证）

Python 格式回归测试位于 `native/analysis/tests/python/formats/`，用于生成并校验 VBS2/VBS3/VBI/VBT 文件结构：

- `analysis_generate.exe` 生成 VBI/VBT
- `python dev.py vtm analyze --format vbs2/vbs3` 生成 VBS/VVC。`resources/` 是只读 fixture 区；直接分析 `resources/video/...` 时，生成物写入 `build/vtm_analysis/<视频名>/`。
- pytest 解析文件并校验 header、索引、NALU、帧统计等格式约束

运行：`python dev.py test`
