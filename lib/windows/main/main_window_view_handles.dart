import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'main_window_state.dart';

class MainWindowViewHandles {
  final GlobalKey fullFrameCaptureKey;
  final GlobalKey viewportKey;
  final GlobalKey analysisOverlayButtonKey;
  final GlobalKey timelineSliderKey;
  final GlobalKey controlsBarKey;
  final GlobalKey loopRangeBarKey;
  final ValueListenable<TimelineHoverState> timelineHoverListenable;

  const MainWindowViewHandles({
    required this.fullFrameCaptureKey,
    required this.viewportKey,
    required this.analysisOverlayButtonKey,
    required this.timelineSliderKey,
    required this.controlsBarKey,
    required this.loopRangeBarKey,
    required this.timelineHoverListenable,
  });
}
