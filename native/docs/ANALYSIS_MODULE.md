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
[Analysis Cache](ANALYSIS_CACHE.md)。当前 runtime cache
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

`native/analysis/analysis_ffi_bridge.cpp` 将分析功能统一暴露给 Windows 和 macOS 的
Flutter/Dart。两端共用 `native/analysis/analysis_ffi_abi.h` 中的 flat ABI struct 定义；
Windows 仍承载完整 analysis UI/IPC。macOS 当前提供 VAC2 base 生成、只读 handle
查询、overlay state/track 绑定符号，以及通过 bundled `void_ffmpeg_analyzer` 的 runtime
overlay VACHUNK 生成；外部 analysis 窗体和 analysis UI/IPC 仍保持 capability-gated。

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
- macOS runner builds force-load the native macOS player archive so the
  `naki_analysis_*` symbols remain visible to Dart FFI even though Swift does
  not call them directly. `macos_analysis_ffi_smoke` covers VAC2 generation,
  open/read/close, frame/NALU range reads, and bucket aggregation on macOS.

## 测试

独立测试目标 `analysis_tests`（Catch2），位于 `native/tests/analysis/`：

- `test_analysis_parsers.cpp` — VAC2/VACHUNK parser 与 cache 测试
- `test_analysis_generator.cpp` — VAC2 base 生成测试（覆盖 H.264/HEVC/VVC/AV1/VP9/MPEG2 样本）
- `test_quality_metrics.cpp` — 实验性 CPU quality metrics 的确定性合成图案测试

运行：`python dev.py test`

全片 VAC2 + VACHUNK 生成性能和落盘体积可用 benchmark 脚本覆盖；Windows 和
macOS 均可运行，macOS 的 `--build` 会构建 portable `VoidPlayerCli` 和本机
`void_ffmpeg_analyzer`：

```bash
python dev.py analysis-benchmark --build
python dev.py analysis-benchmark h264 h265 h266
```

报告默认写入 `build/analysis-benchmark/analysis_benchmark.json` 和
`build/analysis-benchmark/analysis_benchmark.md`，包含每个样片的 base/chunk
耗时、最终 cache 大小、视频大小占比、section 原始/压缩大小，以及 zstd 节省量。

## 实验性无参考质量打分

`quality_metrics_lib` 是不依赖 FFmpeg 的内部静态库。它消费显式的 luma plane
描述，不读取窗口、swap chain 或 compositor 输出；`analysis_lib` 中的
`quality_video_analyzer` 保留离线同步消费、抽样和 frame encoding
parameter side-data/QP 汇总，但输入 open/probe/read/seek/interrupt 通过
`media/MediaInputSession`、codec context/decoder fallback/send/receive 通过
`media/VideoDecodeSession` 和播放器共用。`analysis_lib` 只链接独立的
`void_video_decode_core`，不会反向依赖 renderer、Flutter 或平台 presentation。
当前 CPU reference 明确不使用 `libswscale`。

当前 CLI 输出 `schemaVersion` 为 `5`，指标算法版本为
`metricVersion: quality-demo-v5`。schema 与算法版本仍分开演进；当前两者恰好同为 v5。
算法 v5 在 v4 整帧指标和实验性空间区域基础上提供：

- packet size、frame type 和可用 QP 的分布统计；
- `blockiness` proxy；
- `banding` proxy；
- 基于重模糊前后边缘变化损失的 `blur` proxy；
- 基于低纹理块、截断高频残差并按 8-bit `sigma / 24` 归一化的 `noise` proxy；
- 基于连续三帧 tile luma 二阶变化、带切镜过滤的 `flicker` temporal proxy；
- 抽样帧空间指标时间线，以及全解码帧计算的 flicker 分布。
- sampled frame 的 `spatialRegions`；当前仅 `banding` 输出基于 16×16 luma tile
  聚类的候选区域，同时给出归一化矩形、像素矩形、区域分数、检测阈值和 tile 数。
