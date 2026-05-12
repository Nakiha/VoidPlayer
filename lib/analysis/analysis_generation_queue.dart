import 'analysis_cache_service.dart';
import 'analysis_native_service.dart';

abstract class AnalysisGenerationQueue {
  Future<bool> generate({
    required String videoPath,
    required String hash,
    required int maxCacheBytes,
  });

  Future<bool> generateOverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  });
}

class SerialAnalysisGenerationQueue implements AnalysisGenerationQueue {
  final AnalysisCacheService cache;
  final AnalysisNativeService native;

  Future<void> _queue = Future<void>.value();

  SerialAnalysisGenerationQueue({required this.cache, required this.native});

  @override
  Future<bool> generate({
    required String videoPath,
    required String hash,
    required int maxCacheBytes,
  }) {
    final previous = _queue;
    final task = previous.catchError((_) {}).then((_) {
      return cache.withHashExclusiveLock(hash, () async {
        return native.generateVac2Base(videoPath, hash, maxCacheBytes);
      });
    });
    _queue = task.then<void>((_) {}, onError: (_) {});
    return task;
  }

  @override
  Future<bool> generateOverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  }) {
    final previous = _queue;
    final task = previous.catchError((_) {}).then((_) {
      return cache.withHashExclusiveLock(hash, () async {
        return native.generateOverlayChunk(
          videoPath: videoPath,
          hash: hash,
          startFrame: startFrame,
          endFrame: endFrame,
          maxCacheBytes: maxCacheBytes,
        );
      });
    });
    _queue = task.then<void>((_) {}, onError: (_) {});
    return task;
  }
}
