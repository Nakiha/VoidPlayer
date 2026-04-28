# UI 自动化测试指南

> Flutter UI 改动默认需要 GUI 闭环验证。本文档说明什么时候跑哪些脚本，以及什么时候应补 Action/Assert。

## 入口

纯 Dart / Flutter 单元测试：

```bash
python dev.py test --flutter-only
```

这会执行：

```bash
flutter test test/unit
```

完整测试入口：

```bash
python dev.py test
```

`dev.py test` 默认先跑 Flutter 单元测试，再构建并执行 native standalone tests。

推荐统一使用 `dev.py`：

```bash
python dev.py ui-test ui_tests/smoke/basic.csv
```

`ui-test` 会启动 app，并通过 `--test-script <csv>` 让 `TestRunner` 在主窗口启动后执行脚本。命令可以一次传多个 CSV，`dev.py` 会按传参顺序逐个启动 app 验证，任意一个失败就停止。

```bash
python dev.py ui-test ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h265.csv
```

## 测试目录约定

```text
test/
└── unit/               # 不启动真实窗口的 Dart/Flutter 单元测试

ui_tests/               # 启动真实 app 的 CSV GUI 自动化脚本
├── smoke/              # 快速主窗口 sanity check
├── analysis/           # analysis spawn / 子窗体 / IPC
├── timeline/           # timeline 真实 pointer/click 路径
├── seek/               # 直接 seek / step / rapid seek
├── loop/               # loop range 行为
├── viewport/           # resize / maximize / pan / zoom / split layout
├── track/              # 轨道级刷新和 offset
├── codec/              # codec 解码和非黑屏 smoke
└── local/              # 依赖个人绝对路径的非通用脚本
```

适合放进 `test/unit/` 的内容：

- 参数解析、纯函数、数据模型、payload 校验
- 不依赖真实 Win32 窗口
- 不依赖 native renderer 实例
- 不需要真实 pointer/keyboard 路径

适合放进 `ui_tests/` 的内容：

- timeline / loop range / viewport 的真实交互路径
- renderer 上屏、截图/hash、窗口 resize
- Action + TestRunner 端到端行为

## CSV 脚本格式

```csv
# 注释
时间秒, 指令, 参数...
```

约定：

- 文件编码：UTF-8
- 时间单位：秒
- PTS 单位：微秒
- 空行和 `#` 开头的行会被忽略
- `QUIT, 0` 表示通过并退出

## 目录语义

`ui_tests/README.md` 是目录层面的入口说明。下一次选择脚本时，先看改动影响哪个目录，再从该目录里选最贴近的 CSV。

| 目录 | 什么时候看这里 |
|------|----------------|
| `smoke/` | 通用 UI 改动、先确认 app 能启动和基础播放路径。 |
| `analysis/` | `lib/windows/analysis/`、主窗体生成 analysis、spawn analysis 窗体、analysis IPC track 更新。 |
| `timeline/` | 用户真实点击/拖动 timeline，尤其是 timeline seek、重复点击、防崩溃。 |
| `seek/` | 直接 seek、step forward、rapid seek、seek 后位置稳定。 |
| `loop/` | loop range 开关、start/end handle、loop 尾帧稳定。 |
| `viewport/` | 窗口 resize/maximize、画面 pan/zoom、split screen 边界。 |
| `track/` | 轨道 offset、多轨刷新、轨道级状态变更。 |
| `codec/` | AV1/VP9/H.265 等 codec 上屏和非黑屏 smoke。 |
| `local/` | 依赖个人绝对路径或大型私有素材，只在本机复现特定问题时使用。 |

analysis 目录里有两类脚本：

- `spawn_*.csv` / `ipc_*.csv` 从主窗口执行，覆盖生成 analysis、spawn/reuse analysis workspace、track 更新同步。
- `child_*.csv` 在 analysis 子窗体内执行，是主窗口脚本通过 `SET_ANALYSIS_TEST_SCRIPT` 传进去的辅助脚本，不是普通主窗口 smoke。

## 常用入口

通用改动：

```bash
python dev.py ui-test ui_tests/smoke/basic.csv
```

timeline / seek / 播放控制：

```bash
python dev.py ui-test ui_tests/timeline/h265_timeline_click_visual_regression.csv
python dev.py ui-test ui_tests/seek/h265_seek_position_no_snapback.csv
```

loop range：

```bash
python dev.py ui-test ui_tests/loop/h265_loop_range_enable_regression.csv
python dev.py ui-test ui_tests/loop/h265_loop_range_end_change_frame_hash_regression.csv
```

track offset / 多轨刷新：

```bash
python dev.py ui-test ui_tests/track/h265_track_offset_refresh_visual_regression.csv
```

viewport / layout / resize：

```bash
python dev.py ui-test ui_tests/viewport/viewport_resize_center_regression.csv
python dev.py ui-test ui_tests/viewport/viewport_pan_layout_regression.csv
python dev.py ui-test ui_tests/viewport/split_screen_edges_regression.csv
```

analysis 相关：

```bash
python dev.py ui-test ui_tests/analysis/spawn_h265.csv
python dev.py ui-test ui_tests/analysis/ipc_track_updates.csv
```

## 什么时候必须补测试

以下改动不应只跑现有 smoke：

- 新增用户交互路径
- 修改 Action 绑定或 TestRunner 指令
- 修改 seek/timeline/loop range 行为
- 修改 viewport resize、pan、zoom、split
- 修改 track add/remove/reorder/offset
- 修改 startup options 影响主窗口初始状态
- 修复 GUI bug

如果现有脚本无法覆盖，应优先补：

1. `PlayerAction` 或 `PlayerAssert`
2. `TestRunner` 指令解析
3. `ui_tests/` 对应功能目录下的 CSV

只有当自动化暂时不可行时，最终说明必须写清楚缺口，例如缺少哪个 Action、Assert 或启动参数。

## 真实 UI 路径优先

同一个行为可能有直接调用路径和真实 UI 路径：

- `SEEK_TO`: 直接执行 seek action，适合验证 native seek 能力
- `CLICK_TIMELINE_FRACTION`: 向 timeline slider 派发 pointer down/up，适合验证真实点击路径
- `DRAG_LOOP_HANDLE`: 向 loop range handle 派发真实 pointer 事件

回归用户交互 bug 时，优先用真实 UI 路径。

## 新增脚本建议

脚本命名：

```text
<area>/<scenario>_regression.csv
```

示例：

```text
timeline/h265_timeline_double_click_guard.csv
viewport/viewport_resize_center_regression.csv
analysis/ipc_track_updates.csv
```

脚本内容建议：

- 固定媒体路径，优先使用 `resources/video/...`
- `resources/` 是只读 fixture 区，测试和工具不得把生成物写回这里
- 先 `ADD_MEDIA`，避免 `OPEN_FILE` 弹系统对话框
- 对视觉结果优先使用 capture/hash/assert
- 对交互副作用优先使用显式 Assert，而不是只等待
- 末尾用 `QUIT, 0`

## 提交流程

Flutter UI / Action / 窗口交互改动提交前：

```bash
flutter analyze
python dev.py test --flutter-only
python dev.py ui-test ui_tests/smoke/basic.csv
```

再按影响面补跑对应脚本。最终说明列出实际跑过的脚本；如果没跑某个应跑脚本，要说明原因。