- 可选的 `quality-tile-v1` 证据流：五项指标使用同一约 64×64、边缘均衡的 decoded
  luma 网格，逐 tile 输出 `[0, 1]` 局部分数。
- 每项 CPU 指标的毫秒耗时分布；schema 中保留的 GPU timing 字段在当前
  CPU-only build 中标记为不可用。

这些分数都标记为实验性 `proxy`：`banding` 当前不等同于 CAMBI，
`blur` 和 `noise` 仍会受画面内容影响，`flicker` 会过滤明显切镜但不能消除所有运动
和灯光变化干扰。当前没有融合成总质量分。
`spatialRegions` 是辅助定位证据，不替代整帧分数；弱响应、少于四个相邻 tile 的
孤立响应不会输出区域。banding region 使用 `banding-tile-cc-v2`：除了至少四个相邻
tile，还要求横纵各跨至少两个 tile、包围盒填充率至少 0.5，以过滤单 tile 厚细条和
稀疏连通噪声；每个区域输出 `tileSpan` 与 `fillRatio` 供下游审计。blockiness、blur、
noise 和 flicker 虽不生成矩形候选，但可通过 tile 证据流提供真实局部分数；GUI 后续可
自行选择热力图或基于 tile 的区域化呈现，不能把缺失区域伪造成整帧框。
命令行入口：

```bash
VoidPlayerCli score-quality --input input.mp4 --json
VoidPlayerCli score-quality --input input.mp4 --backend cpu --json
VoidPlayerCli score-quality --input input.mp4 --backend cpu --cpu-mode scalar --json
VoidPlayerCli score-quality --input input.mp4 --backend cpu --decode-threads 32 --cpu-workers 96 --cpu-in-flight 48 --json
VoidPlayerCli score-quality --input input.mp4 --sample-interval-ms 500 --max-samples 10 --json
VoidPlayerCli score-quality --input input.mp4 --metrics banding,flicker --regions summary --json
VoidPlayerCli score-quality --input input.mp4 --metrics blockiness,blur --regions none --jsonl
VoidPlayerCli score-quality --input input.mp4 --events candidates --regions full --jsonl
VoidPlayerCli score-quality --input input.mp4 --events none --jsonl
VoidPlayerCli score-quality --input input.mp4 --tiles full --events none --jsonl
VoidPlayerCli score-quality --input input.mp4 --json --summary-only
VoidPlayerCli score-quality --input input.mp4 --jsonl
```

`--metrics` 接受 `all` 或不重复的逗号分隔子集；CPU analyzer 只调度选中的空间指标，
未选指标在 sample 中为 `null`，对应 distribution/timing 的 `available` 为 `false`。
`--regions none|summary|full` 分别关闭区域计算、仅保留 report 级汇总、或同时输出逐帧
矩形；默认 `full`。`--summary-only` 会把有效区域模式从 `full` 降为 `summary`，因此省略
timeline 时仍保留区域数量、出现帧数、分数、面积和帧覆盖率分布。

`--tiles none|full` 默认 `none`；`full` 仅支持 `--jsonl`，并在每个
`qualityFrameSample` 后输出同 `sampleIndex` 的 `qualityTileSample`。四项空间 proxy
在统一 tile 内重新计算；flicker 使用同网格的连续三帧局部亮度二阶变化。首两帧、切镜
或网格不兼容时 flicker 明确为 `available: false` / `values: null`，不会用零分冒充观测。
帧级 proxy 仍是权威汇总，不能假定它等于 tile 数组的算术平均。tile 协议和算法分别以
`quality-tile-v1`、`quality-tile-metrics-v1` 独立版本化，正式 schema 为
[quality-tile-v1.schema.json](quality-tile-v1.schema.json)。

