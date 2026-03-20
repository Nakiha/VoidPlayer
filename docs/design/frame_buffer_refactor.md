# Frame Buffer 重构计划

## 背景

当前播放器在解码速度波动时帧率不稳定，原因是：
1. **解码是请求驱动的**：Python 层请求一帧，才解码一帧
2. **FrameQueue 未被使用**：`FrameQueue` 类存在但未集成到解码流程
3. **没有预取缓冲**：解码和渲染紧耦合

## 目标

```
DecodeWorker 线程 → [FrameQueue 12帧] → Python/GL 取帧
                      ↑
               持续解码填满队列
```

- Decoder 持续解码填满 FrameQueue
- Python 层直接从 FrameQueue 取帧
- 缓冲足够大（12帧 ≈ 200ms @ 60fps）以吸收解码波动

## 已有优势（无需改动）

| 功能 | 状态 | 位置 |
|------|------|------|
| 多轨道支持 | ✅ 已有 | `DecoderPool` 管理 8 轨道 |
| 零拷贝（D3D11→GL） | ✅ 已有 | `TextureInterop` + WGL_NV_DX_interop |
| GPU NV12→RGBA | ✅ 已有 | D3D11 VideoProcessor |
| 软件解码回退 | ✅ 已有 | `upload_software_frame()` |
| 独立解码线程 | ✅ 已有 | `DecodeWorker` (C++ 线程，不持 GIL) |

## 实现状态

**更新日期**: 2026-03-20

### ✅ 已完成

| Phase | 内容 | 状态 |
|-------|------|------|
| 1.1 | FrameQueue 默认容量增大到 12 | ✅ |
| 1.2 | DecodeWorker 添加 FrameQueue 成员 | ✅ |
| 1.3 | DecodeWorker 添加 pop_frame() 方法 | ✅ |
| 1.4 | HardwareDecoder::take_pending_frame() | ✅ |
| 1.5 | HardwareDecoder::set_pending_frame() | ✅ |
| 2.1 | DecoderPool::get_frame() | ✅ |
| 2.2 | DecoderPool::get_frame_queue_size() | ✅ |
| 2.3 | DecoderPool::request_fill_buffers() | ✅ |
| 3.1 | DecodeWorker::fill_frame_buffer() 填充模式 | ✅ |
| 3.2 | FILL_BUFFER 命令类型 | ✅ |
| 3.3 | pop_frame() 低水位触发填充 | ✅ |
| 绑定 | Python bindings 新接口 | ✅ |

### 改动文件清单

| 文件 | 改动内容 |
|------|----------|
| [frame_queue.hpp](native/include/voidview_native/frame_queue.hpp) | 默认容量 4→12 |
| [decode_worker.hpp](native/include/voidview_native/decode_worker.hpp) | 添加 FrameQueue 成员、pop_frame()、FILL_BUFFER 命令 |
| [decode_worker.cpp](native/src/decode_worker.cpp) | 实现填充模式、低水位触发 |
| [hardware_decoder.hpp](native/include/voidview_native/hardware_decoder.hpp) | 新增 take_pending_frame()、set_pending_frame() |
| [hardware_decoder.cpp](native/src/hardware_decoder.cpp) | 实现帧转移方法 |
| [decoder_pool.py](player/core/decoder_pool.py) | 新增 get_frame()、request_fill_buffers() |
| [bindings.cpp](native/src/bindings.cpp) | 绑定 pop_frame、frame_queue_size、clear_frame_queue |

## 架构说明

### 帧缓冲流程

```
DecodeWorker 线程:
  DECODE_FRAME / FILL_BUFFER 命令
      ↓
  循环解码直到队列满
      ↓
  decoder_->decode_frame_internal()
      ↓
  frame = decoder_->take_pending_frame()  // 转移帧所有权
      ↓
  frame_queue_.push(frame)

主线程 (Python/GL):
  pop_frame(timeout_ms)
      ↓
  frame = frame_queue_.pop(timeout_ms)
      ↓
  decoder_->set_pending_frame(frame)  // 设置回解码器
      ↓
  decoder_->upload_pending_frame()  // 在 GL 线程上传纹理
      ↓
  if (queue_size < 4) trigger FILL_BUFFER  // 低水位自动填充
```

### API 使用示例

```python
# Python 层取帧
success, pts_ms = decoder_pool.get_frame(track_index, timeout_ms=50)
if success:
    # 帧已设置到 decoder，可以上传纹理
    track.decoder.upload_pending_frame()

# 检查队列大小
queue_size = decoder_pool.get_frame_queue_size(track_index)

# 请求填充缓冲
decoder_pool.request_fill_buffers()
```

## 不改动

- `TextureInterop`：零拷贝逻辑不变
- `HardwareDecoder` 核心解码逻辑：只新增帧转移方法
- 软件解码路径：保持现有回退机制
- Packet 缓冲：暂不实现，不是当前瓶颈
- `PlaybackController`：保持原有请求驱动模式，兼容过渡

