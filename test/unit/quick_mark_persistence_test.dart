import 'dart:convert';
import 'dart:io';
import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:sqlite3/sqlite3.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_media_hash.dart';
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

  test('uses quick mark media hash so marks survive path changes', () async {
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

  test('quick mark media hash ignores changes after first megabyte', () async {
    final firstMegabyte = List<int>.filled(kQuickMarkMediaHashPrefixBytes, 7);
    final original = File(p.join(dir.path, 'media', 'prefix_a.mp4'));
    final changedTail = File(p.join(dir.path, 'media', 'prefix_b.mp4'));
    await original.create(recursive: true);
    await original.writeAsBytes([...firstMegabyte, 1, 2, 3, 4]);
    await changedTail.create(recursive: true);
    await changedTail.writeAsBytes([...firstMegabyte, 9, 8, 7, 6, 5]);

    final originalHash = await computeQuickMarkMediaHash(original.path);
    final changedTailHash = await computeQuickMarkMediaHash(changedTail.path);

    expect(changedTailHash, originalHash);
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

  test(
    'heals marks stored under fallback hash once content is readable',
    () async {
      final missing = File(p.join(dir.path, 'media', 'missing.mp4'));
      await repository.saveForMediaRefs(
        [QuickMarkMediaRef(fileId: 1, path: missing.path)],
        const [
          QuickMark(
            id: 4,
            anchor: QuickMarkAnchor(fileId: 1, ptsUs: 5000, dtsUs: 5000),
            sourceRect: Rect.zero,
            text: 'fallback',
          ),
        ],
      );

      await missing.create(recursive: true);
      await missing.writeAsBytes([9, 9, 9, 9]);

      final loaded = await repository.loadForMediaRefs([
        QuickMarkMediaRef(fileId: 2, path: missing.path),
      ]);

      expect(loaded, hasLength(1));
      expect(loaded.single.text, 'fallback');
      expect(loaded.single.fileId, 2);

      final reloaded = await repository.loadForMediaRefs([
        QuickMarkMediaRef(fileId: 3, path: missing.path),
      ]);
      expect(reloaded, hasLength(1), reason: 'healed rows must not duplicate');
    },
  );

  test('distributes marks to every ref sharing the same content', () async {
    final copy = File(p.join(dir.path, 'media', 'a_copy.mp4'));
    await copy.create(recursive: true);
    await copy.writeAsBytes(await mediaA.readAsBytes());

    await repository.saveForMediaRefs(
      [QuickMarkMediaRef(fileId: 1, path: mediaA.path)],
      const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
          sourceRect: Rect.zero,
          text: 'shared',
        ),
      ],
    );

    final refs = [
      QuickMarkMediaRef(fileId: 10, path: mediaA.path),
      QuickMarkMediaRef(fileId: 20, path: copy.path),
    ];
    final loaded = await repository.loadForMediaRefs(refs);

    expect(loaded, hasLength(2));
    expect(loaded.map((mark) => mark.fileId).toSet(), {10, 20});
    expect(loaded.map((mark) => mark.text).toSet(), {'shared'});

    await repository.saveForMediaRefs(refs, loaded);
    final roundTrip = await repository.loadForMediaRefs(refs);
    expect(
      roundTrip,
      hasLength(2),
      reason: 'same-content refs must not multiply marks across round trips',
    );
  });

  test('round-trips judgment fields in payload v2', () async {
    final refs = [QuickMarkMediaRef(fileId: 1, path: mediaA.path)];
    await repository.saveForMediaRefs(refs, const [
      QuickMark(
        id: 1,
        anchor: QuickMarkAnchor(fileId: 1, ptsUs: 1000, dtsUs: 1000),
        sourceRect: Rect.zero,
        origin: QuickMarkOrigin.metric,
        defectType: QuickMarkDefectTypes.banding,
        severity: 4,
        attributes: {'algorithm': 'vmaf', 'score': 23.5},
      ),
    ]);

    final loaded = await repository.loadForMediaRefs(refs);
    final mark = loaded.single;
    expect(mark.origin, QuickMarkOrigin.metric);
    expect(mark.defectType, QuickMarkDefectTypes.banding);
    expect(mark.severity, 4);
    expect(mark.attributes['algorithm'], 'vmaf');
    expect(mark.attributes['score'], 23.5);
  });

  test('reads v1 payload rows with judgment defaults', () async {
    final refs = [QuickMarkMediaRef(fileId: 1, path: mediaA.path)];
    // Establish the media row, then plant a raw v1 payload.
    await repository.saveForMediaRefs(refs, const []);
    final hash = await computeQuickMarkMediaHash(mediaA.path);
    final db = sqlite3.open(repository.databasePath);
    try {
      db.execute(
        'INSERT INTO marks (media_hash, mark_id, payload_json, updated_at_ms) '
        'VALUES (?, ?, ?, ?)',
        [
          hash,
          1,
          jsonEncode({
            'version': 1,
            'id': 1,
            'anchor': {'ptsUs': 1000, 'dtsUs': 1000},
            'sourceRect': {'left': 0, 'top': 0, 'width': 0.1, 'height': 0.1},
            'text': 'legacy',
          }),
          0,
        ],
      );
    } finally {
      db.close();
    }

    final loaded = await repository.loadForMediaRefs(refs);
    final mark = loaded.single;
    expect(mark.text, 'legacy');
    expect(mark.origin, QuickMarkOrigin.human);
    expect(mark.defectType, isNull);
    expect(mark.severity, isNull);
    expect(mark.attributes, isEmpty);
  });

  test('rejects out-of-range severity on read', () async {
    final refs = [QuickMarkMediaRef(fileId: 1, path: mediaA.path)];
    await repository.saveForMediaRefs(refs, const []);
    final hash = await computeQuickMarkMediaHash(mediaA.path);
    final db = sqlite3.open(repository.databasePath);
    try {
      db.execute(
        'INSERT INTO marks (media_hash, mark_id, payload_json, updated_at_ms) '
        'VALUES (?, ?, ?, ?)',
        [
          hash,
          1,
          jsonEncode({
            'version': 2,
            'id': 1,
            'anchor': {'ptsUs': 1000, 'dtsUs': 1000},
            'sourceRect': {'left': 0, 'top': 0, 'width': 0.1, 'height': 0.1},
            'severity': 99,
          }),
          0,
        ],
      );
    } finally {
      db.close();
    }

    final loaded = await repository.loadForMediaRefs(refs);
    expect(loaded.single.severity, isNull);
  });
}
