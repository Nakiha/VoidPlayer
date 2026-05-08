# AGENTS.md

## 项目概述

Windows 端 Flutter 播放器应用，使用 DX11 进行视频渲染，并通过 Flutter Texture widget 上屏。

- **前端 UI**: Flutter (Dart)，Material Design 控件
- **窗体效果**: `flutter_acrylic` Mica 背景效果，原生 Win32 窗口
- **主题适配**: 跟随 Windows 暗色/亮色主题，主题色读取 `HKCU\Software\Microsoft\Windows\DWM\AccentColor`
- **视频渲染**: C++ native 模块，基于 FFmpeg + D3D11 的多轨道视频渲染器
- **纹理桥接**: Flutter Texture widget + FFI 传递 DX11 纹理和输入事件
- **目标平台**: Windows only

## 开发脚本

一站式开发脚本是 `dev.py`。

```bash
# 构建
python dev.py build --native

# 运行
python dev.py launch
python dev.py launch --debug
python dev.py launch --log-level flutter=DEBUG,native=TRACE

# Native demo
python dev.py demo

# 测试
python dev.py test
python dev.py test --native-only
python dev.py ui-test ui_tests/smoke/basic.csv
```

## 硬约束

- `python dev.py build --native` 只构建独立 native 模块，不会重新编译 Flutter Windows runner。
- Flutter runner 会通过 `windows/runner/CMakeLists.txt` 直接编译 `native/` 下的 C++ 源文件进 `void_player.exe`。
- 修改 `native/` C++ 后，必须执行 `flutter build windows --release` 或 `python dev.py ui-test --build ...`，否则上屏测试仍可能运行旧代码。
- native 渲染路径不引入 `libswscale` / `libyuv` 作为通用 fallback；新增像素格式支持时应做确定性转换，并验证软解/硬解颜色一致性。
- 不要在一个轮次里堆无关改动。每轮完成后先测试，再单独提交。

## 验证矩阵

按本轮改动的影响面选择验证命令。多个影响面叠加时取并集。

| 改动类型 | 必跑验证 |
| --- | --- |
| native C++ 单元逻辑 | `python dev.py test --native-only` |
| native C++ 影响 Flutter runner / Texture / 渲染上屏 | `python dev.py test --native-only` -> `flutter build windows --release` -> 相关 `python dev.py ui-test ...` |
| Flutter UI / Action / 主窗口 coordinator / 播放控制 | 相关 `python dev.py ui-test ...`，不要只跑 `python dev.py test --flutter-only` |
| 窗口、布局、pan/zoom、split | `ui_tests/viewport/` 中相关脚本，加 smoke |
| timeline 点击、seek、step、loop | `ui_tests/timeline/` / `ui_tests/seek/` / `ui_tests/loop/` 中相关脚本 |
| track 修改、codec、分析窗口/IPC | `ui_tests/track/` / `ui_tests/codec/` / `ui_tests/analysis/` 中相关脚本 |

通用 smoke 首选：

```bash
python dev.py ui-test ui_tests/smoke/basic.csv
```

如果自动化脚本无法覆盖本轮风险，需要在最终说明里写清楚缺口，例如缺少哪个 Action、Assert 或启动参数。

## UI 自动化索引

- `ui_tests/smoke/`: 主窗口基础加载和通用回归
- `ui_tests/analysis/`: analysis 子窗体、IPC track 更新
- `ui_tests/timeline/`: 真实 timeline pointer/click 路径
- `ui_tests/seek/`: 直接 seek、step、rapid seek
- `ui_tests/loop/`: loop range
- `ui_tests/viewport/`: 窗口尺寸、pan/zoom、split 布局
- `ui_tests/track/`: 轨道级修改
- `ui_tests/codec/`: codec 上屏 smoke
- `ui_tests/local/`: 依赖个人绝对路径的非通用回归

## 日志排查

`dev.py ui-test` 默认把日志写到 `%APPDATA%\VoidPlayer\logs`。

- Flutter/Dart 日志形如 `void_player_main_<pid>_<date>.log`
- native C++ 日志形如 `native_main_<pid>.log`
- UI 自动化失败后，先看最近一对 Flutter/native 日志；Flutter 日志通常包含 `TestRunner FAIL`，native 日志通常包含 demux/decode/render/seek 细节。

常用 PowerShell：

```powershell
Get-ChildItem "$env:APPDATA\VoidPlayer\logs" -File |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 8 FullName,Length,LastWriteTime
```

## 模块文档

- **Flutter / Dart 层**: [lib/doc.md](lib/doc.md)
- **Windows 宿主层**: [windows/doc.md](windows/doc.md)
- **Native C++ 层**: [native/docs/ARCHITECTURE.md](native/docs/ARCHITECTURE.md)
- **Native 线程模型**: [native/docs/THREADING_MODEL.md](native/docs/THREADING_MODEL.md)
- **Native 解码管线**: [native/docs/DECODE_PIPELINE.md](native/docs/DECODE_PIPELINE.md)
- **Native 色彩管线**: [native/docs/COLOR_PIPELINE.md](native/docs/COLOR_PIPELINE.md)
