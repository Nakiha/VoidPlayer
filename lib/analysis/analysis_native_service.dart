import 'dart:async';
import 'dart:isolate';

import 'analysis_ffi.dart';

abstract class AnalysisNativeService {
  Future<bool> load(String analysisPath);
  void unload();
  AnalysisSession? openSession(String analysisPath);
  Future<bool> generateVac2Base(
    String videoPath,
    String hash,
    int maxCacheBytes,
  );
  Future<bool> generateOverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  });
  AnalysisGenerationServiceStats? generationServiceStats();
}

class DefaultAnalysisNativeService implements AnalysisNativeService {
  const DefaultAnalysisNativeService();

  @override
  Future<bool> load(String analysisPath) {
    return Isolate.run(() {
      final session = AnalysisSession.open(analysisPath);
      try {
        return session != null && session.summary.loaded != 0;
      } finally {
        session?.close();
      }
    });
  }

  @override
  void unload() {}

  @override
  AnalysisSession? openSession(String analysisPath) =>
      AnalysisSession.open(analysisPath);

  @override
  Future<bool> generateVac2Base(
    String videoPath,
    String hash,
    int maxCacheBytes,
  ) {
    return Isolate.run(
      () => AnalysisFfi.generateVac2Base(videoPath, hash, maxCacheBytes),
    );
  }

  @override
  Future<bool> generateOverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  }) {
    if (AnalysisFfi.hasGenerationService) {
      return _NativeAnalysisGenerationPoller.instance.generateOverlayChunk(
        videoPath: videoPath,
        hash: hash,
        startFrame: startFrame,
        endFrame: endFrame,
        maxCacheBytes: maxCacheBytes,
      );
    }
    return Isolate.run(
      () => AnalysisFfi.generateVac2OverlayChunk(
        videoPath: videoPath,
        hash: hash,
        startFrame: startFrame,
        endFrame: endFrame,
        maxCacheBytes: maxCacheBytes,
      ),
    );
  }

  @override
  AnalysisGenerationServiceStats? generationServiceStats() =>
      AnalysisFfi.generationServiceStats();
}

class _NativeAnalysisGenerationPoller {
  _NativeAnalysisGenerationPoller._();

  static final instance = _NativeAnalysisGenerationPoller._();

  final Map<int, Completer<bool>> _jobs = {};
  Timer? _timer;

  Future<bool> generateOverlayChunk({
    required String videoPath,
    required String hash,
    required int startFrame,
    required int endFrame,
    required int maxCacheBytes,
  }) {
    final jobId = AnalysisFfi.submitVac2OverlayChunk(
      videoPath: videoPath,
      hash: hash,
      startFrame: startFrame,
      endFrame: endFrame,
      maxCacheBytes: maxCacheBytes,
      priority: 0,
    );
    if (jobId == 0) return Future.value(false);
    final existing = _jobs[jobId];
    if (existing != null) return existing.future;
    final completer = Completer<bool>();
    _jobs[jobId] = completer;
    _ensurePolling();
    return completer.future;
  }

  void _ensurePolling() {
    if (_timer != null) return;
    _timer = Timer.periodic(const Duration(milliseconds: 50), (_) => _poll());
    _poll();
  }

  void _poll() {
    List<AnalysisGenerationJobResult> results;
    try {
      results = AnalysisFfi.pollGenerationJobs();
    } catch (_) {
      for (final completer in _jobs.values) {
        if (!completer.isCompleted) completer.complete(false);
      }
      _jobs.clear();
      _stopIfIdle();
      return;
    }
    for (final result in results) {
      final completer = _jobs.remove(result.jobId);
      if (completer != null && !completer.isCompleted) {
        completer.complete(result.ok);
      }
    }
    _stopIfIdle();
  }

  void _stopIfIdle() {
    if (_jobs.isNotEmpty) return;
    _timer?.cancel();
    _timer = null;
  }
}
