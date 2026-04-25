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
└── generators/                 # 二进制文件生成器
    ├── analysis_generator.h    # VBI+VBT 生成接口
    └── analysis_generator.cpp  # FFmpeg 单趟实现
```

## 二进制格式

三种自定义二进制格式，均使用 `#pragma pack(push, 1)` 紧凑排列，小端序：

| 格式 | 用途 | Header | Entry | 产出方 |
|------|------|--------|-------|--------|
| **VBI** | NALU 索引 | 16B (magic "VBI1" + num_nalus + source_size) | 16B (offset + size + nal_type + tid + layer_id + flags) | `AnalysisGenerator` |
| **VBT** | 时间戳 | 32B (magic "VBT1" + num_packets + time_base) | 32B (pts + dts + poc + size + duration + flags) | `AnalysisGenerator` |
| **VBS2** | CU 统计 | 16B (magic "VBS2" + width/height + num_frames) | 134B frame header + 变长 CU records | VTM DecoderApp（外部工具） |

结构体定义及 `static_assert` 尺寸校验见 `parsers/binary_types.h`。

## 生成管线

入口：`AnalysisGenerator::generate(video_path, vbi_path, vbt_path)`

单趟遍历视频文件，同时产出 VBI 和 VBT：

```
avformat_open_input → avformat_find_stream_info → av_read_frame 循环
  │
  ├── VBT: 从 AVPacket 提取 pts/dts/size/duration/keyframe
  │
  └── VBI: 解析 packet data 中的 NALU
       ├── Annex B (start code 00 00 00 01) → 扫描 start code
       └── Length-prefixed (MP4/MKV 容器) → 读 4B BE 长度前缀
       └── 解析 2B VVC NALU header → nal_type/temporal_id/layer_id/flags
```

NALU 分类：
- **VCL** (flags bit0): nal_type 0-11
- **Slice** (flags bit1): 0,1,2,3,7,8,9,10
- **Keyframe** (flags bit2): 7,8,9 (IDR_W_RADL, IDR_N_LP, CRA)

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

运行：`python dev.py test`
