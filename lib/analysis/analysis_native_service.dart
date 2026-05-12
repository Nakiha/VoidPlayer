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
  void unload() => AnalysisFfi.unload();

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
}
