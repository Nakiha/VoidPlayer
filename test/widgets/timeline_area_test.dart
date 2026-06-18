import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/track_manager.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/widgets/timeline_area.dart';

void main() {
  testWidgets('offset controls report track file id instead of slot', (
    tester,
  ) async {
    final offsets = <({int fileId, int deltaMs})>[];

    await tester.pumpWidget(
      _localized(
        SizedBox(
          width: 800,
          height: 120,
          child: TimelineArea(
            entries: const [
              TrackEntry(
                TrackInfo(
                  fileId: 42,
                  slot: 0,
                  path: '/tmp/source.mp4',
                  width: 320,
                  height: 180,
                  durationUs: 1000000,
                ),
              ),
            ],
            onReorder: (_, _) {},
            onOffsetChanged: (fileId, deltaMs) async {
              offsets.add((fileId: fileId, deltaMs: deltaMs));
            },
            onToggleTrackAudio: (_) {},
            onRemoveTrack: (_) async {},
            onControlsWidthChanged: (_) {},
            controlsWidth: 520,
            maxEffectiveDurationUs: 1000000,
          ),
        ),
      ),
    );

    await tester.tap(find.byTooltip('+10ms'));
    await tester.pump();

    expect(offsets, [(fileId: 42, deltaMs: 10)]);
  });
}

Widget _localized(Widget child) => MaterialApp(
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: Scaffold(body: child),
);
