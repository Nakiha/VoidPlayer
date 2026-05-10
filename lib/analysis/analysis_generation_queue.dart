import 'analysis_cache_service.dart';
import 'analysis_native_service.dart';

abstract class AnalysisGenerationQueue {
  Future<bool> generate({
    required String videoPath,
    required String hash,
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
        return native.generateAnalysis(videoPath, hash, maxCacheBytes);
      });
    });
    _queue = task.then<void>((_) {}, onError: (_) {});
    return task;
  }
}
