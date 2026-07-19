import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/nalu_types.dart';
import 'package:void_player/analysis/ui/charts/analysis_frame_trend.dart';
import 'package:void_player/analysis/ui/page/analysis_page_state.dart';
import 'package:void_player/analysis/ui/page/analysis_page_view.dart';
import 'package:void_player/l10n/app_localizations.dart';

void main() {
  testWidgets('NALU browser always uses the full lower panel', (tester) async {
    final nalu = NaluInfo(
      offset: 0,
      size: 12,
      nalType: 7,
      temporalId: 0,
      layerId: 0,
      flags: 0,
    );

    Widget build(int? selectedNaluIdx) => MaterialApp(
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: AnalysisPageView(
        model: _model(nalu, selectedNaluIdx: selectedNaluIdx),
        actions: _actions,
      ),
    );

    await tester.pumpWidget(build(null));
    expect(find.byKey(analysisNaluBrowserPanelKey), findsOneWidget);
    expect(find.byKey(analysisNaluDetailPanelKey), findsNothing);
    expect(find.text('Select a NALU'), findsNothing);

    await tester.pumpWidget(build(0));
    await tester.pump();
    expect(find.byKey(analysisNaluBrowserPanelKey), findsOneWidget);
    expect(find.byKey(analysisNaluDetailPanelKey), findsNothing);
  });

  testWidgets('frame trend double click activates a frame', (tester) async {
    int? activated;
    final frames = List.generate(
      10,
      (index) => FrameInfo(
        poc: index,
        temporalId: 0,
        sliceType: 1,
        nalType: 1,
        avgQp: 20,
        numRefL0: 0,
        numRefL1: 0,
        refPocsL0: const [],
        refPocsL1: const [],
        pts: index * 3000,
        dts: index * 3000,
        packetSize: 100 + index,
        keyframe: index == 0 ? 1 : 0,
      ),
    );

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Builder(
          builder: (context) => SizedBox(
            width: 800,
            height: 300,
            child: AnalysisFrameTrendView(
              frames: frames,
              totalFrames: frames.length,
              currentIdx: 2,
              selectedFrameIdx: null,
              viewStart: 0,
              viewEnd: 10,
              frameSizeAxisZoom: 1,
              qpAxisZoom: 1,
              ptsOrder: true,
              onZoom: _ignoreDouble,
              onAxisZoom: _ignoreAxisZoom,
              onPan: _ignoreDouble,
              onFrameSelected: _ignoreNullableInt,
              onFrameActivated: (index) => activated = index,
              l: AppLocalizations.of(context)!,
            ),
          ),
        ),
      ),
    );

    final position = tester.getCenter(find.byType(AnalysisFrameTrendView));
    await tester.tapAt(position);
    await tester.pump(const Duration(milliseconds: 50));
    await tester.tapAt(position);
    await tester.pump(const Duration(milliseconds: 350));
    expect(activated, isNotNull);
    expect(activated, inInclusiveRange(0, 9));
  });
}

AnalysisPageViewModel _model(NaluInfo nalu, {required int? selectedNaluIdx}) {
  return AnalysisPageViewModel(
    selectedTab: 0,
    ptsOrder: true,
    selectedNaluIdx: selectedNaluIdx,
    naluFilter: '',
    naluBrowserWidth: 240,
    selectedFrameIdx: null,
    referencePyramidActualTemporalLayers: false,
    visibleFrameCount: 30,
    chartOffset: 0,
    frameSizeAxisZoom: 1,
    qpAxisZoom: 1,
    topPanelFraction: 0.5,
    frames: const [],
    frameIndexBase: 0,
    totalFrameCount: 0,
    frameBuckets: const [],
    frameBucketSize: 1,
    nalus: [nalu],
    naluIndexBase: 0,
    totalNaluCount: 1,
    sortedFrames: const [],
    sortedPocToIndices: const {},
    summary: null,
    codec: AnalysisCodec.h264,
    selectedSortedFrameIdx: null,
    currentSortedFrameIdx: 0,
  );
}

const AnalysisPageActions _actions = AnalysisPageActions(
  onOrderChanged: _ignoreBool,
  onTabChanged: _ignoreInt,
  onReferencePyramidLayerModeChanged: _ignoreBool,
  onChartZoom: _ignoreDouble,
  onChartPan: _ignoreDouble,
  onAxisZoom: _ignoreAxisZoom,
  onChartFrameSelected: _ignoreNullableInt,
  onChartFrameActivated: _ignoreInt,
  onNaluSelected: _ignoreNullableInt,
  onNaluWindowRequested: _ignoreWindow,
  onChartWindowSetForTest: _ignoreChartWindow,
  onNaluFilterChanged: _ignoreString,
  onNaluBrowserWidthChanged: _ignoreDouble,
  onTopPanelFractionChanged: _ignoreDouble,
);

void _ignoreBool(bool _) {}
void _ignoreInt(int _) {}
void _ignoreNullableInt(int? _) {}
void _ignoreDouble(double _) {}
void _ignoreString(String _) {}
void _ignoreAxisZoom(AnalysisFrameTrendAxis axis, double delta) {}
void _ignoreWindow(int start, int count) {}
void _ignoreChartWindow(double offset, double visibleFrameCount) {}
