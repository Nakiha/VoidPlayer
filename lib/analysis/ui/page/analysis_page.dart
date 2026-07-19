import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';
import '../analysis_ui_selection.dart';
import '../testing/analysis_test_host.dart';
import '../widgets/analysis_split_layout_controller.dart';
import 'analysis_page_controller.dart';
import 'analysis_page_view.dart';

class AnalysisPage extends StatefulWidget {
  final int fileId;
  final String hash;
  final AnalysisTestHostRegistry testHosts;
  final bool pollSummary;
  final AnalysisSplitLayoutController? splitLayoutController;
  final ValueChanged<AnalysisUiSelection?>? onSelectionChanged;

  const AnalysisPage({
    super.key,
    required this.fileId,
    required this.hash,
    required this.testHosts,
    this.pollSummary = true,
    this.splitLayoutController,
    this.onSelectionChanged,
  });

  @override
  State<AnalysisPage> createState() => AnalysisPageState();
}

class AnalysisPageState extends State<AnalysisPage>
    implements AnalysisTestHost {
  late AnalysisPageController _controller;
  Object? _lastPublishedSelectionIdentity;

  @override
  void initState() {
    super.initState();
    _controller = _createController();
    _controller.addListener(_publishSelection);
    widget.testHosts.register(widget.fileId, this);
    widget.splitLayoutController?.addListener(_onSplitLayoutChanged);
  }

  @override
  void didUpdateWidget(covariant AnalysisPage oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.splitLayoutController != widget.splitLayoutController) {
      oldWidget.splitLayoutController?.removeListener(_onSplitLayoutChanged);
      widget.splitLayoutController?.addListener(_onSplitLayoutChanged);
    }
    if (oldWidget.fileId != widget.fileId ||
        oldWidget.testHosts != widget.testHosts) {
      oldWidget.testHosts.unregister(oldWidget.fileId, this);
      widget.testHosts.register(widget.fileId, this);
    }
    if (oldWidget.hash != widget.hash ||
        oldWidget.pollSummary != widget.pollSummary) {
      _controller.removeListener(_publishSelection);
      _controller.dispose();
      _controller = _createController();
      _controller.addListener(_publishSelection);
      _lastPublishedSelectionIdentity = null;
    } else if (oldWidget.onSelectionChanged != widget.onSelectionChanged) {
      _publishSelection();
    }
  }

  @override
  void dispose() {
    widget.testHosts.unregister(widget.fileId, this);
    widget.splitLayoutController?.removeListener(_onSplitLayoutChanged);
    _controller.removeListener(_publishSelection);
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _controller,
      builder: (context, _) {
        return AnalysisPageView(
          model: _controller.viewModel,
          actions: _controller.actions,
          splitLayoutController: widget.splitLayoutController,
        );
      },
    );
  }

  AnalysisPageController _createController() {
    final controller = AnalysisPageController(
      hash: widget.hash,
      pollSummary: widget.pollSummary,
    );
    controller.start();
    return controller;
  }

  void _onSplitLayoutChanged() {
    if (mounted) setState(() {});
  }

  void _publishSelection() {
    final callback = widget.onSelectionChanged;
    if (callback == null) return;
    final selection = _selectionSnapshot();
    final identity = selection?.identity;
    if (_lastPublishedSelectionIdentity == identity) return;
    _lastPublishedSelectionIdentity = identity;
    callback(selection);
  }

  AnalysisUiSelection? _selectionSnapshot() {
    final frameIndex = _controller.selectedFrameIdx;
    FrameInfo? frame;
    if (frameIndex != null) {
      final frameOffset = frameIndex - _controller.frameIndexBase;
      if (frameOffset >= 0 && frameOffset < _controller.frames.length) {
        frame = _controller.frames[frameOffset];
      }
    }
    final naluIndex = _controller.selectedNaluIdx;
    if (naluIndex != null) {
      final naluOffset = naluIndex - _controller.naluIndexBase;
      if (naluOffset >= 0 && naluOffset < _controller.nalus.length) {
        return AnalysisNaluSelection(
          fileId: widget.fileId,
          frameIndex: frameIndex,
          frame: frame,
          codec: _controller.codec,
          naluIndex: naluIndex,
          nalu: _controller.nalus[naluOffset],
        );
      }
    }
    if (frameIndex == null || frame == null) return null;
    return AnalysisFrameSelection(
      fileId: widget.fileId,
      frameIndex: frameIndex,
      frame: frame,
      codec: _controller.codec,
    );
  }

  @override
  void updateAnalysisTestState(VoidCallback update) {
    update();
    if (mounted) setState(() {});
  }

  @override
  List<FrameInfo> get analysisFrames => _controller.frames;

  @override
  List<NaluInfo> get analysisNalus => _controller.nalus;

  @override
  int get analysisFrameIndexBase => _controller.frameIndexBase;

  @override
  int get analysisNaluIndexBase => _controller.naluIndexBase;

  @override
  AnalysisSummary? get analysisSummary => _controller.summary;

  @override
  AnalysisCodec get analysisCodec => _controller.codec;

  @override
  int? get selectedAnalysisFrameIdx => _controller.selectedFrameIdx;

  @override
  int? get selectedAnalysisNaluIdx => _controller.selectedNaluIdx;

  @override
  double get analysisChartOffset => _controller.chartOffset;

  @override
  double get analysisVisibleFrameCount => _controller.visibleFrameCount;

  @override
  int get analysisSelectedTab => _controller.selectedTab;

  @override
  bool get analysisPtsOrder => _controller.ptsOrder;

  @override
  bool get analysisReferencePyramidActualTemporalLayers =>
      _controller.referencePyramidActualTemporalLayers;

  @override
  bool get isAnalysisLoaded => _controller.isLoaded;

  @override
  void readAnalysisDataForTest() => _controller.loadDataForTest();

  @override
  int? sortedPositionForFrameIdx(int frameIdx) {
    return _controller.sortedPositionForFrameIdx(frameIdx);
  }

  @override
  void setAnalysisTabForTest(int tab) => _controller.setTab(tab);

  @override
  void setAnalysisOrderForTest(bool ptsOrder) {
    _controller.setPtsOrder(ptsOrder);
  }

  @override
  void setAnalysisReferencePyramidLayerModeForTest(bool useActual) {
    _controller.setReferencePyramidActualTemporalLayers(useActual);
  }

  @override
  void setAnalysisChartWindowForTest(double offset, double visibleFrameCount) {
    _controller.setChartWindowForTest(offset, visibleFrameCount);
  }

  @override
  void selectAnalysisNaluForTest(int naluIdx) {
    _controller.selectNaluForTest(naluIdx);
  }
}
