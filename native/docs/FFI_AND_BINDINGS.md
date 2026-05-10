# FFI 与绑定

## C FFI

头文件: `video_renderer/exports/ffi_exports.h`

所有导出函数使用 `naki_vr_` 前缀，通过宏 `NAKI_VR_FFI_EXPORT` 控制导出。

### 句柄类型

```c
typedef void* naki_vr_player_t;  // 不透明句柄
```

句柄值只是不透明 token，不等同于 `NativePlayer*`。FFI 层内部会先把 token pin 成 handle state，再在 per-handle mutex 下调用 native player。`naki_vr_player_destroy()` 会先从 registry 移除 token，阻止新调用进入，然后等待已 pin 的调用结束并执行 shutdown。

同一 player 句柄上的 FFI 调用会被 native 层串行化；跨不同 player 的调用可以并行。`destroy` 与其他线程上的同句柄调用并发时，已经进入的调用会先完成，destroy 返回后该 token 失效；后续任何查询或操作都会返回默认失败值，并通过 `naki_vr_last_error()` 报告 `NAKI_VR_ERR_INVALID_ARGUMENT`。调用方仍应避免在 destroy 后继续复用旧 token。

### 配置结构

```c
#define NAKI_VR_ABI_VERSION 1u

typedef struct naki_vr_log_config_t {
    uint32_t size;          // sizeof(naki_vr_log_config_t)
    uint32_t abi_version;   // NAKI_VR_ABI_VERSION
    const char* pattern;        // 日志格式，默认 "[%Y-%m-%d %H:%M:%S.%e] [%l] %v"
    const char* file_path;      // 日志文件路径，空 = 无文件日志
    size_t max_file_size;       // 单文件大小上限，默认 5MB，0 = unlimited
    int max_files;              // 轮转文件数，默认 3，0 = no rotation
    int level;                  // NAKI_VR_LOG_*: 0=trace..6=off
} naki_vr_log_config_t;

typedef struct naki_vr_player_config_t {
    uint32_t size;              // sizeof(naki_vr_player_config_t)
    uint32_t abi_version;       // NAKI_VR_ABI_VERSION
    const char** video_paths;   // NULL 终止的文件路径数组
    int64_t hwnd;               // 窗口句柄
    int width, height;          // 初始尺寸
    int use_hardware_decode;    // 0=软解, 1=硬解
    naki_vr_log_config_t log_config;
} naki_vr_player_config_t;
```

所有跨 FFI 的 config/layout struct 必须先填 `size` 和 `abi_version`。FFI 层会校验 struct 大小、ABI 版本、log level、seek type、layout mode 和 pixel-size mode；失败时原有 bool/int API 仍返回失败值，详细原因通过 `naki_vr_last_error()` 查询。

### API 分类

| 分类 | 函数 |
|------|------|
| ABI / 错误 | abi_version / last_error |
| 生命周期 | create / destroy / initialize / shutdown |
| 播放控制 | play / pause / resume / seek / seek_typed / set_speed |
| 逐帧 | step_forward / step_backward |
| 查询 | is_playing / is_initialized / current_pts_us / current_speed / track_count / duration_us |
| 日志 | configure_logging |
| Windows 崩溃诊断 | install_crash_handler / remove_crash_handler |

### Status / last error

```c
typedef enum naki_vr_status_t {
    NAKI_VR_OK = 0,
    NAKI_VR_ERR_INVALID_ARGUMENT = 1,
    NAKI_VR_ERR_NOT_INITIALIZED = 2,
    NAKI_VR_ERR_OPEN_FAILED = 3,
    NAKI_VR_ERR_INTERNAL = 1000,
} naki_vr_status_t;
```

`naki_vr_last_error(player, buf, cap)` 返回最近一次 FFI 调用的 status，并在 `buf` 非空时复制一段诊断文本。当前实现使用线程本地 last-error 状态，`player` 参数保留给后续 per-player 错误状态。

因为 last-error 是 thread-local，必须在产生失败的同一线程读取；其他线程读取到的是该线程自己的最近 FFI 状态。

### Seek 类型常量

```c
#define NAKI_VR_SEEK_KEYFRAME  0
#define NAKI_VR_SEEK_EXACT     1
```

---

## Python 绑定

文件: `video_renderer/exports/bindings.cpp`

使用 pybind11 绑定，导出类：

| Python 类 | C++ 对应 |
|-----------|---------|
| `NativePlayer` | `vr::NativePlayer` |
| `NativePlayerConfig` | `vr::NativePlayerConfig` |
| `Renderer` | `vr::Renderer` |
| `RendererConfig` | `vr::RendererConfig` |
| `LogConfig` | `vr::LogConfig` |
| `SeekType` 枚举 | `Keyframe` / `Exact` |

### 独立函数

```python
configure_logging(LogConfig)  # preserves host spdlog sinks; native file sink supports rotation
install_crash_handler(str)    # crash_dir; explicit opt-in process-global VEH/SEH/CRT/DbgHelp hooks
```

`video_renderer_native` 是 demo/dev tooling binding，不是稳定公开 ABI；稳定跨语言入口仍是 C FFI。

可能阻塞的 Python 调用会释放 GIL，包括 `Renderer` / `NativePlayer` 的 `initialize()`、`shutdown()`、`seek()`、`add_track()`、`remove_track()`、`set_speed()`、`set_track_offset()` 和 `apply_layout()`。这些调用可能打开文件、等待 render/decode 线程、触碰 GPU/FFmpeg 或等待内部锁，不应阻塞其他 Python 线程。

Python `LayoutState` 使用和 C FFI 相同的核心校验：layout mode、pixel-size mode、finite split/zoom/offset、positive zoom。非法 layout 在进入 native renderer 前抛 `ValueError`。`view_offset` 必须是 2 个 float，`order` 必须是 4 个 file_id；`Renderer.apply_layout()` 会把 file_id order 翻译成内部 slot order。

`LogConfig` 的默认行为是 library-safe：不会改 Windows console code page，不读取通用 `SPDLOG_LEVEL`，也不会启动/修改 spdlog 的 process-global flush 策略。VoidPlayer Windows runner 会在 app 层显式打开 `configure_console_codepage`、`use_environment_level_override` 和 `manage_global_flush`。

Crash handler 的实现位于 `common/windows_crash_handler.*`。它不是 renderer 生命周期的一部分；宿主进程需要显式 opt-in，并且要意识到这些 Windows hooks 会影响整个进程。VoidPlayer Windows runner 使用 `WindowsCrashHandlerConfig` 显式选择 SEH/VEH/CRT hooks；可复用库路径不在 renderer 初始化时安装 hooks。

---

## 构建输出

| 目标 | 输出路径 | 说明 |
|------|---------|------|
| video_renderer_ffi | `native/build-msvc/dist/ffi/` | DLL + 头文件 |
| video_renderer_native | `native/build-msvc/dist/python/` | .pyd + FFmpeg DLLs |

C FFI 消费者需链接 `video_renderer_ffi.dll` 并包含 `ffi_exports.h`。
Python 消费者 `import video_renderer_native` 即可。
