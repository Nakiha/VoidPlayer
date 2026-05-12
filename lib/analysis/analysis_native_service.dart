import 'dart:isolate';

import 'package:path/path.dart' as p;

import 'analysis_ffi.dart';

abstract class AnalysisNativeService {
  Future<bool> load(String analysisPath);
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
  Future<bool> load(String analysisPath) {
    return Isolate.run(() {
      if (p.basename(analysisPath).toLowerCase() == 'base.vac') {
        final session = AnalysisSession.open(analysisPath);
        try {
          return session != null && session.summary.loaded != 0;
        } finally {
          session?.close();
        }
      }
      return AnalysisFfi.load(analysisPath);
    });
  }

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
      () => AnalysisFfi.generateVac2Base(videoPath, hash, maxCacheBytes),
    );
  }
}
