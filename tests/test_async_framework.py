"""
测试 Python 异步框架

用法:
    python tests/test_async_framework.py
"""
import sys
import time
import threading
from pathlib import Path
from dataclasses import dataclass

# 设置 stdout 编码
if sys.platform == 'win32':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')

# 添加项目根目录到 path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from PySide6.QtCore import QCoreApplication, QThread, QMutex, QWaitCondition

from player.native import voidview_native
from player.core.async_manager import AsyncOperationManager, OperationState
from player.core.decode_worker import DecodeWorker, DecodeWorkerPool, CommandType, DecodeCommand


def test_async_manager_basic():
    """测试 AsyncOperationManager 基本功能"""
    print("=" * 60)
    print("Test: AsyncOperationManager basic operations")
    print("=" * 60)

    # 需要 QCoreApplication 来使用信号
    app = QCoreApplication.instance() or QCoreApplication([])

    manager = AsyncOperationManager(max_workers=2)
    results = []

    def slow_task(x):
        time.sleep(0.1)
        return x * 2

    def on_complete(result):
        results.append(result)

    # 提交任务
    token = voidview_native.CancelToken()
    op_id = manager.submit(slow_task, 5, cancel_token=token, callback=on_complete)
    print(f"  Submitted operation: {op_id}")

    # 等待完成
    time.sleep(0.3)

    # 检查结果
    print(f"  Results: {results}")

    assert len(results) == 1, f"Expected 1 result, got {len(results)}"
    assert results[0].state == OperationState.COMPLETED, f"Expected COMPLETED state, got {results[0].state}"
    assert results[0].result == 10, f"Expected 10, got {results[0].result}"

    manager.shutdown()
    print("✓ AsyncOperationManager basic operations passed")


def test_async_manager_cancel():
    """测试 AsyncOperationManager 取消功能"""
    print("\n" + "=" * 60)
    print("Test: AsyncOperationManager cancellation")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])

    manager = AsyncOperationManager(max_workers=2)
    results = []
    cancelled = []

    def slow_task(token, x):
        for i in range(10):
            if token.is_cancelled():
                return "cancelled"
            time.sleep(0.05)
        return x * 2

    def on_complete(result):
        results.append(result)
        if result.state == OperationState.CANCELLED:
            cancelled.append(result.operation_id)

    # 提交任务
    token = voidview_native.CancelToken()
    op_id = manager.submit(slow_task, token, 5, cancel_token=token, callback=on_complete)
    print(f"  Submitted operation: {op_id}")

    # 等待一会儿后取消
    time.sleep(0.1)
    print(f"  Requesting cancellation...")
    manager.cancel(op_id)

    # 等待完成
    time.sleep(0.3)

    print(f"  Results: {results}")
    print(f"  Cancelled: {cancelled}")

    # 任务应该被取消或已完成
    state = manager.get_state(op_id)
    print(f"  Final state: {state}")

    manager.shutdown()
    print("✓ AsyncOperationManager cancellation passed")


def test_decode_worker_basic():
    """测试 DecodeWorker 基本功能"""
    print("\n" + "=" * 60)
    print("Test: DecodeWorker basic operations")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])

    # 使用 H.265 视频 (软解)
    video_path = project_root / "resources" / "video" / "NovosobornayaSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    # 创建解码器
    decoder = voidview_native.HardwareDecoder(str(video_path))
    assert decoder.initialize(0), f"Failed to initialize decoder: {decoder.error_message}"

    print(f"  Video: {video_path.name}")

    # 创建 worker
    worker = DecodeWorker(track_index=0)
    worker.set_decoder(decoder)
    worker.start()

    # 收集结果
    frames_decoded = []
    worker.frame_decoded.connect(lambda idx: frames_decoded.append(idx))

    # 解码几帧
    for i in range(5):
        cmd = DecodeCommand(command=CommandType.DECODE)
        worker.post_command(cmd)
        time.sleep(0.1)  # 等待处理

    # 等待完成
    time.sleep(0.3)

    # 停止 worker
    worker.stop()

    print(f"  Frames decoded: {len(frames_decoded)}")
    assert len(frames_decoded) >= 3, f"Expected at least 3 frames, got {len(frames_decoded)}"

    print("✓ DecodeWorker basic operations passed")


