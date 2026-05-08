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

## First Version

- toolbar analysis hover panel 中每个 track row 提供一个小图标激活按钮。
- 只有对应 track 的分析缓存完整时按钮可用。
- 点击按钮加载该 track 的 VAC，并设置 native overlay flags。
- overlay 默认打开 CU grid、prediction mode、QP heatmap；后续再拆成独立开关。
- 同一时间只激活一个 track 的 overlay。

渲染层落地顺序：

1. CU/MB grid：按当前 track geometry 将 block rect 映射到 viewport。
2. QP heatmap：以 block rect 半透明填充，使用 frame `qp_min/qp_max` 或 codec 范围归一化。
3. Prediction mode：用小 glyph/色彩区分 intra/inter/skip/merge。
4. Prediction lines：inter 画 L0/L1 MV，intra 按 intra mode 画方向线。

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

### Phase 1: Native Overlay MVP

- Add overlay state model with active track/hash and mode flags.
- Connect renderer to analysis manager read-only snapshot.
- Draw CU/MB grid and QP heatmap in D3D11.
- Add screenshot regression.

### Phase 2: Interaction

- Add native hit-test API.
- Add Dart hover/click forwarding from `ViewportPanel`.
- Add inspector UI and selected-block highlight.
- Add tests for coordinate mapping under pan/zoom/split.

### Phase 3: Prediction Details

- Draw pred mode glyphs and MV/intra prediction lines.
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
