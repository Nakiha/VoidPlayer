# HDR 三明治结构探索记录

这个 worktree 用来验证 Windows Flutter 播放器是否可以把原生 HDR
DirectComposition surface 放在 Flutter UI 下方，同时保留普通窗口外观和
Flutter 控件覆盖能力。

## 当前可工作的结构

当前可工作的 probe 启动方式：

```powershell
.\build\windows\x64\runner\Debug\void_player.exe --dcomp-alpha-probe
```

HDR/SDR 混合 probe 启动方式：

```powershell
.\build\windows\x64\runner\Debug\void_player.exe --dcomp-hdr-sdr-mix-probe
```

这个 probe 仍然使用 Flutter color-key 洞把 native surface 暴露出来，但洞内的
native DirectComposition tree 多了一层真正的 SDR alpha overlay：

```text
Native child HWND
  HDR visual: R16G16B16A16_FLOAT, scRGB, alpha IGNORE
  SDR overlay visual: B8G8R8A8_UNORM, premultiplied alpha
```

它用于验证 Windows/DWM 对 “SDR premultiplied-alpha visual over FP16 HDR
visual” 的合成表现。它不等价于 Flutter embedder 真 alpha，因为 SDR overlay
是 native D3D shader 画出来的，不是 Flutter 的 ANGLE/Skia surface。

窗口层级是：

1. 最外层 Win32 host HWND，保留 Mica / DWM backdrop。
2. 一个 native child HWND，里面挂 DirectComposition visual。
3. FlutterView child HWND，放在 native child HWND 上方。
4. Flutter UI 正常绘制在最上层。

关键做法：

- host HWND 不再设置 `WS_EX_LAYERED`，这样 Mica 背板可以保留。
- HDR surface 放在独立 child HWND 中，位于 FlutterView child HWND 下方。
- FlutterView child HWND 使用 `WS_EX_LAYERED + LWA_COLORKEY`。
- Flutter 在需要透出原生 surface 的区域绘制 `RGB(0, 255, 255)`。
- Win32 color key 把这个 cyan 颜色二值抠除，露出下方 DComp surface。
- 除视频洞之外的 client 区应由 Flutter 自己绘制不透明/近似不透明的正常控件底色，
  不应该大面积透出 Mica。Mica 主要保留给标题栏/窗口边框视觉。
- 原生 probe 使用 FP16 swapchain：`DXGI_FORMAT_R16G16B16A16_FLOAT`。
- swapchain 设置为 scRGB：`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`。

一次成功日志的核心信息如下：

```text
ColorSpace=RGB_FULL_G2084_NONE_P2020(PQ)
MaxLuminance=417.712
CreateSwapChainForComposition format=R16G16B16A16_FLOAT
CheckColorSpaceSupport RGB_FULL_G10_NONE_P709(scRGB) support=0x00000003
SetColorSpace1 RGB_FULL_G10_NONE_P709(scRGB) hr=0x00000000
```

这说明当前 Windows 输出处于 HDR/PQ 模式，DComp swapchain 是 FP16，
并且 scRGB presentation 设置成功。

## 已验证可行的点

- 原生 DComp HDR 内容可以在 Flutter app 窗口内合成。
- Flutter 控件可以盖在 native surface 上方。
- 窗口移动和缩放时，native child HWND 与 FlutterView child HWND 同属一个
  parent HWND，视觉上不会明显脱节。
- Mica 可以和当前 color-key sandwich 同时存在，前提是 host HWND 不做
  layered/color-key。
- 普通截图软件和 `PrtSc` 对 DWM/Mica/HDR 组合的捕获结果可能不同。实测如果
  client 区大面积透出 Mica，有些截图路径会把 Flutter 覆盖区域捕成白色；
  把 client 区改为 Flutter 实色面板，只在视频洞使用 key 色后，截图结果更稳定。
- 不同截图工具对 HDR 内容的结果可能不同。有些路径会 clamp 或 tone map，
  所以截图不能作为唯一 HDR 证据，应结合 DXGI/DComp 日志判断。

## 当前限制

当前可工作的方案依赖 Win32 color key。这个透明是二值抠像，不是真正的
per-pixel alpha。

实际影响：

- 硬矩形视频洞可以工作。
- 洞的边缘不能做半透明羽化或抗锯齿 alpha。
- Flutter UI 自身仍然可以正常抗锯齿；限制只发生在被 color key 抠除的区域。
- 半透明 Flutter 控件如果直接画在 cyan key 背景上，圆角边缘可能先和 cyan 混合，
  这些混合像素不会被 color key 抠除，可能出现边缘脏色或锯齿。
- key 色必须当作保护色处理，普通 UI 和视频内容外的 Flutter 背景都不要使用
  `RGB(0, 255, 255)`。

播放器 UI 上可以把 HDR 视频区当作硬矩形 underlay 处理，避免在视频洞边界依赖
柔和透明边缘。

