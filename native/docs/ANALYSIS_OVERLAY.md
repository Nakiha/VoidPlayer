# Analysis Overlay

本文档约束主窗口码流遮罩层的当前合同，覆盖 VACHUNK、native
渲染、Dart UI 和命中交互之间的边界。

## Goals

- 在播放 viewport 上叠加 codec block analysis 信息，当前覆盖 CU/MB 划分、QP 热力图、CU bit density 热力图、预测模式、预测线/MV 线。
- 遮罩必须跟随现有 layout：side-by-side、split screen、track order、zoom、pan、像素大小模式。
- 高密度元素由 native/D3D11 绘制，Dart 只负责入口、开关、hover/click 事件、inspector 和少量 UI chrome。
- 点击命中使用 native 的当前帧数据和布局几何，避免把大量 CU/PU/TU records 搬到 Dart。
- VACHUNK schema 服务可见遮罩；缺失的 PU/TU/VVC tool 细节必须显式表现为 unavailable capability。

## Non-Goals

- 当前 baseline 不追求 Elecard/StreamEye 全量工具层。
- 当前 baseline 不绘制真实 TU tree，因为当前 VACHUNK overlay 没有 TU geometry。
- 当前 baseline 不在 Dart `CustomPainter` 中绘制大量 block rect。
- 主窗口 overlay 不绑定到 analysis 子窗口；它通过主进程的 VAC2 base 和按需 VACHUNK chunk 独立工作。

## Current VACHUNK Coverage

当前足够支持：

- H.264: 16x16 MB 栅格、intra/inter、skip、merge/direct 风格标志、inter direction、QP、intra mode、ref index、MV。
- HEVC/VVC: CU `x/y/w/h/depth`、prediction mode、QP、CU/MB coded bit count、intra mode、skip/merge、inter direction、ref index、MV。
- VVC optional streams: 已存在的工具标志可作为 feature availability 暴露给 UI。

当前不足：

- 没有 TU geometry、transform split tree、transform skip、CBF 等字段。
- 没有 PU-level geometry；一个 CU 内多个 PU/MV 时只能做粗粒度展示。
- affine 仅有标志时无法画 control-point MV。

## Overlay UX

### Activation

- toolbar analysis hover panel 继续作为遮罩入口。
- 每个 track row 保持小图标激活按钮，不使用文字按钮。
- 只有该 track 的分析缓存完整时，激活按钮才可点击。
- 点击已激活 track 的小图标会关闭遮罩；点击另一个 cached track 会切换 active overlay track。

### Control Strip

- 遮罩打开后，在 media header 上方展开 overlay control strip。
- control strip 参与主窗口布局，占用底部媒体区高度，viewport 通过 `Expanded` 自然被向上挤压。
- control strip 横向分栏与 media header 的 track slot 对齐；当前 native 只支持一个 active analysis session，因此只有 active track 的分栏可交互，其他分栏显示为 disabled。
- 每个 active track 分栏是一行控件，从左到右：
  - overlay type：`CU/MB 划分`、`QP 热力图`、`编码开销热力图` 三选一。
  - additive layers：`预测模式`、`预测线` 两个独立激活按钮。
  - opacity：调节遮罩透明度。
  - sync：是否把当前面板设置同步到全部 overlay header。
- 功能区状态属于主窗口 overlay，不属于 analysis 子窗口；analysis 子窗口仍负责深度结构浏览。

## Overlay Types and Layers

### Overlay Types

| Type | Primary visual | VACHUNK dependency | Additive layers | First GUI state |
| --- | --- | --- | --- | --- |
| CU | CU/MB partition outlines | H.264 MB grid or HEVC/VVC CU geometry | prediction mode, prediction lines | Enabled |
| QP heatmap | Per-CU/MB QP fill | CU/MB QP | prediction mode, prediction lines | Enabled |
| Bit-cost heatmap | Per-CU/MB syntax bit density fill | `bit_count` in current overlay CU/MB records | prediction mode, prediction lines | Enabled |

### Additive Layers

| Layer | Meaning | Supported now | Notes |
| --- | --- | --- | --- |
| Prediction mode | Draw compact intra/inter/skip/merge labels or colors | Partially | Current VACHUNK has CU-level mode data; glyph rendering belongs in the native renderer. |
| Prediction lines | Draw intra direction lines and L0/L1 MV lines | Partially | Current data is CU-level; PU split and affine control-point precision are unavailable unless the stream provides them. |
| TU grid | Transform unit boundaries | No | Requires TU geometry stream. Do not expose as an active toggle until data exists. |
| PU grid | Prediction unit boundaries | No | Requires PU geometry stream. Current implementation can only show CU-level prediction. |

### Type Defaults

- CU defaults to partition outlines.
- QP heatmap defaults to QP fill plus partition outlines.
- Bit-cost heatmap defaults to coded syntax bit-density fill plus partition outlines.

The type controls the primary native renderer pass. Prediction mode and prediction line layers are independent flags sent in the same overlay state so renderer and UI state stay aligned.

## Architecture Boundaries

### Dart

Dart 负责：

- toolbar analysis hover panel 中展示 per-track cache 状态。
- 只在 cache 就绪时允许激活某个 track 的 overlay。
- 提供 overlay 模式开关、图例、hover/click inspector。
- 把 viewport 物理坐标、点击/hover 状态传给 native。
- 展示 native 返回的命中结果。

Dart 不负责：

