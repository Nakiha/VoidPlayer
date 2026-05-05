import 'dart:isolate';

import 'analysis_ffi.dart';

abstract class AnalysisNativeService {
  bool load(String analysisPath);
  void unload();
  AnalysisSession? openSession(String analysisPath);
  Future<bool> generateAnalysis(
    String videoPath,
    String hash,
    int maxCacheBytes,
  );
}

class DefaultAnalysisNativeService implements AnalysisNativeService {
  const DefaultAnalysisNativeService();

  @override
  bool load(String analysisPath) => AnalysisFfi.load(analysisPath);

  @override
  void unload() => AnalysisFfi.unload();

  @override
  AnalysisSession? openSession(String analysisPath) =>
      AnalysisSession.open(analysisPath);

  @override
  Future<bool> generateAnalysis(
    String videoPath,
    String hash,
    int maxCacheBytes,
  ) {
    return Isolate.run(
      () => AnalysisFfi.generateAnalysis(videoPath, hash, maxCacheBytes),
    );
  }
}
