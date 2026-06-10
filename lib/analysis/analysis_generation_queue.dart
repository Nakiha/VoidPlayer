import '../app_log.dart';
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

  Future<void> _baseQueue = Future<void>.value();
  final Map<String, Future<void>> _exclusiveByHash = {};

  SerialAnalysisGenerationQueue({required this.cache, required this.native});

  @override
  Future<bool> generate({
    required String videoPath,
    required String hash,
    required int maxCacheBytes,
  }) {
    final previous = _baseQueue;
    late final Future<bool> task;
    late final Future<void> exclusiveMarker;
    task = previous
        .catchError((Object error, StackTrace stack) {
          log.warning('previous base analysis generation failed', error, stack);
        })
        .then((_) {
          return cache.withHashExclusiveLock(hash, () async {
            return native.generateVac2Base(videoPath, hash, maxCacheBytes);
          });
        })
        .whenComplete(() {
          if (identical(_exclusiveByHash[hash], exclusiveMarker)) {
            _exclusiveByHash.remove(hash);
          }
        });
    exclusiveMarker = _observeTaskCompletion(
      task,
      'base analysis generation marker completed after failure',
    );
    _exclusiveByHash[hash] = exclusiveMarker;
    _baseQueue = _observeTaskCompletion(
      task,
      'base analysis queue advanced after failure',
    );
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
    final waitForBase = _exclusiveByHash[hash] ?? Future<void>.value();
    return waitForBase
        .catchError((Object error, StackTrace stack) {
          log.warning(
            'base analysis generation failed before overlay chunk',
            error,
            stack,
          );
        })
        .then((_) {
          return cache.withHashSharedLock(hash, () async {
            return native.generateOverlayChunk(
              videoPath: videoPath,
              hash: hash,
              startFrame: startFrame,
              endFrame: endFrame,
              maxCacheBytes: maxCacheBytes,
            );
          });
        });
  }

  Future<void> _observeTaskCompletion<T>(Future<T> task, String label) {
    return task.then<void>(
      (_) {},
      onError: (Object error, StackTrace stack) {
        log.fine(label, error, stack);
      },
    );
  }
}
