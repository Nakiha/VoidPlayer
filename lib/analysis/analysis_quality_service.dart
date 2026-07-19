import 'dart:isolate';

import 'analysis_ffi.dart';

class AnalysisQualityRequest {
  final String videoPath;
  final int sampleIntervalUs;
  final int maxSamples;

  const AnalysisQualityRequest({
    required this.videoPath,
    this.sampleIntervalUs = 1000000,
    this.maxSamples = 0,
  });
}

abstract interface class AnalysisQualityDataSource {
  Future<AnalysisQualityReport> analyze(AnalysisQualityRequest request);
}

class NativeAnalysisQualityService implements AnalysisQualityDataSource {
  const NativeAnalysisQualityService();

  @override
  Future<AnalysisQualityReport> analyze(AnalysisQualityRequest request) {
    return Isolate.run(
      () => AnalysisQualityNative.analyzeSync(
        request.videoPath,
        sampleIntervalUs: request.sampleIntervalUs,
        maxSamples: request.maxSamples,
      ),
    );
  }
}
