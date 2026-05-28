import '../analysis/analysis_overlay.dart';

/// State assertions for test scripts.
sealed class PlayerAssert {
  const PlayerAssert();
}

class AssertPlaying extends PlayerAssert {
  const AssertPlaying();
}

class AssertPaused extends PlayerAssert {
  const AssertPaused();
}

class AssertPosition extends PlayerAssert {
  final int ptsUs;
  final int toleranceMs;
  const AssertPosition(this.ptsUs, this.toleranceMs);
}

class AssertPositionRange extends PlayerAssert {
  final int minUs;
  final int maxUs;
  const AssertPositionRange(this.minUs, this.maxUs);
}

class AssertTrackCount extends PlayerAssert {
  final int count;
  const AssertTrackCount(this.count);
}

class AssertTrackOrder extends PlayerAssert {
  final List<int> fileIds;
  const AssertTrackOrder(this.fileIds);
}

class AssertNativeBackend extends PlayerAssert {
  final String backend;
  final bool available;
  const AssertNativeBackend(this.backend, {required this.available});
}

class AssertTrackMetadata extends PlayerAssert {
  final int slot;
  final String formatName;
  final String decoderName;
  const AssertTrackMetadata({
    required this.slot,
    required this.formatName,
    required this.decoderName,
  });
}

class AssertPresentedFrameRange extends PlayerAssert {
  final int fileId;
  final int minUs;
  final int maxUs;
  const AssertPresentedFrameRange({
    required this.fileId,
    required this.minUs,
    required this.maxUs,
  });
}

class AssertNativeAudio extends PlayerAssert {
  final bool available;
  final int? sampleRate;
  final int? channels;
  final int? activeTrack;
  const AssertNativeAudio({
    required this.available,
    this.sampleRate,
    this.channels,
    this.activeTrack,
  });
}

class AssertNativeDiagnosticIntAtLeast extends PlayerAssert {
  final String key;
  final int minValue;
  const AssertNativeDiagnosticIntAtLeast(this.key, this.minValue);
}

class AssertNativeDiagnosticIntRange extends PlayerAssert {
  final String key;
  final int minValue;
  final int maxValue;
  const AssertNativeDiagnosticIntRange(this.key, this.minValue, this.maxValue);
}

class AssertNativeDiagnosticBool extends PlayerAssert {
  final String key;
  final bool value;
  const AssertNativeDiagnosticBool(this.key, this.value);
}

class AssertNativeDiagnosticString extends PlayerAssert {
  final String key;
  final String value;
  const AssertNativeDiagnosticString(this.key, this.value);
}

class AssertDuration extends PlayerAssert {
  final int ptsUs;
  final int toleranceMs;
  const AssertDuration(this.ptsUs, this.toleranceMs);
}

class AssertEffectiveDuration extends PlayerAssert {
  final int ptsUs;
  final int toleranceMs;
  const AssertEffectiveDuration(this.ptsUs, this.toleranceMs);
}

class AssertLayoutMode extends PlayerAssert {
  final int mode;
  const AssertLayoutMode(this.mode);
}

class AssertZoom extends PlayerAssert {
  final double ratio;
  final double tolerance;
  const AssertZoom(this.ratio, this.tolerance);
}

class AssertSplitPos extends PlayerAssert {
  final double position;
  final double tolerance;
  const AssertSplitPos(this.position, this.tolerance);
}

class AssertViewOffset extends PlayerAssert {
  final double x;
  final double y;
  final double tolerance;
  const AssertViewOffset(this.x, this.y, this.tolerance);
}

class AssertViewCenterStable extends PlayerAssert {
  final String baseline;
  final double tolerance;
  const AssertViewCenterStable(this.baseline, this.tolerance);
}

class AssertMainWindowBorderless extends PlayerAssert {
  const AssertMainWindowBorderless();
}

class AssertCaptureEquals extends PlayerAssert {
  final String expectedCapture;
  final String actualCapture;
  const AssertCaptureEquals(this.expectedCapture, this.actualCapture);
}