## 风险与注意事项

1. **内存占用**：12 帧 1080p NV12 ≈ 40MB，每个轨道独立
2. **纹理上传**：仍需在 GL 线程调用 `upload_pending_frame()`
3. **Seek 处理**：seek 时需要清空并重新填充队列
4. **EOF 处理**：队列为空 + EOF → 播放结束

## 测试验证

```bash
# 基本播放测试
python run_player.py -i resources/video/h266_10s_1920x1080.mp4 --auto-play

# 多轨道测试
python run_player.py -i resources/video/h266_10s_1920x1080.mp4 -i resources/video/h264_9s_1920x1080.mp4 --auto-play

# 验证帧率稳定性（观察解码耗时波动时是否仍然流畅）
```

## 原始改造计划（参考）

### Phase 1: 集成 FrameQueue 到 DecodeWorker

#### 1.1 修改 FrameQueue 容量

**文件**: `native/include/voidview_native/frame_queue.hpp`

```cpp
// 改动：增大默认容量
explicit FrameQueue(size_t max_size = 12);  // 原来是 4
```

#### 1.2 DecodeWorker 持有 FrameQueue

**文件**: `native/include/voidview_native/decode_worker.hpp`

```cpp
class DecodeWorker {
public:
    // 新增：获取 FrameQueue（供 Python 层取帧）
    FrameQueue* frame_queue() { return &frame_queue_; }

    // 新增：从队列取帧（带超时）
    AVFrame* pop_frame(int timeout_ms = -1);

private:
    FrameQueue frame_queue_{12};  // 新增：帧缓冲队列
    // ...
};
```

#### 1.3 DecodeWorker 填充模式

**文件**: `native/src/decode_worker.cpp`

改动 `execute_command()` 中的 `DECODE_FRAME` 逻辑：

```cpp
case DecodeCommandType::DECODE_FRAME: {
    // 填充模式：持续解码直到队列满
    while (!frame_queue_.is_full() && !cancelled_ && running_) {
        bool success = decoder_->decode_frame_internal();
        if (!success) {
            if (decoder_->is_eof()) break;
            // 错误处理
            break;
        }

        // 从 decoder 取出解码帧，放入队列
        AVFrame* frame = decoder_->take_pending_frame();  // 新增方法
        if (frame) {
            frame_queue_.push(frame);
        }
    }
    notify_callback(true, decoder_->get_current_pts_ms());
    break;
}
```

#### 1.4 HardwareDecoder 新增帧转移方法

**文件**: `native/include/voidview_native/hardware_decoder.hpp`

```cpp
/**
 * 取出待处理的帧（转移所有权）
 * 调用后 has_pending_frame() 返回 false
 */
AVFrame* take_pending_frame();
```

**文件**: `native/src/hardware_decoder.cpp`

```cpp
AVFrame* HardwareDecoder::take_pending_frame() {
    if (!impl_->has_pending_frame_) return nullptr;

    AVFrame* frame = impl_->frame_;
    impl_->frame_ = av_frame_alloc();  // 分配新帧供下次使用
    impl_->has_pending_frame_ = false;
    return frame;
}
```

### Phase 2: Python 层改为拉取模式

#### 2.1 DecoderPool 取帧接口

**文件**: `player/core/decoder_pool.py`

```python
def get_frame(self, index: int, timeout_ms: int = 50) -> tuple[bool, int]:
    """
    从指定轨道的帧队列取一帧

    Args:
        index: 轨道索引
        timeout_ms: 超时时间，-1 表示无限等待

    Returns:
        (success, pts_ms) - success=False 表示超时或 EOF
    """
    track = self._tracks[index]
    if not track or not track.worker:
        return False, 0

    frame = track.worker.pop_frame(timeout_ms)
    if frame is None:
        return False, 0

    # 标记有待上传帧
    track.has_pending_upload = True
    return True, track.decoder.current_pts_ms
```

### Phase 3: 持续填充机制

#### 3.1 DecodeWorker 自动填充

**文件**: `native/src/decode_worker.cpp`

新增 `FILL_BUFFER` 命令类型：

```cpp
enum class DecodeCommandType {
    // ...
    FILL_BUFFER,  // 填充帧缓冲直到满
};

case DecodeCommandType::FILL_BUFFER: {
    // 同 DECODE_FRAME 的填充逻辑
    fill_frame_buffer();
    break;
}
```

#### 3.2 低水位触发

当 `frame_queue_.size() < threshold` 时自动触发填充：

```cpp
// 在 pop_frame() 中
AVFrame* DecodeWorker::pop_frame(int timeout_ms) {
    AVFrame* frame = frame_queue_.pop(timeout_ms);

    // 低水位触发
    if (frame && frame_queue_.size() < 4) {
        push_command({DecodeCommandType::FILL_BUFFER, 0});
    }

    return frame;
}
```
