import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';
import 'package:void_player/analysis/analysis_cache_service.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/analysis_generation_queue.dart';
import 'package:void_player/analysis/analysis_native_service.dart';
import 'package:void_player/utils/file_lock.dart';

class _FakeCacheService implements AnalysisCacheService {
  int lockCount = 0;

  @override
  Future<T> withHashExclusiveLock<T>(
    String hash,
    Future<T> Function() action,
  ) async {
    lockCount++;
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
  bool hasLegacyAnalysis(String hash) => throw UnimplementedError();

  @override
  String legacyAnalysisPath(String hash) => throw UnimplementedError();

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

  @override
  Future<bool> generateAnalysis(
    String videoPath,
    String hash,
    int maxCacheBytes,
  ) async {
    generateCount++;
    return true;
  }

  @override
  Future<bool> load(String analysisPath) => throw UnimplementedError();

  @override
  AnalysisSession? openSession(String analysisPath) =>
      throw UnimplementedError();

  @override
  void unload() => throw UnimplementedError();
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
    expect(cache.lockCount, 1);
    expect(native.generateCount, 1);
  });
}
