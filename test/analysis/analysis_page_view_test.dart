import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_ffi.dart';
import 'package:void_player/analysis/nalu_types.dart';
import 'package:void_player/analysis/ui/page/analysis_page_state.dart';
import 'package:void_player/analysis/ui/page/analysis_page_view.dart';
import 'package:void_player/l10n/app_localizations.dart';

void main() {
  testWidgets(
    'NALU browser uses the full lower panel until a unit is selected',
    (tester) async {
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
      expect(find.byKey(analysisNaluDetailPanelKey), findsOneWidget);
    },
  );
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
