import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/widgets/viewport_panel.dart';

void main() {
  Widget buildPanel({
    required List<Offset> pans,
    required List<({double factor, Offset position})> zooms,
  }) {
    return MaterialApp(
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: Scaffold(
        body: Align(
          alignment: Alignment.topLeft,
          child: SizedBox(
            width: 240,
            height: 160,
            child: ViewportPanel(
              textureId: 1,
              viewportState: 2,
              layout: const LayoutState(),
              onPan: pans.add,
              onSplit: (_) {},
              onZoom: (factor, position) =>
                  zooms.add((factor: factor, position: position)),
              onPointerButton: (_, _) {},
            ),
          ),
        ),
      ),
    );
  }

  testWidgets('pan zoom scale noise still pans the viewport', (tester) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    await tester.pumpWidget(buildPanel(pans: pans, zooms: zooms));
    final devicePixelRatio = tester.view.devicePixelRatio;

    final pointer = TestPointer(1, PointerDeviceKind.trackpad);
    final center = tester.getCenter(find.byType(ViewportPanel));
    await tester.sendEventToBinding(pointer.panZoomStart(center));
    await tester.sendEventToBinding(
      pointer.panZoomUpdate(center, pan: const Offset(10, 4), scale: 1.001),
    );
    await tester.sendEventToBinding(
      pointer.panZoomUpdate(center, pan: const Offset(15, 6), scale: 1.0015),
    );

    expect(zooms, isEmpty);
    expect(pans, [
      const Offset(10, 4) * devicePixelRatio,
      const Offset(5, 2) * devicePixelRatio,
    ]);
  });

  testWidgets('pan zoom pinch uses the current gesture position as anchor', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    await tester.pumpWidget(buildPanel(pans: pans, zooms: zooms));
    final devicePixelRatio = tester.view.devicePixelRatio;

    final pointer = TestPointer(1, PointerDeviceKind.trackpad);
    const gesturePosition = Offset(96, 64);
    await tester.sendEventToBinding(pointer.panZoomStart(gesturePosition));
    await tester.sendEventToBinding(
      pointer.panZoomUpdate(gesturePosition, scale: 1.01),
    );

    expect(pans, isEmpty);
    expect(zooms, hasLength(1));
    expect(zooms.single.factor, moreOrLessEquals(1.01));
    expect(zooms.single.position, gesturePosition * devicePixelRatio);
  });
}
