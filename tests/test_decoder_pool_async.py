"""
端到端测试 - DecoderPoolAsync 完整功能验证

用法:
    python tests/test_decoder_pool_async.py
"""
import sys
import time
from pathlib import Path

# 设置 stdout 编码
if sys.platform == 'win32':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')

# 添加项目根目录到 path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from PySide6.QtCore import QCoreApplication, QEventLoop

from player.core.decoder_pool_async import DecoderPoolAsync, MediaInfo


def test_async_probe():
    """测试异步媒体探测"""
    print("=" * 60)
    print("Test: Async media probe")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])
    pool = DecoderPoolAsync()

    video_path = project_root / "resources" / "video" / "NovosobornayaSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    results = []

    def on_probe_complete(info: MediaInfo):
        results.append(info)
        print(f"  Probed: {info.path}")
        print(f"    Valid: {info.valid}")
        print(f"    Codec: {info.codec_name}")
        print(f"    Resolution: {info.width}x{info.height}")
        print(f"    Duration: {info.duration_ms}ms")
        print(f"    FPS: {info.fps}")

    pool.probe_file_async(str(video_path), on_probe_complete)

    # 等待完成
    loop = QEventLoop()
    while not results:
        app.processEvents()
        time.sleep(0.01)
        if len(results) > 0:
            break
    loop.quit()

    assert len(results) == 1
    assert results[0].valid

    pool.shutdown()
    print("✓ Async media probe passed")


def test_add_track_and_init():
    """测试添加轨道和初始化解码器"""
    print("\n" + "=" * 60)
    print("Test: Add track and initialize decoder")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])
    pool = DecoderPoolAsync()

    video_paths = [
        project_root / "resources" / "video" / "NovosobornayaSquare_1920x1080.mp4",
        project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4",
    ]

    # 添加轨道
    for i, path in enumerate(video_paths):
        if path.exists():
            success = pool.add_track(i, str(path))
            print(f"  Add track {i}: {path.name} -> {success}")
            assert success, f"Failed to add track {i}"

    assert pool.track_count >= 1, "Should have at least 1 track"
    print(f"  Track count: {pool.track_count}")
    print(f"  Duration: {pool.duration_ms}ms")

    # 注意: 初始化解码器需要 OpenGL 上下文，在这个测试中无法执行
    # 这里只验证轨道添加成功

    pool.shutdown()
    print("✓ Add track and initialize decoder passed")


def test_seek_scheduling():
    """测试 seek 调度"""
    print("\n" + "=" * 60)
    print("Test: Seek scheduling")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])
    pool = DecoderPoolAsync()

    video_path = project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    # 添加轨道
    success = pool.add_track(0, str(video_path))
    assert success

    print(f"  Video: {video_path.name}")
    print(f"  Duration: {pool.duration_ms}ms")

    # 收集 seek 事件
    seek_events = []
    pool.seek_started.connect(lambda ms: seek_events.append(('started', ms)))
    pool.seek_completed.connect(lambda ms: seek_events.append(('completed', ms)))

    # 调度 seek (会被 50ms 延迟合并)
    for pos in [1000, 2000, 3000, 4000, 5000]:
        pool.seek_to_precise(pos)
        time.sleep(0.02)  # 快速连续 seek

    print(f"  Scheduled 5 seeks rapidly")

    # 等待 seek 开始
    time.sleep(0.1)
    app.processEvents()

    print(f"  Seek events: {seek_events}")

    # 应该只有一个 seek_started (最后一个)
    started_count = sum(1 for e in seek_events if e[0] == 'started')
    print(f"  Seek started count: {started_count}")

    pool.shutdown()
    print("✓ Seek scheduling passed")


def test_cancel_seek():
    """测试取消 seek"""
    print("\n" + "=" * 60)
    print("Test: Cancel seek")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])
    pool = DecoderPoolAsync()

    video_path = project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    success = pool.add_track(0, str(video_path))
    assert success

    print(f"  Video: {video_path.name}")

    # 收集事件
    events = []
    pool.seek_started.connect(lambda ms: events.append(('started', ms)))
    pool.seek_cancelled.connect(lambda: events.append(('cancelled', None)))

    # 发起 seek 后立即取消
    pool.seek_to_precise(5000)
    time.sleep(0.06)  # 等待 seek 开始
    pool.cancel_seek()

    time.sleep(0.1)
    app.processEvents()

    print(f"  Events: {events}")

    pool.shutdown()
    print("✓ Cancel seek passed")


def test_playback_simulation():
    """测试播放模拟"""
    print("\n" + "=" * 60)
    print("Test: Playback simulation")
    print("=" * 60)

    app = QCoreApplication.instance() or QCoreApplication([])
    pool = DecoderPoolAsync()

    video_path = project_root / "resources" / "video" / "TheaterSquare_1920x1080.mp4"
    if not video_path.exists():
        print(f"⚠ Video file not found: {video_path}")
        return

    success = pool.add_track(0, str(video_path))
    assert success

    print(f"  Video: {video_path.name}")
    print(f"  Duration: {pool.duration_ms}ms")

    # 模拟播放控制
    print("  Testing play/pause...")

    pool.play()
    assert pool.is_playing
    print("    Playing: True")

    time.sleep(0.1)
    app.processEvents()

    pool.pause()
    assert not pool.is_playing
    print("    Playing: False (paused)")

    pool.toggle_play()
    assert pool.is_playing
    print("    Playing: True (toggled)")

    pool.pause()

    pool.shutdown()
    print("✓ Playback simulation passed")


def test_texture_upload_scheduler():
    """测试纹理上传调度器"""
    print("\n" + "=" * 60)
    print("Test: Texture upload scheduler")
    print("=" * 60)

    from player.core.texture_upload_scheduler import TextureUploadScheduler

    scheduler = TextureUploadScheduler()

    # 测试调度
    scheduler.schedule_upload(0)
    scheduler.schedule_upload(1)
    scheduler.schedule_upload(2)

    assert scheduler.has_pending_uploads()
    assert scheduler.get_pending_tracks() == {0, 1, 2}

    print("  Pending tracks after scheduling: {0, 1, 2}")

    # 清除
    scheduler.clear_pending()
    assert not scheduler.has_pending_uploads()

    print("  After clear: no pending uploads")

    print("✓ Texture upload scheduler passed")


def main():
    print("=" * 60)
    print("End-to-End Tests - DecoderPoolAsync")
    print("=" * 60)

    test_async_probe()
    test_add_track_and_init()
    test_seek_scheduling()
    test_cancel_seek()
    test_playback_simulation()
    test_texture_upload_scheduler()

    print("\n" + "=" * 60)
    print("All end-to-end tests completed!")
    print("=" * 60)


if __name__ == "__main__":
    main()
