import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/platform/pointer_button_state_provider.dart';
import 'package:void_player/video_renderer_controller.dart';
import 'package:void_player/viewport/display_geometry.dart';
import 'package:void_player/viewport/viewport_display_state.dart';
import 'package:void_player/widgets/app_menu_combo.dart';
import 'package:void_player/widgets/viewport_panel.dart';

class _FakePointerButtonStateProvider implements PointerButtonStateProvider {
  bool primary = false;
  bool secondary = false;

  @override
  bool get isPrimaryButtonDown => primary;

  @override
  bool get isSecondaryButtonDown => secondary;
}

void main() {
  Widget buildPanel({
    required List<Offset> pans,
    required List<({double factor, Offset position})> zooms,
    PointerButtonStateProvider pointerButtonStateProvider =
        emptyPointerButtonStateProvider,
    void Function(bool panning, bool splitting)? onPointerButton,
    ValueChanged<Offset>? onQuickMarkStart,
    ValueChanged<Offset>? onQuickMarkUpdate,
    VoidCallback? onQuickMarkEnd,
    VoidCallback? onQuickMarkCancel,
    List<DisplayTrackGeometry> trackGeometry = const [],
    List<QuickMark> quickMarks = const [],
    int? selectedQuickMarkId,
    ValueChanged<int?>? onQuickMarkSelect,
    ValueChanged<QuickMark>? onQuickMarkChanged,
    ValueChanged<int>? onQuickMarkDeleted,
    ValueChanged<int>? onQuickMarkFocus,
    Size size = const Size(240, 160),
    bool nativeCompositorHole = false,
  }) {
    return MaterialApp(
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: Scaffold(
        body: Align(
          alignment: Alignment.topLeft,
          child: SizedBox(
            width: size.width,
            height: size.height,
            child: ViewportPanel(
              textureId: 1,
              viewportState: const ViewportDisplayState.active(),
              layout: const LayoutState(),
              onPan: pans.add,
              onSplit: (_) {},
              onZoom: (factor, position) =>
                  zooms.add((factor: factor, position: position)),
              onPointerButton: onPointerButton ?? (_, _) {},
              onQuickMarkStart: onQuickMarkStart,
              onQuickMarkUpdate: onQuickMarkUpdate,
              onQuickMarkEnd: onQuickMarkEnd,
              onQuickMarkCancel: onQuickMarkCancel,
              trackGeometry: trackGeometry,
              quickMarks: quickMarks,
              selectedQuickMarkId: selectedQuickMarkId,
              nativeCompositorHole: nativeCompositorHole,
              onQuickMarkSelect: onQuickMarkSelect,
              onQuickMarkChanged: onQuickMarkChanged,
              onQuickMarkDeleted: onQuickMarkDeleted,
              onQuickMarkFocus: onQuickMarkFocus,
              pointerButtonStateProvider: pointerButtonStateProvider,
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

  testWidgets('recovers drag state from injected physical button provider', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final pointerStates = <({bool panning, bool splitting})>[];
    final pointerButtonStateProvider = _FakePointerButtonStateProvider()
      ..secondary = true;
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        pointerButtonStateProvider: pointerButtonStateProvider,
        onPointerButton: (panning, splitting) =>
            pointerStates.add((panning: panning, splitting: splitting)),
      ),
    );

    final gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryButton,
    );
    addTearDown(gesture.removePointer);
    await gesture.addPointer(location: Offset.zero);
    await gesture.moveTo(tester.getCenter(find.byType(ViewportPanel)));

    expect(pointerStates.first, (panning: true, splitting: false));
  });

  testWidgets('primary mouse drag is routed to quick mark instead of pan', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final starts = <Offset>[];
    final updates = <Offset>[];
    var ends = 0;
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        onQuickMarkStart: starts.add,
        onQuickMarkUpdate: updates.add,
        onQuickMarkEnd: () => ends += 1,
      ),
    );
    final devicePixelRatio = tester.view.devicePixelRatio;

    final center = tester.getCenter(find.byType(ViewportPanel));
    final gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kPrimaryButton,
    );
    addTearDown(gesture.removePointer);
    await gesture.down(center);
    await gesture.moveBy(const Offset(24, 12));
    await gesture.up();

    expect(pans, isEmpty);
    expect(starts, [center * devicePixelRatio]);
    expect(updates, [const Offset(24, 12) * devicePixelRatio + starts.single]);
    expect(ends, 1);
  });

  testWidgets('touchpad press drag is routed to quick mark', (tester) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final starts = <Offset>[];
    final updates = <Offset>[];
    var ends = 0;
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        onQuickMarkStart: starts.add,
        onQuickMarkUpdate: updates.add,
        onQuickMarkEnd: () => ends += 1,
      ),
    );
    final devicePixelRatio = tester.view.devicePixelRatio;

    final center = tester.getCenter(find.byType(ViewportPanel));
    final gesture = await tester.createGesture(kind: PointerDeviceKind.touch);
    addTearDown(gesture.removePointer);
    await gesture.down(center);
    await gesture.moveBy(const Offset(-20, 18));
    await gesture.up();

    expect(pans, isEmpty);
    expect(starts, [center * devicePixelRatio]);
    expect(updates, [const Offset(-20, 18) * devicePixelRatio + starts.single]);
    expect(ends, 1);
  });

  testWidgets('secondary mouse drag pans the viewport', (tester) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final starts = <Offset>[];
    await tester.pumpWidget(
      buildPanel(pans: pans, zooms: zooms, onQuickMarkStart: starts.add),
    );
    final devicePixelRatio = tester.view.devicePixelRatio;

    final center = tester.getCenter(find.byType(ViewportPanel));
    final gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kSecondaryButton,
    );
    addTearDown(gesture.removePointer);
    await gesture.down(center);
    await gesture.moveBy(const Offset(20, -8));
    await gesture.up();

    expect(starts, isEmpty);
    expect(pans, [const Offset(20, -8) * devicePixelRatio]);
  });

  testWidgets('native compositor hole still receives viewport pan gestures', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    await tester.pumpWidget(
      buildPanel(pans: pans, zooms: zooms, nativeCompositorHole: true),
    );
    final devicePixelRatio = tester.view.devicePixelRatio;

    expect(find.byType(Texture), findsNothing);

    final center = tester.getCenter(find.byType(ViewportPanel));
    final gesture = await tester.createGesture(
      kind: PointerDeviceKind.mouse,
      buttons: kSecondaryButton,
    );
    addTearDown(gesture.removePointer);
    await gesture.down(center);
    await gesture.moveBy(const Offset(20, -8));
    await gesture.up();

    expect(pans, [const Offset(20, -8) * devicePixelRatio]);
  });

  testWidgets(
    'tapping an existing quick mark selects it without starting a new mark',
    (tester) async {
      final pans = <Offset>[];
      final zooms = <({double factor, Offset position})>[];
      final starts = <Offset>[];
      final selections = <int?>[];
      await tester.pumpWidget(
        buildPanel(
          pans: pans,
          zooms: zooms,
          trackGeometry: const [
            DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
          ],
          quickMarks: const [
            QuickMark(
              id: 11,
              anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
              sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
            ),
          ],
          onQuickMarkStart: starts.add,
          onQuickMarkSelect: selections.add,
        ),
      );

      await tester.tapAt(const Offset(90, 40));

      expect(starts, isEmpty);
      expect(selections, [11]);
    },
  );

  testWidgets('tapping an arrow body selects it without starting a new mark', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final starts = <Offset>[];
    final selections = <int?>[];
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [
          QuickMark(
            id: 11,
            anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
            sourceRect: Rect.fromLTRB(0.25, 0.25, 0.75, 0.75),
            sourceStart: Offset(0.25, 0.25),
            sourceEnd: Offset(0.75, 0.75),
            shape: QuickMarkShape.arrow,
          ),
        ],
        onQuickMarkStart: starts.add,
        onQuickMarkSelect: selections.add,
      ),
    );

    await tester.tapAt(const Offset(120, 80));

    expect(starts, isEmpty);
    expect(selections, [11]);
  });

  testWidgets('selected quick mark panel can delete the mark', (tester) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final deleted = <int>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(520, 260),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 520, height: 260),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkDeleted: deleted.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.delete_outline));

    expect(deleted, [11]);
  });

  testWidgets('selected quick mark panel can edit text', (tester) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(360, 220),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 360, height: 220),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.title));
    await tester.pump();
    final singleLineTop = tester.getTopLeft(find.byType(EditableText)).dy;
    expect(singleLineTop, lessThan(55));
    await tester.enterText(
      find.byType(EditableText),
      'needs review\ncheck edge',
    );
    await tester.pump();

    expect(changes, isNotEmpty);
    expect(changes.last.text, 'needs review\ncheck edge');
    expect(
      tester.getTopLeft(find.byType(EditableText)).dy,
      lessThan(singleLineTop),
    );
  });

  testWidgets('selected quick mark panel can style text', (tester) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      text: 'label',
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(520, 260),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 520, height: 260),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.format_bold));
    expect(changes.last.textBold, isFalse);

    await tester.tap(find.text('14'));
    await tester.pump(const Duration(milliseconds: 200));
    await tester.tap(find.text('18').last);
    await tester.pump(const Duration(milliseconds: 200));

    expect(changes.last.textFontSize, 18.0);
  });

  testWidgets('selected quick mark panel can toggle sync projections', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    expect(mark.syncAcrossTracks, isTrue);
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(560, 260),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 560, height: 260),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.sync));

    expect(changes, isNotEmpty);
    expect(changes.last.syncAcrossTracks, isFalse);
  });

  testWidgets('selected quick mark panel can choose color and stroke', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(560, 260),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 560, height: 260),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    final colorMenu = tester.widget<AppMenuCombo<Color>>(
      find.byWidgetPredicate(
        (widget) =>
            widget is AppMenuCombo<Color> &&
            widget.items.contains(const Color(0xFFBF5AF2)),
      ),
    );
    colorMenu.onChanged(const Color(0xFFBF5AF2));
    await tester.pump();
    expect(changes.last.color, const Color(0xFFBF5AF2));

    final strokeMenu = tester.widget<AppMenuCombo<double>>(
      find.byWidgetPredicate(
        (widget) =>
            widget is AppMenuCombo<double> && widget.items.contains(5.0),
      ),
    );
    strokeMenu.onChanged(5.0);
    await tester.pump();
    expect(changes.last.strokeWidth, 5.0);
  });

  testWidgets('selected quick mark panel can request focus on the mark', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final focused = <int>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(560, 260),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 560, height: 260),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkFocus: focused.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.center_focus_strong));

    expect(focused, [11]);
  });

  testWidgets(
    'tapping a synced quick mark projection selects the source mark',
    (tester) async {
      final pans = <Offset>[];
      final zooms = <({double factor, Offset position})>[];
      final starts = <Offset>[];
      final selections = <int?>[];
      const mark = QuickMark(
        id: 11,
        anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
        sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      );
      await tester.pumpWidget(
        buildPanel(
          pans: pans,
          zooms: zooms,
          trackGeometry: const [
            DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
            DisplayTrackGeometry(fileId: 8, width: 240, height: 160),
          ],
          quickMarks: const [mark],
          onQuickMarkStart: starts.add,
          onQuickMarkSelect: selections.add,
        ),
      );

      await tester.tapAt(const Offset(160, 60));

      expect(starts, isEmpty);
      expect(selections, [11]);
    },
  );

  testWidgets('quick mark text selects first and drags like the mark body', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final selections = <int?>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      text: 'label',
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        onQuickMarkSelect: selections.add,
        onQuickMarkChanged: changes.add,
      ),
    );

    await tester.tapAt(const Offset(70, 28));
    expect(selections, [11]);
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkSelect: selections.add,
        onQuickMarkChanged: changes.add,
      ),
    );
    await tester.pump(const Duration(milliseconds: 500));

    await tester.drag(
      find.byKey(const ValueKey('quick-mark-text-hit-11')),
      const Offset(24, 16),
    );

    expect(find.byType(EditableText), findsNothing);
    expect(changes, isNotEmpty);
    final moved = changes.last;
    expect(moved.sourceRect.left, moreOrLessEquals(0.35));
    expect(moved.sourceRect.top, moreOrLessEquals(0.35));
    expect(moved.sourceRect.right, moreOrLessEquals(0.6));
    expect(moved.sourceRect.bottom, moreOrLessEquals(0.6));
  });

  testWidgets('double clicking quick mark text starts text editing', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      text: 'label',
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
      ),
    );

    await tester.tapAt(const Offset(70, 28));
    await tester.pump(const Duration(milliseconds: 80));
    await tester.tapAt(const Offset(70, 28));
    await tester.pump();

    expect(find.byType(EditableText), findsOneWidget);
  });

  testWidgets('plain enter commits text edit and clears quick mark focus', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final selections = <int?>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      text: 'label',
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(360, 220),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 360, height: 220),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkSelect: selections.add,
        onQuickMarkChanged: changes.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.title));
    await tester.pump();
    await tester.enterText(find.byType(EditableText), 'updated');
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();

    expect(find.byType(EditableText), findsNothing);
    expect(selections.last, isNull);
    expect(changes.last.text, 'updated');
  });

  testWidgets('modified enter keeps text editing for multiline input', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final selections = <int?>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      text: 'label',
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        size: const Size(360, 220),
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 360, height: 220),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkSelect: selections.add,
      ),
    );

    await tester.tap(find.byIcon(Icons.title));
    await tester.pump();
    await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
    await tester.pump();

    expect(find.byType(EditableText), findsOneWidget);
    expect(selections, isNot(contains(null)));
  });

  testWidgets('double clicking a quick mark body starts text editing', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
      ),
    );

    await tester.tapAt(const Offset(75, 40));
    await tester.pump(const Duration(milliseconds: 80));
    await tester.tapAt(const Offset(75, 40));
    await tester.pump();

    expect(find.byType(EditableText), findsOneWidget);
  });

  testWidgets('selected arrow corner handle wins over arrow body hit targets', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.75, 0.75),
      sourceStart: Offset(0.25, 0.25),
      sourceEnd: Offset(0.75, 0.75),
      shape: QuickMarkShape.arrow,
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
    addTearDown(gesture.removePointer);
    await gesture.down(const Offset(180, 120));
    await gesture.moveBy(const Offset(24, 16));
    await gesture.up();

    expect(changes, isNotEmpty);
    final resized = changes.last;
    expect(resized.sourceRect.left, moreOrLessEquals(0.25));
    expect(resized.sourceRect.top, moreOrLessEquals(0.25));
    expect(resized.sourceRect.right, moreOrLessEquals(0.85));
    expect(resized.sourceRect.bottom, moreOrLessEquals(0.85));
  });

  testWidgets('selected quick mark corner handle resizes the source rect', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
    addTearDown(gesture.removePointer);
    await gesture.down(const Offset(120, 80));
    await gesture.moveBy(const Offset(24, 16));
    await gesture.up();

    expect(changes, isNotEmpty);
    final rect = changes.last.sourceRect;
    expect(rect.left, moreOrLessEquals(0.25));
    expect(rect.top, moreOrLessEquals(0.25));
    expect(rect.right, moreOrLessEquals(0.6));
    expect(rect.bottom, moreOrLessEquals(0.6));
  });

  testWidgets('dragging selected quick mark border moves the mark', (
    tester,
  ) async {
    final pans = <Offset>[];
    final zooms = <({double factor, Offset position})>[];
    final changes = <QuickMark>[];
    const mark = QuickMark(
      id: 11,
      anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
      sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
      sourceStart: Offset(0.25, 0.25),
      sourceEnd: Offset(0.5, 0.5),
    );
    await tester.pumpWidget(
      buildPanel(
        pans: pans,
        zooms: zooms,
        trackGeometry: const [
          DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
        ],
        quickMarks: const [mark],
        selectedQuickMarkId: 11,
        onQuickMarkChanged: changes.add,
      ),
    );

    final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
    addTearDown(gesture.removePointer);
    await gesture.down(const Offset(75, 40));
    await gesture.moveBy(const Offset(24, 16));
    await gesture.up();

    expect(changes, isNotEmpty);
    final moved = changes.last;
    expect(moved.sourceRect.left, moreOrLessEquals(0.35));
    expect(moved.sourceRect.top, moreOrLessEquals(0.35));
    expect(moved.sourceRect.right, moreOrLessEquals(0.6));
    expect(moved.sourceRect.bottom, moreOrLessEquals(0.6));
    expect(moved.effectiveSourceStart, const Offset(0.35, 0.35));
    expect(moved.effectiveSourceEnd, const Offset(0.6, 0.6));
  });

  testWidgets(
    'arrow endpoints flip when a resize handle crosses the opposite edge',
    (tester) async {
      final pans = <Offset>[];
      final zooms = <({double factor, Offset position})>[];
      final changes = <QuickMark>[];
      const mark = QuickMark(
        id: 11,
        anchor: QuickMarkAnchor(fileId: 7, ptsUs: 0, dtsUs: 0),
        sourceRect: Rect.fromLTRB(0.25, 0.25, 0.5, 0.5),
        sourceStart: Offset(0.25, 0.25),
        sourceEnd: Offset(0.5, 0.5),
        shape: QuickMarkShape.arrow,
      );
      await tester.pumpWidget(
        buildPanel(
          pans: pans,
          zooms: zooms,
          trackGeometry: const [
            DisplayTrackGeometry(fileId: 7, width: 240, height: 160),
          ],
          quickMarks: const [mark],
          selectedQuickMarkId: 11,
          onQuickMarkChanged: changes.add,
        ),
      );

      final gesture = await tester.createGesture(kind: PointerDeviceKind.mouse);
      addTearDown(gesture.removePointer);
      await gesture.down(const Offset(60, 40));
      await gesture.moveBy(const Offset(96, 0));
      await gesture.up();

      expect(changes, isNotEmpty);
      final resized = changes.last;
      expect(resized.sourceRect.left, moreOrLessEquals(0.5));
      expect(resized.sourceRect.right, moreOrLessEquals(0.65));
      expect(resized.effectiveSourceStart.dx, moreOrLessEquals(0.65));
      expect(resized.effectiveSourceStart.dy, moreOrLessEquals(0.25));
      expect(resized.effectiveSourceEnd.dx, moreOrLessEquals(0.5));
      expect(resized.effectiveSourceEnd.dy, moreOrLessEquals(0.5));
    },
  );
}
