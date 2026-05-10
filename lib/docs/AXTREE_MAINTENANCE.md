# AXTree 维护

VoidPlayer 的 Windows UI 由 Flutter 自绘并通过 Windows accessibility bridge
导出 AXTree/UIA 结构。主窗口和 analysis 窗口都有大量 Texture、CustomPaint、
hover overlay、拖拽分割线和虚拟列表；这些区域不能直接把视觉细节全部暴露给
AXTree，也不能整块 `ExcludeSemantics`。

本项目的策略是：真实控件保留语义，自绘/视频/装饰层降采样为稳定区域节点。

## 目标结构

主窗口：

- `Main toolbar`：打开媒体、视图模式、信息、性能、analysis、设置等真实控件。
- `Video viewport`：DX11 Texture 上屏区，一个粗粒度 image 节点；分屏模式额外保留
  `Viewport split handle` slider 节点。
- `Playback timeline`：媒体头、播放控制、seek slider、循环区间、track rows。
- Track row：每条轨道一个稳定容器；文件名、音频、offset、移除按钮保留控件语义；
  track clip 的 CustomPaint 作为单个视觉区域。

Analysis 窗口：

- `Analysis window`：analysis 根区域。不要在 `MaterialApp.builder` 全局
  `ExcludeSemantics`。
- `Analysis toolbar`：PTS/DTS 顺序和图表 tab 保留真实控件语义。
- `Reference Pyramid` / `Frame Trend`：图表画布各自是一个粗粒度 visual/image 节点，
  chart scrollbar 作为 slider 节点。
- `NAL unit inspector`：NALU browser、detail、分割线组成的稳定区域。
- Workspace tabs/split panes：tab header 和 split pane 都保留区域边界。

## 编码规则

- 新增大块 UI 时，优先用 `AxTreeRegion` 给它一个稳定区域名。
- Texture、CustomPaint、hover tooltip、边框、阴影、选中底色、拖拽 hit area 等视觉层，
  用 `AxTreeVisualRegion` 或 `ExcludeSemantics` 收敛，不要生成细碎节点。
- 按钮、输入框、下拉、tab、switch、seek slider、range/scroll slider 必须保留语义。
- 自写 `GestureDetector`/`InkWell` 控件时，如果外层手写 `Semantics(onTap: ...)`，
  内层要设置 `excludeFromSemantics: true`，避免重复节点。
- hover/mouse move 只更新视觉状态；不要把每个 hover 坐标写进 semantics label/value。
- 拖拽值需要暴露给 AXTree 时，只暴露稳定、低频的 value，例如百分比或当前帧窗口。
- Tooltip 只作为视觉提示；主题里应继续使用 `excludeFromSemantics: true`，控件本身要有
  label。
- 不要为了降低噪声在窗口根、整页、整块功能区使用 `ExcludeSemantics`。只能屏蔽纯视觉子树。

## 常见改动落点

| 区域 | 文件 |
| --- | --- |
| AXTree helper | `lib/widgets/axtree_region.dart` |
| 主窗口区域边界 | `lib/windows/main/main_window_scaffold.dart` |
| Texture / 分屏 handle | `lib/widgets/viewport_panel.dart` |
| seek slider | `lib/widgets/timeline_slider.dart` |
| loop range | `lib/widgets/loop_range_bar.dart` |
| track row / clip visual | `lib/widgets/track_row.dart` |
| analysis app 根 | `lib/windows/analysis/analysis_window.dart` |
| analysis page 布局 | `lib/windows/analysis/page/analysis_page_view.dart` |
| analysis charts | `lib/windows/analysis/charts/` |
| NALU browser/detail | `lib/windows/analysis/widgets/analysis_nalu.dart` |
| workspace tabs/split | `lib/windows/analysis/workspace/` |

## 验证

AXTree 改动属于 Flutter UI / analysis UI 改动。至少运行：

```bash
flutter analyze
python dev.py ui-test --build ui_tests/smoke/basic.csv
```

如果改动 analysis 窗口结构，补跑 analysis 脚本，例如：

```bash
python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h265.csv
```

如果改动 timeline、seek、loop、viewport 分屏语义边界，同时跑对应目录下的 UI 脚本。
`dev.py ui-test` 会捕获 Flutter engine 输出中的 `accessibility_bridge.cc` /
`Failed to update ui::AXTree`，出现 AXTree 更新错误时直接判定失败。针对 media header
码流遮罩面板和 playback controls hover 的回归，优先运行：

```bash
python dev.py ui-test --build ui_tests/analysis/overlay_axtree_controls_hover.csv
```

如果某类 AXTree 风险还没有对应 UI 自动化动作，需要在最终说明里写明缺少的 Action /
Assert，并用截图/OCR/识图工具人工抽检窗口分割效果。