class AssertCaptureChanged extends PlayerAssert {
  final String beforeCapture;
  final String afterCapture;
  const AssertCaptureChanged(this.beforeCapture, this.afterCapture);
}

class AssertCaptureHash extends PlayerAssert {
  final String capture;
  final String hash;
  const AssertCaptureHash(this.capture, this.hash);
}

class AssertCaptureNotBlack extends PlayerAssert {
  final String capture;
  final double minNonBlackRatio;
  final double minAvgLuma;
  const AssertCaptureNotBlack(
    this.capture, {
    this.minNonBlackRatio = 0.01,
    this.minAvgLuma = 4.0,
  });
}

class AssertCaptureRegionNotBlack extends PlayerAssert {
  final String capture;
  final String region;
  final double minNonBlackRatio;
  final double minAvgLuma;

  const AssertCaptureRegionNotBlack(
    this.capture,
    this.region, {
    this.minNonBlackRatio = 0.01,
    this.minAvgLuma = 4.0,
  });
}

class AssertCaptureHasDetail extends PlayerAssert {
  final String capture;
  final double minLumaStdDev;

  const AssertCaptureHasDetail(this.capture, {this.minLumaStdDev = 4.0});
}

class AssertCaptureSplitDiff extends PlayerAssert {
  final String capture;
  final double maxMeanAbsChannel;
  final double maxMeanAbsLuma;
  final double maxMaxChannel;

  const AssertCaptureSplitDiff(
    this.capture, {
    this.maxMeanAbsChannel = double.infinity,
    this.maxMeanAbsLuma = double.infinity,
    this.maxMaxChannel = double.infinity,
  });
}

class AssertCaptureDiff extends PlayerAssert {
  final String expectedCapture;
  final String actualCapture;
  final double maxMeanAbsChannel;
  final double maxMeanAbsLuma;
  final double maxMaxChannel;

  const AssertCaptureDiff(
    this.expectedCapture,
    this.actualCapture, {
    this.maxMeanAbsChannel = double.infinity,
    this.maxMeanAbsLuma = double.infinity,
    this.maxMaxChannel = double.infinity,
  });
}

class AssertAnalysisProcessCount extends PlayerAssert {
  final int count;
  const AssertAnalysisProcessCount(this.count);
}

class AssertAnalysisFfiAvailable extends PlayerAssert {
  final bool available;
  const AssertAnalysisFfiAvailable(this.available);
}

class AssertAnalysisOverlay extends PlayerAssert {
  final bool active;
  final AnalysisOverlayType? type;
  final double? opacity;
  final double opacityTolerance;
  final int? trackCount;

  const AssertAnalysisOverlay({
    required this.active,
    this.type,
    this.opacity,
    this.opacityTolerance = 0.02,
    this.trackCount,
  });
}

class AssertTrackBufferCountBelow extends PlayerAssert {
  final int maxCount;
  const AssertTrackBufferCountBelow(this.maxCount);
}

class AssertResourceUsageBelow extends PlayerAssert {
  final double maxRssMb;
  final double maxDedicatedGpuMb;
  const AssertResourceUsageBelow(this.maxRssMb, this.maxDedicatedGpuMb);
}

class AssertResourceUsageDeltaBelow extends PlayerAssert {
  final String baseline;
  final double maxRssDeltaMb;
  final double maxDedicatedGpuDeltaMb;
  final double? maxPrivateDeltaMb;
  final double? maxKnownGpuDeltaMb;
  const AssertResourceUsageDeltaBelow(
    this.baseline,
    this.maxRssDeltaMb,
    this.maxDedicatedGpuDeltaMb, [
    this.maxPrivateDeltaMb,
    this.maxKnownGpuDeltaMb,
  ]);
}

class AssertNativeSeekCountDelta extends PlayerAssert {
  final String baseline;
  final int expectedDelta;
  const AssertNativeSeekCountDelta(this.baseline, this.expectedDelta);
}
