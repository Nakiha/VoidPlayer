# 日志规范

VoidPlayer 的日志面向两类场景：默认 `INFO` 用于复盘一次用户操作和应用生命周期，
`DEBUG` / `TRACE` 用于定位状态机、时序和逐事件问题。日志不是性能计数器的默认输出。

## 等级约定

| 等级 | 用途 | 示例 |
| --- | --- | --- |
| `INFO` | 有界的用户操作、生命周期和能力选择；每个操作最多一条开始与一条结果 | 媒体打开、质量分析开始/完成、输出模式变化 |
| `DEBUG` | 状态转换、采样后的计数与耗时、内部阶段 | resize/layout、target ring、seek/preroll、线程启停 |
| `TRACE` | 每帧、每个输入消息或其他高频诊断 | Win32/Flutter 快捷键事件、viewport draw trace |
| `WARNING` | 操作仍可继续但已降级，或一个已处理的用户操作失败 | 缓存帧补充失败、fallback、分析协议失败 |
| `ERROR` / `SEVERE` | 不可恢复的子系统错误或未处理异常 | native draw 失败、未处理 Flutter/isolate 异常 |

默认级别必须保持可读：正常播放、鼠标滚动或按住修饰键不应持续产生 `INFO`。
诊断日志即使做了首 N 次或每 N 次采样，也仍属于 `DEBUG`；逐输入和逐帧信息属于
`TRACE`。

## 格式与分类

Dart 新代码使用 `appLogger('Component')` 获得命名 logger，消息采用稳定的事件和
`key=value` 字段：

```dart
final _qualityLogger = appLogger('QualityAnalysis');
_qualityLogger.info(
  'analysis completed operation=$id elapsed_ms=$elapsed samples=$samples',
);
```

Native 日志继续使用可搜索的 `[Component]` 前缀：

```cpp
spdlog::debug("[WindowsLayout] intent={} mode={}", serial, mode);
```

约束：

- 一次异步操作生成稳定的 `operation` / `request_id`，开始、完成、取消和失败沿用它。
- 记录结果、耗时、数量、选择的 backend/capability；不要重复输出整段对象。
- 默认不记录媒体绝对路径、用户输入正文、凭证或协议 payload；媒体仅记录 basename
  或内部 `file_id`。
- 捕获异常时保留 error 与 stack trace；不可用空 `catch` 吞掉影响能力的失败。
- UI 与底层 service 避免重复记录同一操作；底层记录执行结果，UI 只记录 UI 特有的
  降级或后续动作。

## 启用诊断

```bash
python dev.py launch --log-level flutter=DEBUG,native=DEBUG
python dev.py launch --log-level flutter=TRACE,native=TRACE
```

`flutter=DEBUG` 映射到 Dart `FINER`，`native=DEBUG` 映射到 spdlog `debug`；
`TRACE` 打开逐事件诊断。快捷键跨 Flutter/Win32 排查需要两侧同时设为 `TRACE`。

日志文件位于运行时数据根目录的 `logs/`：Dart 为
`void_player_<role>_<pid>_<date>.log`，native 为
`native_<role>_<pid>_<session>.log`。会话字段避免操作系统复用 PID 时把不同进程的
日志追加进同一文件。

## 评审检查

- 这是用户可感知操作、内部状态还是逐事件诊断？等级是否匹配？
- 正常播放一分钟会产生多少条？如果数量随帧、输入或 resize 增长，不能放在 INFO。
- 开始后是否一定有完成、取消或失败结果？是否带同一个 operation id？
- WARNING 是否真的代表降级/失败，而不是预期分支？ERROR 是否会被上层正常处理？
- 日志是否足以回答“哪个组件、什么事件、结果如何、耗时多久”，且没有敏感数据？
