import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/ui/analysis_ui_selection.dart';

void main() {
  test('analysis frame PTS uses cache time base with integer rounding', () {
    final frame = _frame(pts: 3003, dts: 3000);
    const summary = AnalysisSummary(
      loaded: 1,
      frameCount: 1,
      packetCount: 1,
      naluCount: 1,
      videoWidth: 1920,
      videoHeight: 1080,
      timeBaseNum: 1,
      timeBaseDen: 90000,
      currentFrameIdx: 0,
      codec: 1,
    );

    expect(analysisFramePtsUs(frame, summary), 33367);
  });

  test(
    'analysis frame PTS falls back to DTS and rejects invalid time base',
    () {
      const noTimestamp = -9223372036854775808;
      final frame = _frame(pts: noTimestamp, dts: 180000);
      const summary = AnalysisSummary(
        loaded: 1,
        frameCount: 1,
        packetCount: 1,
        naluCount: 1,
        videoWidth: 1920,
        videoHeight: 1080,
        timeBaseNum: 1,
        timeBaseDen: 90000,
        currentFrameIdx: 0,
        codec: 1,
      );

      expect(analysisFramePtsUs(frame, summary), 2000000);
      expect(
        analysisFramePtsUs(
          frame,
          const AnalysisSummary(
            loaded: 1,
            frameCount: 1,
            packetCount: 1,
            naluCount: 1,
            videoWidth: 1920,
            videoHeight: 1080,
            timeBaseNum: 0,
            timeBaseDen: 0,
            currentFrameIdx: 0,
            codec: 1,
          ),
        ),
        isNull,
      );
    },
  );
}

FrameInfo _frame({required int pts, required int dts}) => FrameInfo(
  poc: 0,
  temporalId: 0,
  sliceType: 1,
  nalType: 1,
  avgQp: 20,
  numRefL0: 0,
  numRefL1: 0,
  refPocsL0: const [],
  refPocsL1: const [],
  pts: pts,
  dts: dts,
  packetSize: 100,
  keyframe: 0,
);
