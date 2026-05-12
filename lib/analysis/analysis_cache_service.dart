import '../utils/file_lock.dart';
import 'analysis_cache.dart';

abstract class AnalysisCacheService {
  Future<String?> findHashForUnchangedVideo(String videoPath);
  Future<void> touchEntry(String hash);
  Future<AnalysisCachePruneResult> enforceLimit({
    required int maxBytes,
    Set<String> protectedHashes = const {},
  });
  bool filesExist(String hash);
  bool deleteIfVacVersionMismatch(String hash);
  String analysisPath(String hash);
  String legacyAnalysisPath(String hash);
  bool hasLegacyAnalysis(String hash);
  bool hasOverlayChunks(String hash);
  FileLockHandle acquireHashSharedLockSync(String hash);
  bool hasEntry(String hash, {String? videoPath});
  Future<T> withHashExclusiveLock<T>(String hash, Future<T> Function() action);
  Future<AnalysisCacheSnapshot> snapshot({int maxBytes = 0});
  bool hasIncompleteContainer(String hash);
  Future<void> addEntry(String hash, String name, String videoPath);
  String formatBytes(int bytes);
}

class DefaultAnalysisCacheService implements AnalysisCacheService {
  const DefaultAnalysisCacheService();

  @override
  Future<String?> findHashForUnchangedVideo(String videoPath) =>
      AnalysisCache.findHashForUnchangedVideo(videoPath);

  @override
  Future<void> touchEntry(String hash) => AnalysisCache.touchEntry(hash);

  @override
  Future<AnalysisCachePruneResult> enforceLimit({
    required int maxBytes,
    Set<String> protectedHashes = const {},
  }) {
    return AnalysisCache.enforceLimit(
      maxBytes: maxBytes,
      protectedHashes: protectedHashes,
    );
  }

  @override
  bool filesExist(String hash) => AnalysisCache.filesExist(hash);

  @override
  bool deleteIfVacVersionMismatch(String hash) =>
      AnalysisCache.deleteIfVacVersionMismatch(hash);

  @override
  String analysisPath(String hash) => AnalysisCache.analysisPath(hash);

  @override
  String legacyAnalysisPath(String hash) =>
      AnalysisCache.legacyAnalysisPath(hash);

  @override
  bool hasLegacyAnalysis(String hash) => AnalysisCache.hasLegacyAnalysis(hash);

  @override
  bool hasOverlayChunks(String hash) => AnalysisCache.hasOverlayChunks(hash);

  @override
  FileLockHandle acquireHashSharedLockSync(String hash) =>
      AnalysisCache.acquireHashSharedLockSync(hash);

  @override
  bool hasEntry(String hash, {String? videoPath}) =>
      AnalysisCache.hasEntry(hash, videoPath: videoPath);

  @override
  Future<T> withHashExclusiveLock<T>(String hash, Future<T> Function() action) {
    return AnalysisCache.withHashExclusiveLock(hash, action);
  }

  @override
  Future<AnalysisCacheSnapshot> snapshot({int maxBytes = 0}) =>
      AnalysisCache.snapshot(maxBytes: maxBytes);

  @override
  bool hasIncompleteContainer(String hash) =>
      AnalysisCache.hasIncompleteContainer(hash);

  @override
  Future<void> addEntry(String hash, String name, String videoPath) =>
      AnalysisCache.addEntry(hash, name, videoPath);

  @override
  String formatBytes(int bytes) => AnalysisCache.formatBytes(bytes);
}