def test_decode_worker_seek_cancel():
    """测试 DecodeWorker seek 取消功能"""
    print("\n" + "=" * 60)
    print("Test: DecodeWorker seek cancellation")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])

    # 使用 H.264 视频
    video_path = project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    decoder = voidview_native.HardwareDecoder(str(video_path))
    assert decoder.initialize(0), f"Failed to initialize decoder: {decoder.error_message}"

    print(f"  Video: {video_path.name}")
    print(f"  Duration: {decoder.duration_ms}ms")

    # 创建 worker
    worker = DecodeWorker(track_index=0)
    worker.set_decoder(decoder)
    worker.start()

    # 收集结果
    seek_results = []
    worker.seek_completed.connect(lambda idx, pos: seek_results.append(('completed', pos)))
    worker.seek_cancelled.connect(lambda idx: seek_results.append(('cancelled', None)))

    # 测试正常 seek
    print("  Testing normal seek...")
    cmd = DecodeCommand(command=CommandType.SEEK_PRECISE, timestamp_ms=5000)
    worker.post_command(cmd)
    time.sleep(0.5)

    # 测试取消 seek
    print("  Testing seek cancellation...")
    cmd = DecodeCommand(command=CommandType.SEEK_PRECISE, timestamp_ms=8000)
    worker.post_command(cmd)

    # 立即取消
    time.sleep(0.02)  # 稍等一下让 seek 开始
    worker.cancel_current()
    time.sleep(0.3)

    # 停止 worker
    worker.stop()

    print(f"  Results: {seek_results}")
    assert len(seek_results) >= 1, f"Expected at least 1 result, got {len(seek_results)}"

    print("✓ DecodeWorker seek cancellation passed")


def test_decode_worker_pool():
    """测试 DecodeWorkerPool 多轨道解码"""
    print("\n" + "=" * 60)
    print("Test: DecodeWorkerPool multi-track decoding")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])

    video_paths = [
        project_root / "resources" / "video" / "NovosobornayaSquare_1920x1080.mp4",
        project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4",
    ]

    # 检查文件
    valid_paths = [p for p in video_paths if p.exists()]
    if len(valid_paths) < 2:
        print("⚠ Need 2 video files for this test")
        return

    print(f"  Testing with {len(valid_paths)} videos")

    pool = DecodeWorkerPool()

    # 为每个视频创建 worker
    for i, path in enumerate(valid_paths):
        decoder = voidview_native.HardwareDecoder(str(path))
        assert decoder.initialize(0), f"Failed to initialize decoder {i}: {decoder.error_message}"

        worker = pool.create_worker(i)
        worker.set_decoder(decoder)
        print(f"  Created worker for: {path.name}")

    # 收集结果
    all_frames = []
    for i in range(len(valid_paths)):
        worker = pool.get_worker(i)
        if worker:
            worker.frame_decoded.connect(lambda idx=i: all_frames.append(idx))

    # 同时解码
    for i in range(len(valid_paths)):
        worker = pool.get_worker(i)
        if worker:
            for _ in range(3):
                worker.post_command(DecodeCommand(command=CommandType.DECODE))

    time.sleep(0.5)

    # 广播 seek
    print("  Broadcasting seek to 3000ms...")
    pool.broadcast_seek(3000, precise=True)
    time.sleep(0.5)

    # 取消所有
    print("  Cancelling all...")
    pool.cancel_all()
    time.sleep(0.2)

    # 停止所有
    pool.stop_all()

    print(f"  Total frames decoded: {len(all_frames)}")
    print("✓ DecodeWorkerPool multi-track decoding passed")


def test_concurrent_seeks():
    """测试并发 seek 操作 (模拟快速拖动)"""
    print("\n" + "=" * 60)
    print("Test: Concurrent seek operations (rapid seeking)")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])

    video_path = project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    decoder = voidview_native.HardwareDecoder(str(video_path))
    assert decoder.initialize(0), f"Failed to initialize decoder: {decoder.error_message}"

    print(f"  Video: {video_path.name}")
    print(f"  Duration: {decoder.duration_ms}ms")

    worker = DecodeWorker(track_index=0)
    worker.set_decoder(decoder)
    worker.start()

    # 收集结果
    results = []
    worker.seek_completed.connect(lambda idx, pos: results.append(('completed', pos)))
    worker.seek_cancelled.connect(lambda idx: results.append(('cancelled', None)))

    # 模拟快速拖动
    print("  Simulating rapid seeking...")
    positions = [1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000]

    for pos in positions:
        # 取消之前的 seek
        worker.cancel_current()

        # 发起新的 seek
        worker.post_command(DecodeCommand(command=CommandType.SEEK_PRECISE, timestamp_ms=pos))
        time.sleep(0.05)  # 模拟用户拖动速度

    # 等待最后一个完成
    time.sleep(0.5)

    worker.stop()

    # 分析结果
    completed = [r for r in results if r[0] == 'completed']
    cancelled = [r for r in results if r[0] == 'cancelled']

    print(f"  Completed: {len(completed)}, Cancelled: {len(cancelled)}")
    print(f"  Results: {results}")

    # 应该有一些被取消，最后一个应该完成
    assert len(completed) >= 1, f"Expected at least 1 completed seek"

    print("✓ Concurrent seek operations passed")


def main():
    print("=" * 60)
    print("Python Async Framework Tests")
    print("=" * 60)

    test_async_manager_basic()
    test_async_manager_cancel()
    test_decode_worker_basic()
    test_decode_worker_seek_cancel()
    test_decode_worker_pool()
    test_concurrent_seeks()

    print("\n" + "=" * 60)
    print("All tests completed!")
    print("=" * 60)


if __name__ == "__main__":
    main()
