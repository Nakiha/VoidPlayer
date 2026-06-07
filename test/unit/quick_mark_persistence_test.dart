import 'dart:io';
import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_persistence.dart';

void main() {
  late Directory dir;
  late FileQuickMarkRepository repository;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp(
      'void_player_quick_mark_persistence_test_',
    );
    repository = FileQuickMarkRepository(File('${dir.path}/local.vpmarks'));
  });

  tearDown(() async {
    if (await dir.exists()) await dir.delete(recursive: true);
  });

  test('saves and loads marks by stable media path', () async {
    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 7, path: '/media/a.mp4')],
      const [
        QuickMark(
          id: 3,
          anchor: QuickMarkAnchor(
            fileId: 7,
            ptsUs: 12000,
            dtsUs: 11000,
            durationUs: 4000,
            analysisFrameIndex: 9,
          ),
          sourceRect: Rect.fromLTWH(0.1, 0.2, 0.3, 0.4),
          sourceStart: Offset(0.1, 0.2),
          sourceEnd: Offset(0.4, 0.6),
          color: Color(0xFF34C759),
          strokeWidth: 5,
          shape: QuickMarkShape.arrow,
          text: 'note',
          textBold: false,
          textFontSize: 18,
          syncAcrossTracks: false,
        ),
      ],
    );

    final loaded = await repository.loadForMediaRefs([
      QuickMarkMediaRef(fileId: 42, path: '/media/a.mp4'),
    ]);

    expect(loaded, hasLength(1));
    final mark = loaded.single;
    expect(mark.id, 3);
    expect(mark.fileId, 42);
    expect(mark.anchor.ptsUs, 12000);
    expect(mark.anchor.dtsUs, 11000);
    expect(mark.anchor.analysisFrameIndex, 9);
    expect(mark.sourceRect, Rect.fromLTWH(0.1, 0.2, 0.3, 0.4));
    expect(mark.effectiveSourceStart, const Offset(0.1, 0.2));
    expect(mark.effectiveSourceEnd, const Offset(0.4, 0.6));
    expect(mark.color.toARGB32(), 0xFF34C759);
    expect(mark.strokeWidth, 5);
    expect(mark.shape, QuickMarkShape.arrow);
    expect(mark.text, 'note');
    expect(mark.textBold, isFalse);
    expect(mark.textFontSize, 18);
    expect(mark.syncAcrossTracks, isFalse);
  });

  test('saving one media preserves marks for inactive media', () async {
    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 1, path: '/media/a.mp4')],
      const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
          sourceRect: Rect.zero,
          text: 'a',
        ),
      ],
    );
    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 2, path: '/media/b.mp4')],
      const [
        QuickMark(
          id: 2,
          anchor: QuickMarkAnchor(fileId: 2, ptsUs: 2000, dtsUs: 2000),
          sourceRect: Rect.zero,
          text: 'b',
        ),
      ],
    );

    final loaded = await repository.loadForMediaRefs([
      QuickMarkMediaRef(fileId: 10, path: '/media/a.mp4'),
      QuickMarkMediaRef(fileId: 20, path: '/media/b.mp4'),
    ]);

    expect(loaded.map((mark) => mark.text).toSet(), {'a', 'b'});
    expect(loaded.map((mark) => '${mark.text}:${mark.fileId}').toSet(), {
      'a:10',
      'b:20',
    });
  });
}
