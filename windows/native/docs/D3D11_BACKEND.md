# D3D11 后端

## D3D11Device

头文件: `d3d11/device.h`

封装 ID3D11Device + ImmediateContext + SwapChain。

```cpp
class D3D11Device {
    bool initialize(void* hwnd, int width, int height);
    void shutdown();
    void resize(int width, int height);
    void present(int sync_interval = 1);

    ID3D11Device* device();
    ID3D11DeviceContext* context();
    IDXGISwapChain* swap_chain();
};
```

### 创建参数

- Debug 模式启用 D3D11 Debug Layer
- SwapChain 格式：DXGI_FORMAT_R8G8B8A8_UNORM
- 默认 VSync：sync_interval = 1

---

## 纹理管理

头文件: `d3d11/texture.h`

### 创建纹理

```cpp
ID3D11Texture2D* create_rgba_texture(device, width, height);
```

### 上传（软解路径）

```cpp
void upload_rgba_data(context, texture, data, row_pitch, height);
```

CPU RGBA 数据 → GPU 纹理，每帧一次 UpdateSubresource。

### 纹理池

软解时复用纹理避免每帧创建/销毁。FrameConverter 的 TextureFrame 复用已有纹理槽。

---

## ShaderManager

头文件: `d3d11/shader.h`

HLSL 着色器编译管理。

```cpp
class ShaderManager {
    bool compile_from_file(const char* hlsl_path);
    void bind(ID3D11DeviceContext* ctx);
};
```

---

## multitrack.hlsl

路径: `video_renderer/shaders/multitrack.hlsl`

### 功能

- 顶点着色器：全屏四边形
- 像素着色器：
  - RGBA 纹理采样（软解）
  - NV12 双平面采样 + BT.601 YUV→RGB（硬解零拷贝）
  - 1-4 轨道布局（单画面 / 左右 / 2×2）
  - 宽高比校正 + Letterbox

### NV12 零拷贝路径

```
D3D11VA 解码输出
  → ID3D11Texture2D (NV12, 纹理数组)
  → 创建 SRV (ShaderResourceView)
  → Shader 直接采样 Y 和 UV 平面
  → BT.601 转换为 RGB
  → 无 CPU 拷贝
```

### D3D11VA 纹理数组对齐

D3D11VA 输出为纹理数组，每帧位于不同 index：
- `texture_array_index` 指定帧在数组中的索引
- SRV 创建时需绑定正确的 array slice

### 布局模式

| 轨道数 | 布局 |
|--------|------|
| 1 | 全屏单画面 |
| 2 | 左右 1/2 |
| 3-4 | 2×2 网格 |

---

## Present

```cpp
device_.present(sync_interval);
// sync_interval = 1: VSync
// sync_interval = 0: 立即呈现
```

Render 线程独占 ImmediateContext，无需额外同步。
