# D3D11 后端

This document is Windows-specific. The shared renderer scheduler and
platform-backend boundary are documented in [ARCHITECTURE.md](ARCHITECTURE.md),
[THREADING_MODEL.md](THREADING_MODEL.md), and [DATA_PIPELINE.md](DATA_PIPELINE.md).

## D3D11Device

头文件: `windows/d3d11/device.h`

封装 `ID3D11Device`、ImmediateContext，以及窗口模式下的可选 SwapChain。

```cpp
bool initialize(void* hwnd, int width, int height);
bool initialize_headless(IDXGIAdapter* adapter, int width, int height);
void shutdown();
void resize(int width, int height);
void present(int sync_interval = 1);
```

当前 Flutter 主窗口走 headless 模式，native demo/standalone 可使用窗口模式。
窗口模式只作为 Windows native demo/dev 路径保留，swap chain 使用双缓冲
`DXGI_SWAP_EFFECT_FLIP_DISCARD`。
headless 模式必须传入 Flutter view 暴露的 DXGI adapter；native 不再在
adapter 缺失或创建失败时 fallback 到自建 D3D11 device，因为这种 device
无法保证与 Flutter texture registrar 共享 DXGI handle。

D3D11 后端的最低 feature level 是 `D3D_FEATURE_LEVEL_11_0`。渲染 shader
固定编译为 `vs_5_0` / `ps_5_0`，headless shared texture 和 D3D11VA
decode provider 也按 11_0+ 作为后端契约；低于该等级的 adapter 会初始化失败。

## Headless shared texture

`D3D11HeadlessOutput` 在 headless 模式下创建三缓冲 BGRA shared texture：

```
draw back buffer -> swap front index -> Flutter opens shared handle -> Texture widget displays
```

设计目标：

- 避免 renderer 覆盖 Flutter 正在读取的 buffer。
- Flutter texture lease release callback 驱动 buffer 重新可用。
- `capture_front_buffer()` 可以把当前 front buffer 读回 BGRA，用于 UI 自动化截图/hash。

Renderer 只负责在持有 device/texture mutex 后调用 `begin_frame_locked()`、绘制、`publish_frame_locked()`；shared handle、GPU fence、resize pending buffers 和 capture 逻辑都收敛在 `D3D11HeadlessOutput`。

`D3D11HeadlessOutput` 中带 `_locked` 后缀的 public 方法都要求调用方已经持有 `texture_mutex()`。当前锁顺序固定为 `device_mutex -> texture_mutex`。`Renderer::acquire_shared_texture()` 和 `Renderer::capture_front_buffer()` 是对外安全入口，会短暂持有 texture mutex。

## 纹理路径

`D3D11FramePresenter` 负责把 `TextureFrame` 准备成 shader 可采样资源，并持有每轨的 BGRA upload texture、NV12/P010 renderer-owned texture、Y/UV SRV 等缓存。Renderer 的 draw 阶段只消费准备好的 SRV 和 metadata。

### BGRA 上传路径

当前主要保留给 fallback package、旧测试或直接 BGRA texture 输入：

```
BGRA CPU buffer -> UpdateSubresource -> B8G8R8A8_UNORM texture -> shader sample
```

### NV12/P010 硬解路径

H.264/H.265 等 renderer-owned surface 路径：

```
D3D11VA texture array slice
  -> CopySubresourceRegion 到 renderer-owned NV12/P010 texture
  -> 创建 Y plane / UV plane SRV
  -> shader YUV->RGB
```

这里不是直接长期持有 decoder surface。复制一次 slice 能让 FFmpeg decode pool 在 seek/recreate 后安全复用 surface，避免跨线程/跨生命周期引用。

当前 renderer-owned direct path 只支持 `DXGI_FORMAT_NV12`、`DXGI_FORMAT_P010`、`DXGI_FORMAT_P016`。`Y210/Y216/Y410/Y416/AYUV` 等 4:2:2 / 4:4:4 硬件 surface 不能按 NV12/P010 采样；这类流在 shader path 补齐前应走软件解码。

## ShaderManager

头文件: `windows/d3d11/shader.h`

HLSL shader 内嵌到构建产物，运行时编译并绑定：

- BGRA 纹理采样
- NV12/P010 Y/UV 双平面采样
- 单轨/双轨/四宫格布局
- 宽高比和 letterbox

## D3D11VA device 策略

| 路径 | Device/context 策略 |
|------|---------------------|
| H.264/H.265 renderer-owned NV12/P010 | `DecodeDeviceMode::IndependentDevice`，使用独立 decode device，surface 带 `DECODER|SHADER_RESOURCE|MISC_SHARED` |
| AV1/VP9 hwdownload | `DecodeDeviceMode::FfmpegOwnedHwDownloadDevice`，让 FFmpeg 创建 D3D11VA device/context，匹配 CLI hwaccel 行为 |
| 诊断/实验 | `DecodeDeviceMode::SharedRenderDevice`，显式传入 render device；默认路径禁止依赖“传 nullptr”语义 |

D3D11 immediate context 必须串行化。decode provider 会设置 lock/unlock callback，renderer 侧也用 device mutex 保护 draw/copy/flush。

## Present

窗口模式调用 `IDXGISwapChain::Present(sync_interval, 0)`；headless 模式不调用 SwapChain Present，而是绘制到 shared texture 并触发 Flutter texture callback。

产品上屏模式、诊断合同和 HDR/DComp 后续阶段见
[WINDOWS_PRESENTATION_BACKEND.md](WINDOWS_PRESENTATION_BACKEND.md)。
