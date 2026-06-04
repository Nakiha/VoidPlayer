import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/native_player/native_player_api.dart';
import 'package:void_player/native_player/native_player_events.dart';
import 'package:void_player/windows/stats_window.dart';

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
