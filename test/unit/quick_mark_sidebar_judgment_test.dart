import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/l10n/app_localizations.dart';
import 'package:void_player/main_window/main_window_view_model.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/widgets/quick_mark_sidebar.dart';

void main() {
  const mark = QuickMark(
    id: 1,
    anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
    sourceRect: Rect.fromLTWH(0.1, 0.1, 0.2, 0.2),
  );

  Widget host({required ValueChanged<QuickMark> onMarkChanged}) {
    return MaterialApp(
      locale: const Locale('en'),
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: Scaffold(
        body: QuickMarkSidebar(
          width: 360,
          onClose: () {},
          marks: const MainWindowMarksVm(
            allMarks: [mark],
            visibleMarks: [mark],
            visibleMarkIds: {1},
            selectedMarkId: 1,
            tracksByFileId: {},
            thumbnailsByMarkId: {},
            currentPtsUs: 1000,
          ),
          actions: MainWindowMarksActions(
            onJumpToMark: (_) {},
            onSelectVisibleMark: (_) {},
            onMarkChanged: onMarkChanged,
            onMarkDeleted: (_) {},
            onFocusVisibleMark: (_) {},
          ),
        ),
      ),
    );
  }

  testWidgets('selecting a defect type reports it through onMarkChanged', (
    tester,
  ) async {
    final changes = <QuickMark>[];
    await tester.pumpWidget(host(onMarkChanged: changes.add));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('Defect'));
    await tester.pumpAndSettle();
    await tester.tap(find.text(QuickMarkDefectTypes.banding).last);
    await tester.pumpAndSettle();

    expect(changes, hasLength(1));
    expect(changes.single.defectType, QuickMarkDefectTypes.banding);
    expect(changes.single.id, mark.id);
  });

  testWidgets('selecting a severity reports it through onMarkChanged', (
    tester,
  ) async {
    final changes = <QuickMark>[];
    await tester.pumpWidget(host(onMarkChanged: changes.add));
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('Severity'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('S4').last);
    await tester.pumpAndSettle();

    expect(changes, hasLength(1));
    expect(changes.single.severity, 4);
  });

  testWidgets('clearing severity reports null through onMarkChanged', (
    tester,
  ) async {
    final changes = <QuickMark>[];
    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: QuickMarkSidebar(
            width: 360,
            onClose: () {},
            marks: const MainWindowMarksVm(
              allMarks: [
                QuickMark(
                  id: 1,
                  anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
                  sourceRect: Rect.fromLTWH(0.1, 0.1, 0.2, 0.2),
                  severity: 3,
                ),
              ],
              visibleMarks: [
                QuickMark(
                  id: 1,
                  anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
                  sourceRect: Rect.fromLTWH(0.1, 0.1, 0.2, 0.2),
                  severity: 3,
                ),
              ],
              visibleMarkIds: {1},
              selectedMarkId: 1,
              tracksByFileId: {},
              thumbnailsByMarkId: {},
              currentPtsUs: 1000,
            ),
            actions: MainWindowMarksActions(
              onJumpToMark: (_) {},
              onSelectVisibleMark: (_) {},
              onMarkChanged: changes.add,
              onMarkDeleted: (_) {},
              onFocusVisibleMark: (_) {},
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byTooltip('Severity'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('None').last);
    await tester.pumpAndSettle();

    expect(changes, hasLength(1));
    expect(changes.single.severity, isNull);
  });
}
