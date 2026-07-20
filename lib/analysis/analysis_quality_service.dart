import 'dart:isolate';

import 'analysis_ffi.dart';
import 'analysis_manager.dart';

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

abstract interface class AnalysisQualityFrameDataSource {
  Future<Map<int, FrameInfo>> readCachedFrames(
    String videoPath,
    List<AnalysisQualitySample> samples,
  );
}

class NativeAnalysisQualityService
    implements AnalysisQualityDataSource, AnalysisQualityFrameDataSource {
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

  @override
  Future<Map<int, FrameInfo>> readCachedFrames(
    String videoPath,
    List<AnalysisQualitySample> samples,
  ) {
    return AnalysisManager.instance.readCachedFrames(
      videoPath: videoPath,
      frameIndices: samples.map((sample) => sample.decodedFrameIndex),
      ptsUsByFrameIndex: {
        for (final sample in samples) sample.decodedFrameIndex: sample.ptsUs,
      },
    );
  }
}
