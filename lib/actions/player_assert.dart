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

class AssertTrackCount extends PlayerAssert {
  final int count;
  const AssertTrackCount(this.count);
}

class AssertDuration extends PlayerAssert {
  final int ptsUs;
  final int toleranceMs;
  const AssertDuration(this.ptsUs, this.toleranceMs);
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

class AssertViewOffset extends PlayerAssert {
  final double x;
  final double y;
  final double tolerance;
  const AssertViewOffset(this.x, this.y, this.tolerance);
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
