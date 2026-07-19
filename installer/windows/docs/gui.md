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
| `--deep-link <uri>` | 接收 `voidplayer://` 协议链接，通常由系统协议注册自动传入。 |

### Loop Range 示例

```text
void_player.exe --loop-range=1.5s:4s
void_player.exe --loop-range=1500ms:4000ms
void_player.exe --loop-range-us=1500000:4000000
```

`--loop-range` 会在首次加载媒体后自动启用 loop range、暂停播放，并 seek 到 range 起点。

## voidplayer:// 协议

安装包会为当前用户注册 `voidplayer://` 协议。网页或其他程序可以通过该协议拉起 VoidPlayer，即使 VoidPlayer 当前没有运行。

```text
voidplayer://v1/open?loopRange=1.5s:4s
voidplayer://v1/open?loopStart=1.5s&loopEnd=4s
```

当前协议只开放 loop range 参数：

| 参数 | 说明 |
| --- | --- |
| `loopRange=<start>:<end>` | 启动后首次加载媒体时启用 loop range。无单位数值按秒解析，也支持 `s`、`ms`、`us` 后缀。 |
| `loopStart=<time>` / `loopEnd=<time>` | 与 `loopRange` 等价，拆分传递起止时间。 |

开发/测试参数如 `--test-script`、`--silent-ui-test` 不会通过
`voidplayer://` 暴露。

## 安装与卸载数据

默认安装目录是当前用户的 `%LOCALAPPDATA%\Programs\VoidPlayer`。运行时配置、日志、分析缓存默认写入 `%APPDATA%\VoidPlayer`；如果系统没有 `APPDATA`，会回退到 `%LOCALAPPDATA%\VoidPlayer`。native crash symbol cache 也可能写入 `%LOCALAPPDATA%\VoidPlayer`。

卸载程序会删除安装目录内历史遗留的 `logs/`、`cache/`、`config.json`，并清理 `%APPDATA%\VoidPlayer` 与 `%LOCALAPPDATA%\VoidPlayer` 下的运行时数据。

## 命令行工具

发布包会在 `void_player.exe` 旁放置 `VoidPlayerCli.exe`，用于只读检查 VAC2/VACHUNK 分析缓存。用法见 [cli.md](cli.md)。
