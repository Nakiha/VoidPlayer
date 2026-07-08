import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/native_player/native_player_api.dart';
import 'package:void_player/native_player/native_player_events.dart';
import 'package:void_player/performance/performance_health.dart';
import 'package:void_player/windows/stats_window.dart';

PerformanceHealthSnapshot _health({
  PerformanceHealthLevel level = PerformanceHealthLevel.ok,
  PerformanceHealthKind kind = PerformanceHealthKind.ok,
  double displayRefreshHz = 120,
  double displayTickHz = 120,
  double nativeCompositorCompositeHz = 0,
  double nativeCompositorSourceCacheHz = 0,
  double nativeCompositorSourceProjectionHz = 0,
  double drawP95Us = 1800,
  double metalP95Us = 1800,
  String presentationBackend = 'unknown',
  int metalBufferExhaustionCount = 0,
  int largeGapCount = 0,
  bool playing = false,
  int trackCount = 1,
}) {
  return PerformanceHealthSnapshot(
    level: level,
    kind: kind,
    reason: '',
    presentationBackend: presentationBackend,
    displayRefreshHz: displayRefreshHz,
    displayTickHz: displayTickHz,
    layoutDrawHz: 0,
    layoutIntentHz: 0,
    nativeCompositorCompositeHz: nativeCompositorCompositeHz,
    nativeCompositorSourceCacheHz: nativeCompositorSourceCacheHz,
    nativeCompositorSourceProjectionHz: nativeCompositorSourceProjectionHz,
    drawP95Us: drawP95Us,
    backendP95Us: 0,
    metalP95Us: metalP95Us,
    hostIntervalP95Ms: 0,
    playbackCadenceRatio: 0,
    presentedFrameRateHz: 0,
    expectedFrameRateHz: 0,
    metalBufferExhaustionCount: metalBufferExhaustionCount,
    metalBufferExhaustionDelta: 0,
    metalFailureCount: 0,
    metalFailureDelta: 0,
    largeGapCount: largeGapCount,
    largeGapDelta: 0,
    monotonicViolationCount: 0,
    presentedFramePtsDistinctCount: 0,
    presentedFramePtsAdvanceUs: 0,
    presentedFrameExpectedIntervalUs: 0,
    playing: playing,
    trackCount: trackCount,
  );
}

