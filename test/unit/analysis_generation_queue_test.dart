import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_cache_service.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/analysis_generation_queue.dart';
import 'package:void_player/analysis/analysis_native_service.dart';
import 'package:void_player/utils/file_lock.dart';

class _FakeCacheService implements AnalysisCacheService {
  int exclusiveLockCount = 0;
  int sharedLockCount = 0;

  @override
  Future<T> withHashExclusiveLock<T>(
    String hash,
    Future<T> Function() action,
  ) async {
    exclusiveLockCount++;
    return action();
  }

  @override
  Future<T> withHashSharedLock<T>(
    String hash,
    Future<T> Function() action,
  ) async {
    sharedLockCount++;
    return action();
  }

  @override
  bool filesExist(String hash) => true;

  @override
  bool deleteIfVacVersionMismatch(String hash) => false;

  @override
  Future<void> addEntry(String hash, String name, String videoPath) =>
      throw UnimplementedError();

  @override
  FileLockHandle acquireHashSharedLockSync(String hash) =>
      throw UnimplementedError();

  @override
  String analysisPath(String hash) => throw UnimplementedError();

  @override
  bool hasOverlayChunks(String hash) => throw UnimplementedError();

  @override
  bool hasOverlayChunkForFrame(String hash, int frameIndex) =>
      throw UnimplementedError();

  @override
  Future<AnalysisCachePruneResult> enforceLimit({
    required int maxBytes,
    Set<String> protectedHashes = const {},
  }) => throw UnimplementedError();

  @override
  Future<String?> findHashForUnchangedVideo(String videoPath) =>
      throw UnimplementedError();

  @override
  String formatBytes(int bytes) => throw UnimplementedError();

  @override
  bool hasEntry(String hash, {String? videoPath}) => throw UnimplementedError();

  @override
  bool hasIncompleteContainer(String hash) => throw UnimplementedError();

  @override
  Future<AnalysisCacheSnapshot> snapshot({int maxBytes = 0}) =>
      throw UnimplementedError();

  @override
  Future<void> touchEntry(String hash) => throw UnimplementedError();
}

class _FakeNativeService implements AnalysisNativeService {
  int generateCount = 0;
  int generateOverlayChunkCount = 0;
  int activeBaseCount = 0;
  int activeOverlayCount = 0;
  int maxActiveOverlayCount = 0;
  Completer<bool>? nextBaseCompleter;
  final List<Completer<bool>> overlayCompleters = [];

  @override
  Future<bool> generateVac2Base(
    String videoPath,
    String hash,
    int maxCacheBytes,
  ) async {
    generateCount++;
    activeBaseCount++;
    try {
      return await (nextBaseCompleter?.future ?? Future.value(true));
    } finally {
      activeBaseCount--;
      nextBaseCompleter = null;
    }
  }

  @override
  Future<bool> generateOverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  }) async {
    generateOverlayChunkCount++;
    activeOverlayCount++;
    maxActiveOverlayCount = activeOverlayCount > maxActiveOverlayCount
        ? activeOverlayCount
        : maxActiveOverlayCount;
    final completer = overlayCompleters.isNotEmpty
        ? overlayCompleters.removeAt(0)
        : null;
    try {
      return await (completer?.future ?? Future.value(true));
    } finally {
      activeOverlayCount--;
    }
  }

  @override
  Future<bool> load(String analysisPath) => throw UnimplementedError();

  @override
  AnalysisSession? openSession(String analysisPath) =>
      throw UnimplementedError();

  @override
  void unload() => throw UnimplementedError();

  @override
  AnalysisGenerationServiceStats? generationServiceStats() => null;
}

void main() {
  test('generation queue rewrites stale cache even when files exist', () async {
    final cache = _FakeCacheService();
    final native = _FakeNativeService();
    final queue = SerialAnalysisGenerationQueue(cache: cache, native: native);

    final ok = await queue.generate(
      videoPath: 'video.mp4',
      hash: 'hash',
      maxCacheBytes: 0,
    );

    expect(ok, isTrue);
    expect(cache.exclusiveLockCount, 1);
    expect(cache.sharedLockCount, 0);
    expect(native.generateCount, 1);
  });

  test(
    'generation queue writes overlay chunks under shared hash lock',
    () async {
      final cache = _FakeCacheService();
      final native = _FakeNativeService();
      final queue = SerialAnalysisGenerationQueue(cache: cache, native: native);

      final ok = await queue.generateOverlayChunk(
        videoPath: 'video.mp4',
        hash: 'hash',
        startFrame: 10,
        endFrame: 20,
        maxCacheBytes: 0,
      );

      expect(ok, isTrue);
      expect(cache.exclusiveLockCount, 0);
      expect(cache.sharedLockCount, 1);
      expect(native.generateOverlayChunkCount, 1);
    },
  );

  test('generation queue allows concurrent overlay chunk generation', () async {
    final cache = _FakeCacheService();
    final firstChunk = Completer<bool>();
    final secondChunk = Completer<bool>();
    final native = _FakeNativeService()
      ..overlayCompleters.addAll([firstChunk, secondChunk]);
    final queue = SerialAnalysisGenerationQueue(cache: cache, native: native);

    final first = queue.generateOverlayChunk(
      videoPath: 'video.mp4',
      hash: 'hash',
      startFrame: 0,
      endFrame: 63,
      maxCacheBytes: 0,
    );
    final second = queue.generateOverlayChunk(
      videoPath: 'video.mp4',
      hash: 'hash',
      startFrame: 64,
      endFrame: 127,
      maxCacheBytes: 0,
    );

    await Future<void>.delayed(Duration.zero);

    expect(native.maxActiveOverlayCount, 2);
    firstChunk.complete(true);
    secondChunk.complete(true);
    expect(await Future.wait([first, second]), [true, true]);
    expect(cache.sharedLockCount, 2);
  });

  test(
    'generation queue blocks same-hash overlay while base is writing',
    () async {
      final cache = _FakeCacheService();
      final baseGeneration = Completer<bool>();
      final overlayChunk = Completer<bool>();
      final native = _FakeNativeService()
        ..nextBaseCompleter = baseGeneration
        ..overlayCompleters.add(overlayChunk);
      final queue = SerialAnalysisGenerationQueue(cache: cache, native: native);

      final base = queue.generate(
        videoPath: 'video.mp4',
        hash: 'hash',
        maxCacheBytes: 0,
      );
      await Future<void>.delayed(Duration.zero);

      final overlay = queue.generateOverlayChunk(
        videoPath: 'video.mp4',
        hash: 'hash',
        startFrame: 0,
        endFrame: 63,
        maxCacheBytes: 0,
      );
      await Future<void>.delayed(Duration.zero);

      expect(native.activeBaseCount, 1);
      expect(native.activeOverlayCount, 0);

      baseGeneration.complete(true);
      expect(await base, isTrue);
      await Future<void>.delayed(Duration.zero);

      expect(native.activeOverlayCount, 1);
      overlayChunk.complete(true);
      expect(await overlay, isTrue);
    },
  );
}
