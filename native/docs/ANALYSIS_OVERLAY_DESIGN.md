# Analysis Overlay Design and Roadmap

本文档约束主窗口码流遮罩层的长期设计，避免后续在 VBS4、native 渲染、Dart UI 和命中交互之间反复改边界。

## Goals

- 在播放 viewport 上叠加 codec block analysis 信息，第一版覆盖 CU/MB 划分、预测模式、预测线/MV 线、QP 热力图。
- 遮罩必须跟随现有 layout：side-by-side、split screen、track order、zoom、pan、像素大小模式。
- 高密度元素由 native/D3D11 绘制，Dart 只负责入口、开关、hover/click 事件、inspector 和少量 UI chrome。
- 点击命中使用 native 的当前帧数据和布局几何，避免把大量 CU/PU/TU records 搬到 Dart。
- VBS4 schema 先服务第一版可见遮罩，后续以 optional streams 和 feature flags 支持 PU/TU/VVC tool 细节。

## Non-Goals

- 第一版不追求 Elecard/StreamEye 全量工具层。
- 第一版不绘制真实 TU tree，因为当前 VBS4 没有 TU geometry。
- 第一版不在 Dart `CustomPainter` 中绘制大量 block rect。
- 第一版不把 overlay 状态绑定到 analysis 子窗口；主窗口 overlay 使用主进程已加载的 VAC。

## Current VBS4 Coverage

当前足够支持：

- H.264: 16x16 MB 栅格、intra/inter、skip、merge/direct 风格标志、inter direction、QP、intra mode、ref index、MV。
- HEVC/VVC: CU `x/y/log2_w/log2_h/depth`、prediction mode、QP、intra mode、skip/merge、inter direction、ref index、MV。
- VVC optional streams: MIP、ISP、IBC、PLT、affine、SBT 等工具标志可逐步扩展。

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
- control strip 横向分栏与 media header 的 track slot 对齐；第一版 native 只支持一个 active analysis session，因此只有 active track 的分栏可交互，其他分栏显示为 disabled。
- 每个 active track 分栏包含三类控件：
  - overlay type：切换主视觉层。
  - additive layers：切换可叠加的辅助层。
  - opacity：调节遮罩透明度。
- 功能区状态属于主窗口 overlay，不属于 analysis 子窗口；analysis 子窗口仍负责深度结构浏览。

## Overlay Types and Layers

### Overlay Types

| Type | Primary visual | VBS4 dependency | Additive layers | First GUI state |
| --- | --- | --- | --- | --- |
| CU | CU/MB partition outlines | H.264 MB grid or HEVC/VVC CU geometry | CU grid | Enabled |
| Prediction | Prediction mode glyph/color | CU pred mode, skip/merge/inter/intra flags | CU grid, prediction mode | Enabled |
| Prediction lines | Intra direction and inter MV lines | CU-level intra mode and MV; PU/affine details optional | CU grid, prediction mode, prediction lines | Enabled |
| QP heatmap | Per-CU/MB QP fill | CU/MB QP | CU grid, prediction mode | Enabled |
| CU bit-cost heatmap | Per-CU bit/cost fill | future cost/bit optional stream | CU grid, prediction mode, prediction lines | GUI/protocol enabled, visual data pending |

### Additive Layers

| Layer | Meaning | Supported now | Notes |
| --- | --- | --- | --- |
| CU grid | Draw CU/MB boundaries over any primary type | Yes | Base layer for pure CU and useful guide for heatmaps. |
| Prediction mode | Draw compact intra/inter/skip/merge labels or colors | Partially | Current VBS4 has CU-level mode data; glyph rendering lands in native renderer phase. |
| Prediction lines | Draw intra direction lines and L0/L1 MV lines | Partially | Current data is CU-level; PU split and affine control-point precision require future streams. |
| TU grid | Transform unit boundaries | No | Requires TU geometry stream. Do not expose as an active toggle until data exists. |
| PU grid | Prediction unit boundaries | No | Requires PU geometry stream. Current MVP can only show CU-level prediction. |

### Type Defaults

- CU defaults to `CU grid`.
- Prediction defaults to `CU grid + prediction mode`.
- Prediction lines defaults to `CU grid + prediction mode + prediction lines`.
- QP heatmap defaults to `CU grid`.
- CU bit-cost heatmap defaults to `CU grid` until cost data exists.

The type controls the primary native renderer pass. Additive layers are independent flags sent in the same overlay state so later renderer phases do not need UI rewiring.

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

- 加载当前 overlay track 的 VAC/VBS4。
- 按当前播放 PTS 找到对应 analysis frame。
- 解压和缓存当前/相邻帧的 block records。
- 构建轻量空间索引，用于 hit-test。
- 暴露 overlay state、hit result、feature availability 给 Dart。

### Native Renderer

