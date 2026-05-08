int trackPtsEndUs({required int startTimeUs, required int durationUs}) {
  if (durationUs <= 0) return 0;
  if (startTimeUs <= 0) return durationUs;

  // Most containers expose duration as a span. Some FLV files expose a value
  // closer to the absolute end PTS; detect those so a -start offset maps the
  // track to its actual playable span instead of the full PTS epoch.
  if (durationUs > startTimeUs && durationUs - startTimeUs < durationUs ~/ 2) {
    return durationUs;
  }
  return startTimeUs + durationUs;
}

int trackPlayableDurationUs({
  required int startTimeUs,
  required int durationUs,
}) {
  final endUs = trackPtsEndUs(startTimeUs: startTimeUs, durationUs: durationUs);
  return (endUs - startTimeUs).clamp(0, 1 << 62).toInt();
}
