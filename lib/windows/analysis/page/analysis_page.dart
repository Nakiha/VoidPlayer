import 'dart:async';

import 'package:flutter/material.dart';

import '../../../analysis/analysis_ffi.dart';
import '../../../analysis/nalu_types.dart';
import '../testing/analysis_test_host.dart';
import '../testing/analysis_test_runner.dart';
import '../widgets/analysis_split_layout_controller.dart';
import 'analysis_page_controller.dart';
import 'analysis_page_view.dart';

class AnalysisPage extends StatefulWidget {
  final String hash;
  final String? testScriptPath;
  final bool pollSummary;
  final AnalysisSplitLayoutController? splitLayoutController;

  const AnalysisPage({
    super.key,
    required this.hash,
    this.testScriptPath,
    this.pollSummary = true,
    this.splitLayoutController,
  });

  @override
  State<AnalysisPage> createState() => AnalysisPageState();
}

class AnalysisPageState extends State<AnalysisPage>
    implements AnalysisTestHost {
  late AnalysisPageController _controller;
  bool _testStarted = false;

  @override
  void initState() {
    super.initState();
    _controller = _createController();
    widget.splitLayoutController?.addListener(_onSplitLayoutChanged);
    _maybeStartTestRunner();
  }

  @override
  void didUpdateWidget(covariant AnalysisPage oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.splitLayoutController != widget.splitLayoutController) {
      oldWidget.splitLayoutController?.removeListener(_onSplitLayoutChanged);
      widget.splitLayoutController?.addListener(_onSplitLayoutChanged);
    }
    if (oldWidget.hash != widget.hash ||
        oldWidget.pollSummary != widget.pollSummary) {
      _controller.dispose();
      _controller = _createController();
      _testStarted = false;
      _maybeStartTestRunner();
    } else if (oldWidget.testScriptPath != widget.testScriptPath) {
      _testStarted = false;
      _maybeStartTestRunner();
    }
  }

  @override
  void dispose() {
    widget.splitLayoutController?.removeListener(_onSplitLayoutChanged);
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

  void _maybeStartTestRunner() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final scriptPath = widget.testScriptPath;
      if (!mounted || scriptPath == null || _testStarted) return;
      _testStarted = true;
      unawaited(runAnalysisTestScript(scriptPath));
    });
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
  NakiAnalysisSummary? get analysisSummary => _controller.summary;

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
  void selectAnalysisNaluForTest(int naluIdx) {
    _controller.selectNaluForTest(naluIdx);
  }
}