`--events none|candidates` 控制 JSONL 的 `qualityEvent`，默认 `candidates`。候选策略版本
为 `quality-candidate-policy-v1`：banding 每个检测区域建立独立候选轨道，保留各自峰值帧
的真实局部矩形，且只一对一合并时间连续并且区域 IoU 至少 0.10 的样本；同帧多个问题
不会丢失或合成大框。其余指标使用视频内部的稳健相对异常阈值
`max(P90, median + 3 * 1.4826 * MAD)`，至少需要五个有效样本。相对候选的 `region` 为
`null`，下游只能生成时间标记，不能伪造成整帧矩形。所有事件均明确
`calibrated: false`，不是绝对 pass/fail 判定。空间事件仅在 `--regions full` 下输出。
正式 schema 为 [quality-event-v1.schema.json](quality-event-v1.schema.json)。

`--json` 输出一个完整 report；`--json --summary-only` 省略 timeline，适合直接交给
Agent 或存储批量摘要；`--jsonl` 首行输出不含内联 timeline 的 report，后续每行输出一个
`qualityFrameSample`，适合长视频流式摄取。report 明确记录 `selectedMetrics` 以及请求/实际
区域输出模式。机器输出失败时返回非零退出码，并在 stdout 输出 `qualityError` JSON
envelope。CLI 的 native diagnostics 固定写入 stderr，因此 stdout 在 `--json` 下是单个
JSON 对象，在 `--jsonl` 下是纯 JSON Lines。

schema v5 保留 v4 的 `metrics`、`stream`、`timingsMs` 和 `timeline` 读取路径，同时增加：

- `selectedMetrics` 与每项指标的 `selected`，让未选中的 `null` 与真实零分可区分；
- `regionSummary`，即使 timeline 被省略也能消费空间证据；
- `capabilities.requestedRegionOutput` / `effectiveRegionOutput`，避免静默降级；

- `metricDefinitions`：每项指标的范围、方向、单位、实验状态和简述；
- `execution`：请求/实际 backend、CPU dispatch、decode threads、worker 和队列深度；
- `capabilities`：timeline 编码方式，以及是否支持绝对阈值或跨指标融合；
- `capabilities.spatialRegionMetrics`：当前可输出局部区域的指标列表；
- `sampling.maxSamples` / `sampling.truncated`：区分完整分析与因
  `--max-samples` 提前停止的 packet/frame/QP/flicker 前缀统计；
- `warnings`：QP 缺失、unsupported layout、重复 PTS 等可操作提示；
- distribution 的 `available`，避免把 `count=0` 的零值误当真实观测；
- timeline 的 `sampleIndex` 和 `decodedFrameIndex`，其中 `sampleIndex` 是唯一键；
- `timingSemantics`，明确并行 metric task 时间不能相加作为帧墙钟时间。

正式 JSON Schema：
[quality-output-v5.schema.json](quality-output-v5.schema.json)。旧版
[quality-output-v4.schema.json](quality-output-v4.schema.json) 保留用于历史产物验证。

CLI/GUI 的进程生命周期、JSONL 顺序、请求身份、缓存键、进度、取消和退出码由
[QUALITY_PROCESS_PROTOCOL.md](QUALITY_PROCESS_PROTOCOL.md) 单独定义。生命周期当前为
`protocolVersion: 1`，schema 为
[quality-process-v1.schema.json](quality-process-v1.schema.json)；它与质量 payload v5
和指标算法版本独立演进。

`--backend auto` 是 CLI 默认值。当前 main 构建只启用 CPU/SIMD quality backend：
`auto` 会在 `backendDiagnostic` 中记录 GPU backend 未编译并确定性回退 CPU；
显式 `--backend wgpu` fail closed，不静默回退。这个选项和 schema 中的 GPU timing
字段只作为后续独立 compute backend 的兼容边界，不代表播放器恢复或依赖 WGPU
presentation。

CPU 路径保留 scalar 实现作为 SIMD parity oracle 和所有格式的 fallback；x86/x64
构建会额外编译独立 AVX2 translation unit，并在运行时检查 CPU/OS
能力后分发。当前 SIMD kernels 覆盖 packed 8-bit、9–16 bit 和 P010 luma 的
blockiness、banding、blur 和 noise，支持 FFmpeg 常见的 padded positive stride；
交错/偏移采样和非 x86 架构仍走 scalar。整个 CLI 不使用全局 `-mavx2`，因此旧 x86 server 不会因非法指令
启动失败；`--cpu-mode scalar` 可强制基准路径，`backendDiagnostic` 会记录实际 dispatch。

