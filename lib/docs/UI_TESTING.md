# UI 自动化测试指南

> Flutter UI 改动默认需要 GUI 闭环验证。本文档说明什么时候跑哪些脚本，以及什么时候应补 Action/Assert。

## 入口

纯 Dart / Flutter 单元测试：

```bash
flutter test
```

或只跑某个目录：

```bash
flutter test test/unit
```

推荐统一使用 `dev.py`：

```bash
python dev.py ui-test ui_tests/smoke_basic.csv
```

`ui-test` 会启动 app，并通过 `--test-script <csv>` 让 `TestRunner` 在主窗口启动后执行脚本。

## 测试目录约定

```text
test/
└── unit/               # 不启动真实窗口的 Dart/Flutter 单元测试

ui_tests/               # 启动真实 app 的 CSV GUI 自动化脚本
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

## 选择脚本

通用改动：

```bash
python dev.py ui-test ui_tests/smoke_basic.csv
```

timeline / seek / 播放控制：

```bash
python dev.py ui-test ui_tests/h265_timeline_click_visual_regression.csv
python dev.py ui-test ui_tests/h265_seek_position_no_snapback.csv
```

loop range：

```bash
python dev.py ui-test ui_tests/h265_loop_range_enable_regression.csv
python dev.py ui-test ui_tests/h265_loop_range_end_change_frame_hash_regression.csv
```

track offset / 多轨刷新：

```bash
python dev.py ui-test ui_tests/h265_track_offset_refresh_visual_regression.csv
```

viewport / layout / resize：

```bash
python dev.py ui-test ui_tests/viewport_resize_center_regression.csv
python dev.py ui-test ui_tests/viewport_pan_layout_regression.csv
python dev.py ui-test ui_tests/split_screen_edges_regression.csv
```

analysis IPC 相关：

```bash
python dev.py ui-test ui_tests/analysis_ipc_track_updates.csv
python dev.py ui-test ui_tests/analysis_spawn_h265.csv
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
3. `ui_tests/*.csv`

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
<feature>_<scenario>_regression.csv
```

示例：

```text
h265_timeline_double_click_guard.csv
viewport_resize_center_regression.csv
analysis_ipc_track_updates.csv
```

脚本内容建议：

- 固定媒体路径，优先使用 `resources/video/...`
- 先 `ADD_MEDIA`，避免 `OPEN_FILE` 弹系统对话框
- 对视觉结果优先使用 capture/hash/assert
- 对交互副作用优先使用显式 Assert，而不是只等待
- 末尾用 `QUIT, 0`

## 提交流程

Flutter UI / Action / 窗口交互改动提交前：

```bash
flutter analyze
python dev.py ui-test ui_tests/smoke_basic.csv
```

再按影响面补跑对应脚本。最终说明列出实际跑过的脚本；如果没跑某个应跑脚本，要说明原因。