Flutter 控件建议避免直接画在 key 色背景上。最终 probe 采用的做法是：
整个 client 区先画 Flutter 深色面板，只在视频矩形区域画 cyan key 色。
这样 Flutter 控件的圆角、阴影、文字抗锯齿都和正常面板色混合，不会和 key 色
混合产生 fringe。代价是 Mica 不再大面积透出 client 区，但这符合播放器需求：
只需要标题栏/边框有 Mica 质感，主体区域由播放器 UI 自己负责。

## true alpha 尝试结论

我们试过几种 true alpha 路径，都没有得到完整三明治效果。

1. Flutter 透明背景，不使用 `LWA_COLORKEY`
   - Mica 和 native DComp surface 能露出。
   - 但 Flutter UI 进入 DComp 矩形后会被 native surface 吃掉。

2. DComp target 挂 host HWND，而不是单独 child HWND
   - 结果类似：native DComp visual 在重叠区域仍然压过 Flutter UI。

3. `WS_EX_LAYERED + LWA_ALPHA`
   - Flutter overlay 回来了。
   - 但 Flutter 透明区域变成黑色/不透明，Mica 和 HDR surface 都被挡住。

4. DComp child HWND 使用 `WS_EX_NOREDIRECTIONBITMAP`
   - 根据 topmost 设置不同，要么 HDR surface 不见，要么 Flutter UI 仍然被压住。

5. `CreateSurfaceFromHwnd(flutter_hwnd)` 终极尝试
   - 新增 probe 启动方式：

     ```powershell
     .\build\windows\x64\runner\Debug\void_player.exe --dcomp-surface-probe
     ```

   - 结构改为 host HWND 上挂一个 topmost DirectComposition target：

     ```text
     Host HWND with Mica
       DirectComposition visual tree
         HDR FP16/scRGB swapchain visual
         Flutter child HWND surface visual from CreateSurfaceFromHwnd
     ```

   - FlutterView child HWND 设置 `WS_EX_LAYERED + LWA_ALPHA`，不再使用 color key。
   - DComp 侧调用 `CreateSurfaceFromHwnd(flutter_hwnd)` 成功，随后对原
     FlutterView child HWND 设置 `DWMWA_CLOAK`，避免原 child HWND 再直接绘制。
   - API 层全部成功：

     ```text
     CreateTargetForHwnd target=<host> topmost=1
     CreateSurfaceFromHwnd flutter=<flutter> hr=0x00000000
     Flutter surface visual SetContent hr=0x00000000
     SurfaceProbe DwmCloak hwnd=<flutter> cloaked=1 hr=0x00000000
     ```

   - 实际截图：`build/dcomp_surface_probe_final_capture.png`。
   - 结果：Flutter UI 可以作为 DComp visual 出现在上层，但 Flutter 透明洞区域变成
     黑色/不透明，没有透出下方 HDR swapchain。半透明 Flutter 控件自身能显示，
     但它是叠在 opaque black clear 上，而不是真正 source-over 到 HDR visual。
   - 额外变体：只设置 `WS_EX_LAYERED`、不调用 `SetLayeredWindowAttributes`
     时，`CreateSurfaceFromHwnd` 仍可成功，但 wrapped Flutter surface 整块黑掉，
     连 Flutter UI 都不显示。截图：`build/dcomp_surface_probe_no_lwa_capture.png`。

当前判断：stock Windows Flutter HWND composition 下，很难得到以下真 alpha
顺序：

```text
Mica backdrop -> native HDR DComp surface -> Flutter true-alpha UI
```

这更像是 Windows HWND / DirectComposition / Flutter swapchain 的 airspace /
opaque swapchain 限制，不是简单漏了某个 Win32 style。`CreateSurfaceFromHwnd`
能把 Flutter child HWND 包进 DComp visual tree，但 stock Flutter Windows surface
没有提供可用于 DComp source-over 的 per-pixel alpha。

## 推荐方向

短期 Windows HDR 路线建议保留二值 color-key sandwich：

```text
Host HWND with Mica
Native DComp HDR child HWND
FlutterView layered child HWND with cyan color-key
```

UI 绘制策略：

```text
Flutter client background = normal opaque panel color
Video rectangle only = cyan key color
Flutter controls = normal colors, avoid cyan key color
```

这个路径应该作为显式 Windows HDR 实验路径进入后续实现，并且保留 fallback：

- HDR 输出不可用时回退到现有 Flutter Texture 路径。
- DComp/scRGB/swapchain 初始化失败时回退。
- UI 设计上把视频区域视为硬矩形，不依赖视频洞边缘的半透明 alpha。

如果未来要追求真正 per-pixel alpha，可能需要更大的工程路径：

- 把 Flutter UI 和 video surface 放进同一个 DirectComposition visual tree。
- patch Windows Flutter embedder，让 Flutter Windows surface 以 premultiplied alpha
  swapchain/visual 输出，并暴露 Flutter swapchain / visual 的排序能力。
- 或者用独立 top-level transparent overlay window 承载 Flutter UI，再处理输入、
  焦点、z-order、DPI、多屏和窗口同步问题。

下一步更现实的产品化验证，是把当前 probe 的 FP16 色条替换成真实视频
swapchain，同时保留现有 color-key sandwich 与日志。
