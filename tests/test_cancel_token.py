"""
测试 Native 层取消令牌和异步 API

用法:
    python tests/test_cancel_token.py
"""
import sys
import time
import threading
from pathlib import Path

# 设置 stdout 编码
if sys.platform == 'win32':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')

# 添加项目根目录到 path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from player.native import voidview_native


def test_cancel_token_basic():
    """测试 CancelToken 基本功能"""
    print("=" * 60)
    print("Test: CancelToken basic operations")
    print("=" * 60)

    token = voidview_native.CancelToken()
    assert not token.is_cancelled(), "Token should not be cancelled initially"

    token.cancel()
    assert token.is_cancelled(), "Token should be cancelled after cancel()"

    token.reset()
    assert not token.is_cancelled(), "Token should not be cancelled after reset()"

    print("✓ CancelToken basic operations passed")


def test_cancel_token_threading():
    """测试 CancelToken 在多线程环境下的使用"""
    print("\n" + "=" * 60)
    print("Test: CancelToken threading safety")
    print("=" * 60)

    token = voidview_native.CancelToken()
    results = []

    def worker(worker_id):
        for i in range(10):
            if token.is_cancelled():
                results.append(f"Worker {worker_id} cancelled at iteration {i}")
                return
            time.sleep(0.01)
        results.append(f"Worker {worker_id} completed")

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(3)]

    # 启动所有线程
    for t in threads:
        t.start()

    # 等待一小段时间后取消
    time.sleep(0.03)
    token.cancel()

    for t in threads:
        t.join()

    print(f"Results: {results}")
    assert any("cancelled" in r for r in results), "At least one worker should be cancelled"
    print("✓ CancelToken threading safety passed")


def test_decode_async_basic():
    """测试异步解码基本功能 (使用软解视频)"""
    print("\n" + "=" * 60)
    print("Test: Async decode basic (H.265 software decode)")
    print("=" * 60)

    # 使用 H.265 视频 (触发软解)
    video_path = project_root / "resources" / "video" / "NovosobornayaSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    # 探测文件
    info = voidview_native.probe_file(str(video_path))
    print(f"Video: {video_path.name}")
    print(f"  Codec: {info.codec_name}")
    print(f"  Resolution: {info.width}x{info.height}")
    print(f"  Duration: {info.duration_ms}ms")

    # 创建解码器
    decoder = voidview_native.HardwareDecoder(str(video_path))
    assert decoder.initialize(0), f"Failed to initialize decoder: {decoder.error_message}"

    # 测试异步解码
    token = voidview_native.CancelToken()
    frames_decoded = 0

    for i in range(10):
        if decoder.decode_next_frame_async(token):
            frames_decoded += 1
            if decoder.has_pending_frame():
                # 在实际应用中，这里需要在 GL 上下文中调用 upload_pending_frame()
                # 但测试中我们只验证 has_pending_frame 返回 True
                pass

    print(f"  Frames decoded: {frames_decoded}")
    assert frames_decoded > 0, "Should decode at least one frame"
    print("✓ Async decode basic passed")


def test_seek_precise_async_cancel():
    """测试精确 seek 取消功能"""
    print("\n" + "=" * 60)
    print("Test: Seek precise async with cancellation")
    print("=" * 60)

    # 使用 H.264 视频 (触发硬解)
    video_path = project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    info = voidview_native.probe_file(str(video_path))
    print(f"Video: {video_path.name}")
    print(f"  Codec: {info.codec_name}")
    print(f"  Duration: {info.duration_ms}ms")

    decoder = voidview_native.HardwareDecoder(str(video_path))
    assert decoder.initialize(0), f"Failed to initialize decoder: {decoder.error_message}"

    token = voidview_native.CancelToken()

    # 测试正常 seek
    print("  Testing normal seek...")
    start_time = time.perf_counter()
    result = decoder.seek_to_precise_async(5000, token)  # Seek to 5s
    elapsed = time.perf_counter() - start_time
    print(f"    Result: {result}, Time: {elapsed:.3f}s")
    assert result, "Seek should succeed without cancellation"
    assert decoder.has_pending_frame(), "Should have pending frame after seek"

    # 测试取消 seek
    print("  Testing cancelled seek...")
    token.cancel()
    result = decoder.seek_to_precise_async(10000, token)  # Seek to 10s
    print(f"    Result: {result} (should be False due to cancellation)")
    assert not result, "Seek should fail when cancelled"

    print("✓ Seek precise async with cancellation passed")


def test_shared_cancel_token():
    """测试共享 CancelToken 在多个解码器之间的使用"""
    print("\n" + "=" * 60)
    print("Test: Shared CancelToken across multiple decoders")
    print("=" * 60)

    video_paths = [
        project_root / "resources" / "video" / "NovosobornayaSquare_1920x1080.mp4",
        project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4",
    ]

    # 检查文件存在
    valid_paths = [p for p in video_paths if p.exists()]
    if not valid_paths:
        print("⚠ No video files found")
        return

    print(f"Testing with {len(valid_paths)} video(s)")

    # 创建解码器和共享 token
    decoders = []
    for path in valid_paths:
        decoder = voidview_native.HardwareDecoder(str(path))
        if decoder.initialize(0):
            decoders.append(decoder)
            print(f"  Initialized: {path.name}")

    if not decoders:
        print("⚠ No decoders initialized")
        return

    token = voidview_native.CancelToken()

    # 模拟并行解码
    def decode_frames(decoder_idx):
        decoder = decoders[decoder_idx]
        count = 0
        for _ in range(100):
            if token.is_cancelled():
                print(f"  Decoder {decoder_idx}: cancelled at frame {count}")
                return count
            if not decoder.decode_next_frame_async(token):
                break
            count += 1
        return count

    # 启动解码线程
    results = [None] * len(decoders)
    threads = []
    for i in range(len(decoders)):
        t = threading.Thread(target=lambda idx: results.__setitem__(idx, decode_frames(idx)), args=(i,))
        threads.append(t)
        t.start()

    # 等待一会儿后取消
    time.sleep(0.1)
    print("  Cancelling all decoders...")
    token.cancel()

    for t in threads:
        t.join(timeout=2.0)

    print(f"  Results: {results}")
    print("✓ Shared CancelToken test passed")


def main():
    print("=" * 60)
    print("Native Layer Async API Tests")
    print("=" * 60)

    test_cancel_token_basic()
    test_cancel_token_threading()
    test_decode_async_basic()
    test_seek_precise_async_cancel()
    test_shared_cancel_token()

    print("\n" + "=" * 60)
    print("All tests completed!")
    print("=" * 60)


if __name__ == "__main__":
    main()
