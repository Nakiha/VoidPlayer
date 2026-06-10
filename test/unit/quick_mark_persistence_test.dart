import 'dart:io';
import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_persistence.dart';

void main() {
  late Directory dir;
  late SqliteQuickMarkRepository repository;
  late File mediaA;
  late File mediaB;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp(
      'void_player_quick_mark_persistence_test_',
    );
    mediaA = File(p.join(dir.path, 'media', 'a.mp4'));
    mediaB = File(p.join(dir.path, 'media', 'b.mp4'));
    await mediaA.create(recursive: true);
    await mediaA.writeAsBytes([1, 2, 3, 4]);
    await mediaB.create(recursive: true);
    await mediaB.writeAsBytes([5, 6, 7, 8]);
    repository = SqliteQuickMarkRepository(
      databasePath: p.join(dir.path, 'storage.sqlite'),
    );
  });

  tearDown(() async {
    if (await dir.exists()) await dir.delete(recursive: true);
  });

  test('saves and loads marks by stable media path', () async {
    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 7, path: mediaA.path)],
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
      QuickMarkMediaRef(fileId: 42, path: mediaA.path),
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
      [QuickMarkMediaRef(fileId: 1, path: mediaA.path)],
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
      [QuickMarkMediaRef(fileId: 2, path: mediaB.path)],
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
      QuickMarkMediaRef(fileId: 10, path: mediaA.path),
      QuickMarkMediaRef(fileId: 20, path: mediaB.path),
    ]);

    expect(loaded.map((mark) => mark.text).toSet(), {'a', 'b'});
    expect(loaded.map((mark) => '${mark.text}:${mark.fileId}').toSet(), {
      'a:10',
      'b:20',
    });
  });

  test('uses content hash so marks survive path changes', () async {
    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 1, path: mediaA.path)],
      const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
          sourceRect: Rect.zero,
          text: 'same bytes',
        ),
      ],
    );

    final moved = File(p.join(dir.path, 'renamed', 'a-renamed.mp4'));
    await moved.create(recursive: true);
    await moved.writeAsBytes(await mediaA.readAsBytes());

    final loaded = await repository.loadForMediaRefs([
      QuickMarkMediaRef(fileId: 99, path: moved.path),
    ]);

    expect(loaded, hasLength(1));
    expect(loaded.single.fileId, 99);
    expect(loaded.single.text, 'same bytes');
  });

  test('uses stable media id fallback when media file is missing', () async {
    final missingPath = p.join(dir.path, 'missing', 'offline.mp4');

    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 1, path: missingPath)],
      const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
          sourceRect: Rect.zero,
          text: 'offline',
        ),
      ],
    );

    final loaded = await repository.loadForMediaRefs([
      QuickMarkMediaRef(fileId: 9, path: missingPath),
    ]);

    expect(loaded, hasLength(1));
    expect(loaded.single.fileId, 9);
    expect(loaded.single.text, 'offline');
  });
}
