# D3D11 后端

## D3D11Device

头文件: `d3d11/device.h`

封装 `ID3D11Device`、ImmediateContext，以及窗口模式下的可选 SwapChain。

```cpp
bool initialize(void* hwnd, int width, int height);
bool initialize_headless(IDXGIAdapter* adapter, int width, int height);
void shutdown();
void resize(int width, int height);
void present(int sync_interval = 1);
```

当前 Flutter 主窗口走 headless 模式，native demo/standalone 可使用窗口模式。

## Headless shared texture

`D3D11HeadlessOutput` 在 headless 模式下创建三缓冲 BGRA shared texture：

```
draw back buffer -> swap front index -> Flutter opens shared handle -> Texture widget displays
```

设计目标：

- 避免 renderer 覆盖 Flutter 正在读取的 buffer。
- resize 时旧 buffers 会按 `kPendingBufferRetireDelay` 延迟保活，降低 Flutter texture callback 仍在读取旧 handle 时的黑闪风险。
- `capture_front_buffer()` 可以把当前 front buffer 读回 BGRA，用于 UI 自动化截图/hash。

Renderer 只负责在持有 device/texture mutex 后调用 `begin_frame_locked()`、绘制、`publish_frame_locked()`；shared handle、GPU fence、resize pending buffers 和 capture 逻辑都收敛在 `D3D11HeadlessOutput`。

`D3D11HeadlessOutput` 中带 `_locked` 后缀的 public 方法都要求调用方已经持有 `texture_mutex()`。当前锁顺序固定为 `device_mutex -> texture_mutex`。`Renderer::shared_texture()`、`Renderer::shared_texture_handle()` 和 `Renderer::capture_front_buffer()` 是对外安全入口，会短暂持有 texture mutex。延迟释放旧 buffers 只是 best-effort 保活，不是严格的 Flutter handle ack 协议。

## 纹理路径

`D3D11FramePresenter` 负责把 `TextureFrame` 准备成 shader 可采样资源，并持有每轨的 RGBA upload texture、NV12 renderer-owned texture、Y/UV SRV 等缓存。Renderer 的 draw 阶段只消费准备好的 SRV 和 metadata。

### RGBA 上传路径

来源包括软件解码和 AV1/VP9 硬解 hwdownload：

```
RGBA CPU buffer -> UpdateSubresource -> RGBA texture -> shader sample
```

### NV12 硬解路径

H.264/H.265 等 renderer-owned surface 路径：

```
D3D11VA texture array slice
  -> CopySubresourceRegion 到 renderer-owned NV12 texture
  -> 创建 Y plane / UV plane SRV
  -> shader NV12->RGB
```

这里不是直接长期持有 decoder surface。复制一次 slice 能让 FFmpeg decode pool 在 seek/recreate 后安全复用 surface，避免跨线程/跨生命周期引用。

## ShaderManager

头文件: `d3d11/shader.h`

HLSL shader 内嵌到构建产物，运行时编译并绑定：

- RGBA 纹理采样
- NV12 Y/UV 双平面采样
- 单轨/双轨/四宫格布局
- 宽高比和 letterbox

## D3D11VA device 策略

| 路径 | Device/context 策略 |
|------|---------------------|
| H.264/H.265 renderer-owned NV12 | `DecodeDeviceMode::IndependentDevice`，使用独立 decode device，surface 带 `DECODER|SHADER_RESOURCE|MISC_SHARED` |
| AV1/VP9 hwdownload | `DecodeDeviceMode::FfmpegOwnedHwDownloadDevice`，让 FFmpeg 创建 D3D11VA device/context，匹配 CLI hwaccel 行为 |
| 诊断/实验 | `DecodeDeviceMode::SharedRenderDevice`，显式传入 render device；默认路径禁止依赖“传 nullptr”语义 |

D3D11 immediate context 必须串行化。decode provider 会设置 lock/unlock callback，renderer 侧也用 device mutex 保护 draw/copy/flush。

## Present

窗口模式调用 `IDXGISwapChain::Present(sync_interval, 0)`；headless 模式不调用 SwapChain Present，而是绘制到 shared texture 并触发 Flutter texture callback。
