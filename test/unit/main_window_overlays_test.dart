import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/main_window/main_window_overlays.dart';
import 'package:void_player/native_player/native_player_protocol.dart';
import 'package:void_player/platform/path_launcher.dart';
import 'package:void_player/track_manager.dart';

void _setViewportSize(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

const _twoTracks = [
  TrackEntry(
    TrackInfo(
      fileId: 1,
      slot: 0,
      path: 'h264_9s_1920x1080.mp4',
      width: 1920,
      height: 1080,
      durationUs: 9999000,
      formatName: 'QuickTime / MOV',
      codecName: 'h264',
      codecLongName: 'H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10',
      decoderName: 'VideoToolbox / h264',
      bitRate: 15460000,
    ),
  ),
  TrackEntry(
    TrackInfo(
      fileId: 2,
      slot: 1,
      path: 'h265_10s_1920x1080.mp4',
      width: 1920,
      height: 1080,
      durationUs: 9949000,
      formatName: 'QuickTime / MOV',
      codecName: 'hevc',
      codecLongName: 'H.265 / HEVC (High Efficiency Video Coding)',
      decoderName: 'VideoToolbox / hevc',
      bitRate: 8180000,
    ),
  ),
];

class _FakePathLauncher implements PathLauncher {
  final List<String> locatedPaths = [];

  @override
  bool get isAvailable => true;

  @override
  Future<void> locateFile(String path) async {
    locatedPaths.add(path);
  }

  @override
  Future<void> openFolder(String path) async {}
}

void main() {
  testWidgets('floating side panels leave uncovered viewport interactive', (
    tester,
  ) async {
    var taps = 0;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: Stack(
            children: [
              Positioned.fill(
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTap: () => taps++,
                ),
              ),
              FloatingSidePanelsSlot(
                mediaInfoVisible: false,
                profilerVisible: true,
                tracks: const [],
                onCloseMediaInfo: () {},
                onCloseProfiler: () {},
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 190));

    await tester.tapAt(const Offset(760, 64));

    expect(taps, 1);
  });

  testWidgets('stacked side panels leave profiler side gutter draggable', (
    tester,
  ) async {
    _setViewportSize(tester, const Size(1366, 768));
    var dragUpdates = 0;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: Stack(
            children: [
              Positioned.fill(
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onPanUpdate: (_) => dragUpdates++,
                ),
              ),
              FloatingSidePanelsSlot(
                mediaInfoVisible: true,
                profilerVisible: true,
                tracks: _twoTracks,
                onCloseMediaInfo: () {},
                onCloseProfiler: () {},
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 190));

    await tester.dragFrom(const Offset(760, 360), const Offset(80, 0));

    expect(dragUpdates, greaterThan(0));
  });

  testWidgets('media info table fills expanded panel width', (tester) async {
    _setViewportSize(tester, const Size(2048, 768));
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: Stack(
            children: [
              FloatingSidePanelsSlot(
                mediaInfoVisible: true,
                profilerVisible: false,
                tracks: _twoTracks,
                onCloseMediaInfo: () {},
                onCloseProfiler: () {},
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 190));

    final mediaInfoWidth = tester.getSize(find.byType(MediaInfoPage)).width;
    final tableWidth = tester.getSize(find.byType(DataTable)).width;
    expect(mediaInfoWidth, greaterThan(700));
    expect(tableWidth, closeTo(mediaInfoWidth, 1));
  });

  testWidgets('media info locate button delegates to path launcher', (
    tester,
  ) async {
    _setViewportSize(tester, const Size(1600, 600));
    final launcher = _FakePathLauncher();
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: MediaInfoPage(
            tracks: [_twoTracks.first],
            pathLauncher: launcher,
          ),
        ),
      ),
    );

    await tester.tap(find.byTooltip('Locate file'));
    await tester.pump();

    expect(launcher.locatedPaths, ['h264_9s_1920x1080.mp4']);
  });

  testWidgets('floating side panels animate newly added lower panel', (
    tester,
  ) async {
    _setViewportSize(tester, const Size(1366, 768));

    Widget buildHost({required bool mediaInfo, required bool profiler}) {
      return MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: Stack(
            children: [
              FloatingSidePanelsSlot(
                mediaInfoVisible: mediaInfo,
                profilerVisible: profiler,
                tracks: _twoTracks,
                onCloseMediaInfo: () {},
                onCloseProfiler: () {},
              ),
            ],
          ),
        ),
      );
    }

    await tester.pumpWidget(buildHost(mediaInfo: true, profiler: false));
    await tester.pump(const Duration(milliseconds: 190));
    expect(find.text('Media Info'), findsOneWidget);
    expect(find.text('Performance Monitor'), findsNothing);

    await tester.pumpWidget(buildHost(mediaInfo: true, profiler: true));
    final profilerFadeValues = tester
        .widgetList<FadeTransition>(
          find.ancestor(
            of: find.text('Performance Monitor'),
            matching: find.byType(FadeTransition),
          ),
        )
        .map((transition) => transition.opacity.value);
    expect(profilerFadeValues.any((value) => value < 1), isTrue);

    await tester.pump(const Duration(milliseconds: 190));
    final settledProfilerFadeValues = tester
        .widgetList<FadeTransition>(
          find.ancestor(
            of: find.text('Performance Monitor'),
            matching: find.byType(FadeTransition),
          ),
        )
        .map((transition) => transition.opacity.value);
    expect(settledProfilerFadeValues.every((value) => value == 1), isTrue);

    final stackedProfilerTop = tester
        .getTopLeft(find.text('Performance Monitor'))
        .dy;

    await tester.pumpWidget(buildHost(mediaInfo: false, profiler: true));
    await tester.pump(const Duration(milliseconds: 45));
    final movingProfilerTop = tester
        .getTopLeft(find.text('Performance Monitor'))
        .dy;
    await tester.pump(const Duration(milliseconds: 45));
    final laterProfilerTop = tester
        .getTopLeft(find.text('Performance Monitor'))
        .dy;

    expect(movingProfilerTop, lessThan(stackedProfilerTop));
    expect(laterProfilerTop, lessThan(movingProfilerTop));

    await tester.pumpWidget(buildHost(mediaInfo: false, profiler: false));
    await tester.pump(const Duration(milliseconds: 70));
    expect(find.text('Performance Monitor'), findsOneWidget);
    final exitingProfilerFadeValues = tester
        .widgetList<FadeTransition>(
          find.ancestor(
            of: find.text('Performance Monitor'),
            matching: find.byType(FadeTransition),
          ),
        )
        .map((transition) => transition.opacity.value);
    expect(exitingProfilerFadeValues.any((value) => value < 1), isTrue);

    await tester.pump(const Duration(milliseconds: 149));
    expect(find.text('Performance Monitor'), findsNothing);
  });
}
