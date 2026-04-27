# VoidPlayer GUI

## 启动参数

主窗口支持这些启动参数：

```text
void_player.exe [options]
```

| 参数 | 说明 |
| --- | --- |
| `--log-level=flutter=DEBUG,native=TRACE,ffmpeg=INFO` | 覆盖日志级别。 |
| `--test-script <csv>` | 主窗口启动后执行 UI 自动化脚本。 |
| `--silent-ui-test` | UI 自动化时隐藏/不激活测试窗口，通常由 `dev.py ui-test` 注入。 |
| `--loop-range=<start>:<end>` | 启动后首次加载媒体时启用 loop range。无单位数值按秒解析，也支持 `s`、`ms`、`us` 后缀。 |
| `--loop-range-us=<startUs>:<endUs>` | 与 `--loop-range` 等价，但无单位数值按微秒解析，方便脚本和调试。 |

### Loop Range 示例

```text
void_player.exe --loop-range=1.5s:4s
void_player.exe --loop-range=1500ms:4000ms
void_player.exe --loop-range-us=1500000:4000000
```

`--loop-range` 会在首次加载媒体后自动启用 loop range、暂停播放，并 seek 到 range 起点。

## 内部窗口参数

下面这些参数主要供 VoidPlayer 自身启动独立分析窗口或 secondary window 时使用，普通用户一般不需要手动传入：

| 参数 | 说明 |
| --- | --- |
| `--standalone-analysis` | 启动独立分析窗口进程。 |
| `--hash=<hash>` | 要打开的分析缓存 hash，可重复传入。 |
| `--fileName=<name>` | 与 hash 对应的展示文件名，可重复传入。 |
| `--x=<px>` / `--y=<px>` | 初始窗口位置。 |
| `--width=<px>` / `--height=<px>` | 初始窗口大小。 |
| `--accentColor=<argbInt>` | 主窗口传递给子窗口的主题色。 |
| `--analysis-ipc-port=<port>` / `--analysis-ipc-token=<token>` | 分析窗口 IPC 连接参数。 |
| `multi_window <windowId> <json>` | `desktop_multi_window` 的 secondary window 路由参数。 |