void main() {
  test('native diagnostics stats source maps macOS per-track stats', () async {
    final source = NativeDiagnosticsStatsDataSource(
      _FakeNativePlayerApi({
        'processRssBytes': 8192,
        'processPrivateBytes': 6144,
        'nativeCpuFrameMemoryBytes': 4096,
        'nativePacketQueueMemoryBytes': 2048,
        'nativeTrackDiagnostics': [
          {
            'fileId': 7,
            'decodeFps': 59.8,
            'decodeAvgMs': 2.5,
            'decodeMaxMs': 7.25,
            'bufferCount': 3,
            'bufferCapacity': 8,
            'bufferState': 2,
            'cpuFrameMemoryBytes': 1024,
            'packetQueueMemoryBytes': 512,
            'currentPtsUs': 123000,
            'currentDtsUs': 120000,
          },
        ],
      }),
    );

    final snapshot = await source.load();

    expect(snapshot, isNotNull);
    expect(snapshot!.memory.workingSetBytes, 8192);
    expect(snapshot.memory.privateBytes, 6144);
    expect(snapshot.memory.cpuFrameBytes, 4096);
    expect(snapshot.memory.packetQueueBytes, 2048);
    expect(snapshot.tracks, hasLength(1));
    expect(snapshot.tracks.single.fileId, 7);
    expect(snapshot.tracks.single.fps, closeTo(59.8, 0.001));
    expect(snapshot.tracks.single.bufferCount, 3);
    expect(snapshot.tracks.single.bufferCapacity, 8);
    expect(snapshot.tracks.single.cpuFrameMemoryBytes, 1024);
    expect(snapshot.tracks.single.packetQueueMemoryBytes, 512);
    expect(snapshot.tracks.single.currentPtsUs, 123000);
  });

  test(
    'native diagnostics stats source uses presentation cadence for fps',
    () async {
      final source = NativeDiagnosticsStatsDataSource(
        _FakeNativePlayerApi({
          'trackCount': 1,
          'isPlaying': true,
          'presentedFramePtsDistinctCount': 131,
          'presentedFramePtsAdvanceUs': 6030000,
          'presentedFrameExpectedIntervalUs': 33333,
          'nativeTrackDiagnostics': [
            {
              'fileId': 7,
              'decodeFps': 0.0,
              'bufferCount': 4,
              'bufferCapacity': 4,
              'bufferState': 2,
            },
          ],
        }),
      );

      final snapshot = await source.load();

      expect(snapshot, isNotNull);
      expect(
        snapshot!.health.kind,
        PerformanceHealthKind.playbackCadencePressure,
      );
      expect(snapshot.tracks.single.fps, closeTo(21.6, 0.2));
    },
  );

  test('track diagnostics keep per-track presentation fps when available', () {
    final row = StatsTrackRow.fromDiagnostics({
      'fileId': 1,
      'presentationFps': 29.97,
      'decodeFps': 0.0,
    }, fallbackFps: 14.0);

    expect(row.fps, closeTo(29.97, 0.001));
  });

  testWidgets(
    'memory summary fits a compact profiler panel without scrolling',
    (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('zh'),
          localizationsDelegates: const [
            AppLocalizations.delegate,
            GlobalMaterialLocalizations.delegate,
            GlobalWidgetsLocalizations.delegate,
            GlobalCupertinoLocalizations.delegate,
          ],
          supportedLocales: AppLocalizations.supportedLocales,
          home: const SizedBox(
            width: 560,
            child: StatsMemorySummarySection(
              memory: StatsMemorySummary(
                workingSetBytes: 255433113,
                privateBytes: 268016025,
                dedicatedGpuBytes: 122683392,
                cpuFrameBytes: 0,
                packetQueueBytes: 5033164,
              ),
            ),
          ),
        ),
      );

      expect(find.byType(SingleChildScrollView), findsNothing);
      expect(find.text('RSS'), findsOneWidget);
      expect(find.text('私有'), findsOneWidget);
      expect(find.text('GPU帧'), findsOneWidget);
      expect(find.text('CPU帧'), findsOneWidget);
      expect(find.text('包队列'), findsOneWidget);
    },
  );

  testWidgets('health summary localizes track count', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('zh'),
        localizationsDelegates: const [
          AppLocalizations.delegate,
          GlobalMaterialLocalizations.delegate,
          GlobalWidgetsLocalizations.delegate,
          GlobalCupertinoLocalizations.delegate,
        ],
        supportedLocales: AppLocalizations.supportedLocales,
        home: SizedBox(
          width: 560,
          child: StatsHealthSummarySection(health: _health(trackCount: 1)),
        ),
      ),
    );

    expect(find.text('1 条轨道'), findsOneWidget);
    expect(find.text('1 tracks'), findsNothing);
  });

  testWidgets('health summary localizes metric labels', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('zh'),
        localizationsDelegates: const [
          AppLocalizations.delegate,
          GlobalMaterialLocalizations.delegate,
          GlobalWidgetsLocalizations.delegate,
          GlobalCupertinoLocalizations.delegate,
        ],
        supportedLocales: AppLocalizations.supportedLocales,
        home: SizedBox(
          width: 560,
          child: StatsHealthSummarySection(
            health: _health(
              nativeCompositorCompositeHz: 108,
              drawP95Us: 2100,
              metalP95Us: 2000,
            ),
          ),
        ),
      ),
    );

    expect(find.textContaining('合成 108/120Hz'), findsOneWidget);
    expect(find.textContaining('显示 120/120Hz'), findsOneWidget);
    expect(find.textContaining('绘制 p95 2.1ms'), findsOneWidget);
    expect(find.textContaining('compositor'), findsNothing);
    expect(find.textContaining('display-link'), findsNothing);
  });

  testWidgets('health summary labels Metal backend latency', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('zh'),
        localizationsDelegates: const [
          AppLocalizations.delegate,
          GlobalMaterialLocalizations.delegate,
          GlobalWidgetsLocalizations.delegate,
          GlobalCupertinoLocalizations.delegate,
        ],
        supportedLocales: AppLocalizations.supportedLocales,
        home: SizedBox(
          width: 560,
          child: StatsHealthSummarySection(
            health: _health(
              presentationBackend: 'native-metal-cvpixelbuffer-target',
              metalP95Us: 2400,
            ),
          ),
        ),
      ),
    );

    expect(find.textContaining('Metal p95 2.4ms'), findsOneWidget);
  });

  testWidgets('health summary keeps a stable height across metric counts', (
    tester,
  ) async {
    Widget build(PerformanceHealthSnapshot health) {
      return MaterialApp(
        locale: const Locale('zh'),
        localizationsDelegates: const [
          AppLocalizations.delegate,
          GlobalMaterialLocalizations.delegate,
          GlobalWidgetsLocalizations.delegate,
          GlobalCupertinoLocalizations.delegate,
        ],
        supportedLocales: AppLocalizations.supportedLocales,
        home: SizedBox(
          width: 560,
          child: StatsHealthSummarySection(health: health),
        ),
      );
    }

    await tester.pumpWidget(build(_health(drawP95Us: 0, metalP95Us: 0)));
    final compactHeight = tester
        .getSize(find.byType(StatsHealthSummarySection))
        .height;

    await tester.pumpWidget(
      build(
        _health(
          nativeCompositorCompositeHz: 108,
          nativeCompositorSourceCacheHz: 60,
          nativeCompositorSourceProjectionHz: 120,
          metalBufferExhaustionCount: 92,
          playing: true,
          largeGapCount: 2,
        ),
      ),
    );
    final detailedHeight = tester
        .getSize(find.byType(StatsHealthSummarySection))
        .height;

    expect(detailedHeight, compactHeight);
  });
}

class _FakeNativePlayerApi implements NativePlayerApi {
  final Map<String, dynamic> diagnostics;

  _FakeNativePlayerApi(this.diagnostics);

  @override
  Stream<NativePlayerEvent> get events => const Stream.empty();

  @override
  Future<Map<String, dynamic>> getDiagnostics() async => diagnostics;

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}
