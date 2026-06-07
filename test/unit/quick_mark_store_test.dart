import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_store.dart';

void main() {
  test('quick mark store assigns stable ids and advances next id', () {
    final store = QuickMarkStore(
      marks: const [
        QuickMark(
          id: 7,
          anchor: QuickMarkAnchor(fileId: 1, ptsUs: 0, dtsUs: 0),
          sourceRect: Rect.zero,
        ),
      ],
    );

    final next = store.add(
      const QuickMark(
        id: 0,
        anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
        sourceRect: Rect.zero,
      ),
    );

    expect(next.marks.map((mark) => mark.id), const [7, 8]);
    expect(next.nextId, 9);
  });

  test('quick mark store updates and deletes marks immutably', () {
    final store = QuickMarkStore(
      marks: const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(fileId: 1, ptsUs: 0, dtsUs: 0),
          sourceRect: Rect.zero,
        ),
        QuickMark(
          id: 2,
          anchor: QuickMarkAnchor(fileId: 2, ptsUs: 0, dtsUs: 0),
          sourceRect: Rect.zero,
        ),
      ],
    );

    final updated = store.update(store.markById(1)!.copyWith(text: 'note'));
    final deleted = updated.deleteForFileId(2);

    expect(store.markById(1)?.text, isEmpty);
    expect(updated.markById(1)?.text, 'note');
    expect(deleted.marks.map((mark) => mark.id), const [1]);
    expect(deleted.contains(2), isFalse);
  });

  test('quick mark store derives visible marks and visible selection', () {
    final store = QuickMarkStore(
      marks: const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(
            fileId: 1,
            ptsUs: 1000,
            dtsUs: 1000,
            durationUs: 1000,
          ),
          sourceRect: Rect.zero,
        ),
        QuickMark(
          id: 2,
          anchor: QuickMarkAnchor(
            fileId: 1,
            ptsUs: 4000,
            dtsUs: 4000,
            durationUs: 1000,
          ),
          sourceRect: Rect.zero,
        ),
      ],
    );

    final view = store.view(
      context: const QuickMarkFrameContext(
        currentPtsUs: 0,
        presentedFrameAnchors: {
          1: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
        },
      ),
      selectedMarkId: 2,
    );

    expect(view.allMarks.map((mark) => mark.id), const [1, 2]);
    expect(view.visibleMarks.map((mark) => mark.id), const [1]);
    expect(view.visibleMarkIds, const {1});
    expect(view.selectedMarkId, 2);
    expect(view.visibleSelectedMarkId, isNull);
  });
}
