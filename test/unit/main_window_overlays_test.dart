import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/windows/main/main_window_overlays.dart';

void main() {
  testWidgets('floating side panels animate newly added lower panel', (
    tester,
  ) async {
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
                tracks: const [],
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