CPU analyzer 使用长期 worker pool。解码线程按显示顺序计算很轻的 temporal signature；
每个抽样帧只拷贝一次可见 luma，再把四个空间指标作为独立任务投递。队列允许多个帧
同时在途，完成后稳定按 PTS 归并，因此可以同时利用 FFmpeg frame/slice decode threading
和指标线程。`--decode-threads`、`--cpu-workers`、`--cpu-in-flight` 可显式调优；
未指定时 decode 使用 FFmpeg auto、worker 使用全部逻辑 CPU，在途帧数按 worker 数
自动推导并限制为最多 64 帧。高核数 server 可按 workload 在 decode 与 metric 间分配
核心，例如 128 逻辑核先从 `--decode-threads 32 --cpu-workers 96 --cpu-in-flight 48`
开始 A/B。队列有界，内存不会随视频长度增长。

Linux CPU server 构建不依赖 Rust/wgpu，使用系统 FFmpeg development package 的
`pkg-config` metadata：

```bash
# Ubuntu/Debian build dependencies
sudo apt-get install cmake g++ pkg-config \
  libavcodec-dev libavformat-dev libavutil-dev libswresample-dev

python3 native/build.py --cpu-server
build/native/standalone/portable/VoidPlayerCli \
  score-quality --input input.mp4 --backend cpu --json
```

也可以设置 `FFMPEG_ROOT` 使用自带的 FFmpeg SDK。所有平台的 `--backend auto`
都会明确记录 wgpu 未编译并回退 CPU；显式 `--backend wgpu` 仍 fail closed。

未来如果增加 GPU quality compute，应作为独立、可复用的 analysis session 接在
quality backend seam 后面，并沿用 main 的平台设备边界；不能恢复已移除的 WGPU
播放器合成路径。共享 decode session 已复用 Windows D3D11VA 与 macOS
VideoToolbox provider 的 capability seam；本版 quality CLI 仍请求软件输出，以锁定
CPU luma 的算法 parity。硬解 hwdownload 或 decoded surface 到 compute texture 的
zero-copy 输入仍是后续优化，不能把“共用解码器”误报成已经消除 GPU/CPU 拷贝。

该入口目前仅用于离线基准和指标校准，尚未写入 VAC2/VACHUNK、FFI 或 GUI。

Overlay heatmap renderer 成本可用独立 benchmark 覆盖：

```bash
python dev.py analysis-overlay-benchmark --iterations 240 --with-grid
```

该命令会生成一个临时 VAC2 base 和 overlay VACHUNK，然后调用
`VoidPlayerCli benchmark-overlay` 对指定帧重复栅格化。Windows 默认额外调用
`VoidPlayerCli benchmark-overlay-gpu` 量化 D3D11 overlay pass；macOS 当前只跑
CPU raster / cache toolchain 部分，报告写入
`build/analysis-overlay-benchmark/analysis_overlay_benchmark.json` 和 `.md`。
它用于检查 dirty frame 下 CU/MB 热力图填充和边界 mask 路径的回归；raster 内核会跳过
完整覆盖热力图的 BGRA 清屏，并用固定 LUT 计算 QP / bit-density 颜色。GUI 当前对热力图
和 prediction-mode 填充使用 16-byte packed rect structured buffer + D3D11 instanced quad
pass，CU/MB 反色边界用同一 rect buffer 在 GPU 侧生成 R8 mask render target。该
benchmark 不包含最终窗口合成，但报告会同时给出 CPU texture upload、当前 GPU
rect upload 字节数估算，以及 rect upload CPU wall time、color pass、GPU mask pass、
invert pass、full overlay pass 的 timestamp query 平均耗时。GUI 平滑 pan/zoom/resize
会复用已上传的 overlay materialization。