D3D11 renderer 负责：

- 在视频 pass 后叠加 overlay pass。
- 复用现有 layout state 和 track geometry，保证遮罩与视频像素对齐。
- 按显示尺度做 LOD：小尺度下隐藏过细线框或合并热力图。
- 绘制 selected/hover 高亮。

第一版实现先采用 correctness-first 路径：CPU 按当前 VBS4 frame 生成一张 viewport 尺寸的 BGRA dynamic texture，再由 D3D11 full-screen overlay pass 做 alpha blend。这样可以先把 CU grid、QP heatmap、prediction colors、MV line 和 CU complexity proxy heatmap 与现有 Texture 上屏路径打通；后续大码率/高分辨率优化再迁移到 GPU instance/structured-buffer 绘制。

当前 VBS4 CU record 没有真实 bit-cost 字段，因此第一版 `cuBitCostHeatmap` 只能使用 CU depth/面积/QP 的复杂度 proxy 保持类型可见；真正的 CU bit-cost heatmap 必须等 VBS4 增加对应 stream 后替换。

## First Version

- toolbar analysis hover panel 中每个 track row 提供一个小图标激活按钮。
- 只有对应 track 的分析缓存完整时按钮可用。
- 点击按钮加载该 track 的 VAC，并设置 native overlay state。
- overlay 默认类型为 CU，附加层为 CU grid，透明度为 55%。
- 遮罩打开后 media header 上方出现 per-track overlay control strip。
- control strip 可切换 CU / prediction / prediction lines / QP heatmap / CU bit-cost heatmap，可切换附加层，可调整透明度。
- 同一时间只激活一个 track 的 overlay。

渲染层落地顺序：

1. CU/MB grid：按当前 track geometry 将 block rect 映射到 viewport。
2. QP heatmap：以 block rect 半透明填充，使用 frame `qp_min/qp_max` 或 codec 范围归一化。
3. Prediction mode：第一版用色彩区分 intra/inter/skip/merge，后续补小 glyph。
4. Prediction lines：第一版画基础 L0 MV，后续补 L1 MV 和 intra direction。
5. CU bit-cost heatmap：第一版用复杂度 proxy，后续接真实 bit-cost stream。

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
- optional parent id：PU/TU 后续用于回到所属 CU。

命中策略：

- 优先命中最小面积 unit。
- hover 频率高，native 需要缓存最近一次 frame index 和空间索引。
- Dart 只显示返回结果，不自行重算 block ownership。

## VBS4 Extension Rules

- 新增字段以 optional stream 增量加入，旧 reader 可跳过。
- 每个 stream 有 feature flag，或可从 stream directory 判断存在性。
- 新增 unit tree 使用独立 unit type，不重用 CU record 塞额外含义。
- 对 UI 需要的 geometry 直接存储或确定性推导，避免解码器私有状态泄漏到 UI。

建议的第二轮扩展：

- `PU1`: `parent_cu`, `x/y/log2_w/log2_h`, `inter_dir`, `ref_l0/l1`, `mv_l0/l1`, `merge/skip/affine`.
- `TU1`: `parent_cu`, `x/y/log2_w/log2_h`, `depth`, `transform_skip`, `cbf_y/u/v`.
- `AFF1`: affine control-point MVs.
- `TOOL1`: VVC SBT、ISP、MIP、IBC、PLT 等工具细分。

## Roadmap

### Phase 0: Activation Path

- Add design document.
- Add toolbar hover-panel activation button.
- Gate activation on complete cache.
- Load cached VAC and set overlay flags.
- Add media-header overlay control strip.
- Add overlay type/layer/opacity state protocol.

### Phase 1: Native Overlay MVP

- Add overlay state model with active track/hash and mode flags.
- Connect renderer to the loaded analysis manager and active track id.
- Draw CU/MB grid, QP heatmap, prediction-mode colors, and basic MV lines in D3D11.
- Trigger paused-frame redraw when overlay state changes.
- Add screenshot regression that proves overlay modes change viewport pixels.

### Phase 2: Interaction

- Add native hit-test API.
- Add Dart hover/click forwarding from `ViewportPanel`.
- Add inspector UI and selected-block highlight.
- Add tests for coordinate mapping under pan/zoom/split.

### Phase 3: Prediction Details

- Replace first-pass prediction colors/lines with pred mode glyphs and precise MV/intra direction rendering.
- Add per-layer toggles and legend.
- Add LOD rules for dense 4K/8K streams.

### Phase 4: VBS4 PU/TU

- Extend VBS4 with optional PU/TU streams.
- Update generators and parser tests.
- Add PU/TU overlay modes and hit-test hierarchy.

### Phase 5: Elecard-Class Polish

- Add VVC tool overlays.
- Add exportable overlay screenshots.
- Add per-frame comparison/dual-track analysis affordances.
