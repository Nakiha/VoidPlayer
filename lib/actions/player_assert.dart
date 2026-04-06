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
