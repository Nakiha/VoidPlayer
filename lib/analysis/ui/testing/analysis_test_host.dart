import 'package:flutter/widgets.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';

abstract class AnalysisTestHost {
  bool get mounted;
  List<FrameInfo> get analysisFrames;
  List<NaluInfo> get analysisNalus;
  int get analysisFrameIndexBase;
  int get analysisNaluIndexBase;
  AnalysisSummary? get analysisSummary;
  AnalysisCodec get analysisCodec;
  int? get selectedAnalysisFrameIdx;
  int? get selectedAnalysisNaluIdx;
  double get analysisChartOffset;
  double get analysisVisibleFrameCount;
  int get analysisSelectedTab;
  bool get analysisPtsOrder;
  bool get analysisReferencePyramidActualTemporalLayers;
  bool get isAnalysisLoaded;

  void readAnalysisDataForTest();
  int? sortedPositionForFrameIdx(int frameIdx);
  void updateAnalysisTestState(VoidCallback update);
  void setAnalysisTabForTest(int tab);
  void setAnalysisOrderForTest(bool ptsOrder);
  void setAnalysisReferencePyramidLayerModeForTest(bool useActual);
  void setAnalysisChartWindowForTest(double offset, double visibleFrameCount);
  void selectAnalysisNaluForTest(int naluIdx);
}

class AnalysisTestHostRegistry extends ChangeNotifier {
  final Map<int, AnalysisTestHost> _hosts = <int, AnalysisTestHost>{};
  int? _selectedFileId;

  AnalysisTestHost? get activeHost {
    final selected = _selectedFileId;
    if (selected != null) {
      final host = _hosts[selected];
      if (host != null && host.mounted) return host;
    }
    for (final host in _hosts.values) {
      if (host.mounted) return host;
    }
    return null;
  }

  void register(int fileId, AnalysisTestHost host) {
    if (identical(_hosts[fileId], host)) return;
    _hosts[fileId] = host;
    notifyListeners();
  }

  void unregister(int fileId, AnalysisTestHost host) {
    if (!identical(_hosts[fileId], host)) return;
    _hosts.remove(fileId);
    notifyListeners();
  }

  void selectFileId(int? fileId) {
    if (_selectedFileId == fileId) return;
    _selectedFileId = fileId;
    notifyListeners();
  }

  Future<AnalysisTestHost> waitForActiveHost(Duration timeout) async {
    final stopwatch = Stopwatch()..start();
    while (stopwatch.elapsed < timeout) {
      final host = activeHost;
      if (host != null) return host;
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    throw AssertionError(
      'Timed out waiting for an active analysis page after '
      '${timeout.inMilliseconds}ms',
    );
  }
}
