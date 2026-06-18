import 'dart:math' as math;
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

  test('mergeLoaded replaces marks for loaded files and keeps others', () {
    const keptMark = QuickMark(
      id: 5,
      anchor: QuickMarkAnchor(fileId: 9, ptsUs: 1000, dtsUs: 1000),
      sourceRect: Rect.zero,
      text: 'kept',
    );
    const staleMark = QuickMark(
      id: 6,
      anchor: QuickMarkAnchor(fileId: 1, ptsUs: 2000, dtsUs: 2000),
      sourceRect: Rect.zero,
      text: 'stale',
    );
    const loadedMark = QuickMark(
      id: 1,
      anchor: QuickMarkAnchor(fileId: 1, ptsUs: 3000, dtsUs: 3000),
      sourceRect: Rect.zero,
      text: 'loaded',
    );

    final store = QuickMarkStore.mergeLoaded(
      current: const [keptMark, staleMark],
      loaded: const [loadedMark],
      nextId: 7,
    );

    expect(store.marks.map((mark) => mark.text), ['kept', 'loaded']);
    expect(store.nextId, greaterThanOrEqualTo(7));
  });

  test('mergeLoaded remaps colliding ids across media', () {
    const memoryMark = QuickMark(
      id: 1,
      anchor: QuickMarkAnchor(fileId: 9, ptsUs: 1000, dtsUs: 1000),
      sourceRect: Rect.zero,
      text: 'memory',
    );
    const loadedA = QuickMark(
      id: 1,
      anchor: QuickMarkAnchor(fileId: 1, ptsUs: 2000, dtsUs: 2000),
      sourceRect: Rect.zero,
      text: 'a1',
    );
    const loadedB = QuickMark(
      id: 1,
      anchor: QuickMarkAnchor(fileId: 2, ptsUs: 3000, dtsUs: 3000),
      sourceRect: Rect.zero,
      text: 'b1',
    );
    const loadedB2 = QuickMark(
      id: 2,
      anchor: QuickMarkAnchor(fileId: 2, ptsUs: 4000, dtsUs: 4000),
      sourceRect: Rect.zero,
      text: 'b2',
    );

    final store = QuickMarkStore.mergeLoaded(
      current: const [memoryMark],
      loaded: const [loadedA, loadedB, loadedB2],
      nextId: 1,
    );

    final ids = store.marks.map((mark) => mark.id).toList();
    expect(ids.toSet(), hasLength(4), reason: 'all ids must be unique');
    final byText = {for (final mark in store.marks) mark.text: mark};
    expect(byText['memory']!.id, 1);
    expect(byText['a1']!.fileId, 1);
    expect(byText['b1']!.fileId, 2);
    expect(byText['b2']!.fileId, 2);
    expect(store.nextId, greaterThan(ids.reduce(math.max)));
  });

  test('mergeLoaded keeps non-colliding loaded ids stable', () {
    const loadedA = QuickMark(
      id: 3,
      anchor: QuickMarkAnchor(fileId: 1, ptsUs: 2000, dtsUs: 2000),
      sourceRect: Rect.zero,
      text: 'a3',
    );
    const loadedB = QuickMark(
      id: 8,
      anchor: QuickMarkAnchor(fileId: 2, ptsUs: 3000, dtsUs: 3000),
      sourceRect: Rect.zero,
      text: 'b8',
    );

    final store = QuickMarkStore.mergeLoaded(
      current: const [],
      loaded: const [loadedA, loadedB],
      nextId: 1,
    );

    final byText = {for (final mark in store.marks) mark.text: mark};
    expect(byText['a3']!.id, 3);
    expect(byText['b8']!.id, 8);
    expect(store.nextId, 9);
  });
}