- 遍历或绘制全量 CU/PU/TU。
- 维护与 native shader 重复的一套 viewport-to-video 几何。
- 持有大量 per-frame block records。

### Native Analysis

Native analysis 负责：

- 加载当前 overlay track 的 VAC2/VACHUNK。
- 按当前播放 PTS 找到对应 analysis frame。
- 解压和缓存当前/相邻帧的 block records。
- 构建轻量空间索引，用于 hit-test。
- 暴露 overlay state、hit result、feature availability 给 Dart。

### Native Renderer

Shared native renderer 负责：

- 从 VAC2/VACHUNK 和当前 `RendererDrawSnapshot` 构建平台无关的 overlay scene。
- 用稳定 generation/hash 表达 `track_file_id + frame_index + overlay state + video size`
  的 materialization 边界。
- 复用现有 layout state 和 track geometry，保证遮罩与视频像素对齐。
- 按最终显示尺度保持线框像素宽度稳定，避免高倍率下把源像素线条放大成粗块。
- 绘制 selected/hover 高亮。

Windows D3D11 backend 当前按 `track_file_id + frame_index + overlay state + video size`
缓存 renderer 侧 overlay materialization；只有实际上屏帧、overlay 类型/图层/透明度或
视频尺寸变化时才重新读取 VACHUNK。QP / bit-density / prediction-mode 填充不再生成
整张 BGRA dynamic texture，而是把每个 CU/MB 写成 16-byte packed rect instance，
上传到 D3D11 structured buffer，并由 instanced quad pass 在 GPU 上直接绘制。平滑
pan/zoom/resize 只更新已有 layout constants，不重新 raster 或上传整张 video-space
color texture。

CU/MB 反色线框也复用同一组 rect instances，在 GPU 侧写入 video-size R8 mask render
target。这个选择保留了共享边界的幂等绘制语义：同一条边只在 mask 中置位一次，再通过
fullscreen invert pass 合成，避免直接画线时双重叠加导致的点状闪烁。预测线/MV 线仍走
小量 BGRA color texture fallback。使用
`python dev.py analysis-overlay-benchmark --iterations 240` 可以独立测量 dirty-frame
CPU raster 成本、当前 GUI/DX11 路径的 estimated rect upload 字节数，以及
rect upload / color pass / GPU mask pass / invert pass / full overlay pass 的粒度耗时。
如果 GPU mask pass 成为瓶颈，优化应保持 rect/mask 语义，不能退回会产生双重叠加闪烁的普通 alpha line 绘制。

Windows native-compositor source-projection 路径还会在 DComp compositor
device 上保留一份 video-space overlay primitive buffer。只有 primitive
generation、track/file/尺寸、overlay mode 或输出 target class 变化时才重新
打包和上传；pan/zoom/split/order 只更新 projection constants。高刷新门禁要求
`windowsOverlayLayerReuseCount` 高于 dirty raster/upload 计数，并且
`windowsViewportRedrawDuringProjectionCount == 0`。

### macOS Overlay Layer Policy

macOS high-refresh viewport 不应把 overlay primitive rebuild/upload/raster 放在每次
display-link final composite 热路径里。当前 Metal immediate primitive path 可作为
兼容 fallback，但不能继续扩张为主路径；高倍率 pan/zoom 下它会让同一批 CU 线框随 layout tick
反复进入 `WgpuMetalPresentationBackend` 和 overlay composite pass，导致 command completion
超过 display budget。

当前合同：

- `overlayLayerRasterCount` 应主要随 frame/chunk/mode 变化增长，而不是随
  `layoutIntentCount` 增长。
- `overlayLayerReuseCount` 在 pan/zoom 期间明显高于 raster count。
- `overlayCompositeP95Us` 保持在 display-link final composite budget 内；overlay layer
  raster 可以低频，但 viewport 不能因为 overlay 进入 backpressure 风暴。
- `overlayWaitingForChunk`、overlay fallback reason 和 drop/budget 诊断必须可见，
  不能把 CPU fallback 或缺 chunk 伪装成健康 GPU path。

当前 VACHUNK CU/MB record 已带 `bit_count`，`cuBitCostHeatmap` 使用
`bit_count * 64 * 64 / (w * h)` 计算归一到 64x64 的 bit density，并用固定
log2 标尺绘制，避免不同帧/片源自适应导致颜色抖动。

## Hit-Test Contract

输入：

- viewport physical x/y。
- pointer event kind：hover、primary click、secondary click。
- 可选 modifier flags。

native 输出：

- `track_file_id` 或 track slot。
- `frame_index`、`poc`。
- `unit_type`：MB、CU、PU、TU。
- `unit_id`，仅在当前 materialized frame 内稳定。
- `x/y/w/h/depth/qp/pred_mode`。
- mode-specific fields：intra mode、skip、merge、inter direction、ref index、MV。
- optional parent id：PU/TU 存在时用于回到所属 CU。

命中策略：

- 优先命中最小面积 unit。
- hover 频率高，native 需要缓存最近一次 frame index 和空间索引。
- Dart 只显示返回结果，不自行重算 block ownership。

## VACHUNK Stream Rules

- 新增字段以 optional stream 增量加入，旧 reader 可跳过。
- 每个 stream 有 feature flag，或可从 stream directory 判断存在性。
- 新增 unit tree 使用独立 unit type，不重用 CU record 塞额外含义。
- 对 UI 需要的 geometry 直接存储或确定性推导，避免解码器私有状态泄漏到 UI。
