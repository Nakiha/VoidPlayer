import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/nalu_types.dart';
import 'package:void_player/analysis/ui/testing/analysis_test_executor.dart';
import 'package:void_player/analysis/ui/testing/analysis_test_host.dart';

void main() {
  test(
    'analysis executor updates the registered page through the narrow host',
    () async {
      final host = _FakeAnalysisTestHost();
      final executor = AnalysisTestExecutor(host);

      await executor.execute(
        const AnalysisTestCommand(AnalysisTestCommandType.setTab, [
          'frame_trend',
        ]),
      );
      await executor.execute(
        const AnalysisTestCommand(AnalysisTestCommandType.setOrder, ['dts']),
      );
      await executor.execute(
        const AnalysisTestCommand(AnalysisTestCommandType.setLayerMode, [
          'actual',
        ]),
      );
      await executor.execute(
        const AnalysisTestCommand(AnalysisTestCommandType.assertCodec, [
          'h264',
        ]),
      );

      expect(host.analysisSelectedTab, 1);
      expect(host.analysisPtsOrder, isFalse);
      expect(host.analysisReferencePyramidActualTemporalLayers, isTrue);
      expect(host.updateCount, 3);
    },
  );

  test(
    'analysis host registry follows workspace selection and unregisters',
    () {
      final registry = AnalysisTestHostRegistry();
      addTearDown(registry.dispose);
      final first = _FakeAnalysisTestHost();
      final second = _FakeAnalysisTestHost();

      registry
        ..register(10, first)
        ..register(20, second)
        ..selectFileId(20);
      expect(registry.activeHost, same(second));

      registry.unregister(20, second);
      expect(registry.activeHost, same(first));
    },
  );
}

class _FakeAnalysisTestHost implements AnalysisTestHost {
  int updateCount = 0;

  @override
  bool get mounted => true;

  @override
  List<FrameInfo> get analysisFrames => const [];

  @override
  List<NaluInfo> get analysisNalus => const [];

  @override
  int get analysisFrameIndexBase => 0;

  @override
  int get analysisNaluIndexBase => 0;

  @override
  AnalysisSummary? get analysisSummary => null;

  @override
  AnalysisCodec get analysisCodec => AnalysisCodec.h264;

  @override
  int? get selectedAnalysisFrameIdx => null;

  @override
  int? get selectedAnalysisNaluIdx => null;

  @override
  double get analysisChartOffset => 0;

  @override
  double get analysisVisibleFrameCount => 100;

  @override
  int analysisSelectedTab = 0;

  @override
  bool analysisPtsOrder = true;

  @override
  bool analysisReferencePyramidActualTemporalLayers = false;

  @override
  bool get isAnalysisLoaded => false;

  @override
  void readAnalysisDataForTest() {}

  @override
  int? sortedPositionForFrameIdx(int frameIdx) => frameIdx;

  @override
  void updateAnalysisTestState(VoidCallback update) {
    updateCount++;
    update();
  }

  @override
  void setAnalysisTabForTest(int tab) {
    analysisSelectedTab = tab;
  }

  @override
  void setAnalysisOrderForTest(bool ptsOrder) {
    analysisPtsOrder = ptsOrder;
  }

  @override
  void setAnalysisReferencePyramidLayerModeForTest(bool useActual) {
    analysisReferencePyramidActualTemporalLayers = useActual;
  }

  @override
  void setAnalysisChartWindowForTest(double offset, double visibleFrameCount) {}

  @override
  void selectAnalysisNaluForTest(int naluIdx) {}
}
