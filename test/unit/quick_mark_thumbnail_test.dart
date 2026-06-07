import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_thumbnail.dart';

void main() {
  const mark = QuickMark(
    id: 1,
    anchor: QuickMarkAnchor(fileId: 7, ptsUs: 1000, dtsUs: 1000),
    sourceRect: Rect.fromLTWH(0.1, 0.2, 0.3, 0.4),
  );

  test('queues thumbnails for new marks', () {
    final thumbnails = QuickMarkThumbnailStore.reconcile(
      marks: const [mark],
      current: const {},
    );

    expect(thumbnails.keys, const {1});
    expect(thumbnails[1]?.status, QuickMarkThumbnailStatus.queued);
    expect(thumbnails[1]?.sourceKey, contains('rect=0.100000'));
  });

  test('keeps ready asset when mark source is unchanged', () {
    final sourceKey = QuickMarkThumbnailStore.sourceKeyForMark(mark);
    final thumbnails = QuickMarkThumbnailStore.reconcile(
      marks: const [mark],
      current: {
        1: QuickMarkThumbnail(
          markId: 1,
          sourceKey: sourceKey,
          status: QuickMarkThumbnailStatus.ready,
          assetPath: '/tmp/thumb.png',
        ),
      },
    );

    expect(thumbnails[1]?.status, QuickMarkThumbnailStatus.ready);
    expect(thumbnails[1]?.assetPath, '/tmp/thumb.png');
  });

  test('keeps ready asset when only mark text changes', () {
    final sourceKey = QuickMarkThumbnailStore.sourceKeyForMark(mark);
    final thumbnails = QuickMarkThumbnailStore.reconcile(
      marks: [mark.copyWith(text: 'note')],
      current: {
        1: QuickMarkThumbnail(
          markId: 1,
          sourceKey: sourceKey,
          status: QuickMarkThumbnailStatus.ready,
          assetPath: '/tmp/thumb.png',
        ),
      },
    );

    expect(thumbnails[1]?.status, QuickMarkThumbnailStatus.ready);
    expect(thumbnails[1]?.assetPath, '/tmp/thumb.png');
  });

  test(
    'requeues thumbnail when mark source changes and prunes deleted marks',
    () {
      final sourceKey = QuickMarkThumbnailStore.sourceKeyForMark(mark);
      final changed = mark.copyWith(
        sourceRect: const Rect.fromLTWH(0.2, 0.2, 0.3, 0.4),
      );
      final thumbnails = QuickMarkThumbnailStore.reconcile(
        marks: [changed],
        current: {
          1: QuickMarkThumbnail(
            markId: 1,
            sourceKey: sourceKey,
            status: QuickMarkThumbnailStatus.ready,
            assetPath: '/tmp/thumb.png',
          ),
          2: const QuickMarkThumbnail(
            markId: 2,
            sourceKey: 'stale',
            status: QuickMarkThumbnailStatus.ready,
            assetPath: '/tmp/stale.png',
          ),
        },
      );

      expect(thumbnails.keys, const {1});
      expect(thumbnails[1]?.status, QuickMarkThumbnailStatus.queued);
      expect(thumbnails[1]?.assetPath, isNull);
    },
  );
}
