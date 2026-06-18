import 'dart:convert';
import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/marks/quick_mark.dart';
import 'package:void_player/marks/quick_mark_export.dart';

void main() {
  test('export document carries lineage and judgment fields', () {
    const media = [
      QuickMarkExportMedia(
        fileId: 1,
        slotIndex: 0,
        path: '/clips/source_x264_crf28.mp4',
        mediaHash: 'hashA',
        sourceId: 'clip01',
      ),
      QuickMarkExportMedia(
        fileId: 2,
        slotIndex: 1,
        path: '/clips/source_x265_crf30.mp4',
        mediaHash: 'hashB',
        sourceId: 'clip01',
      ),
    ];
    const marks = [
      QuickMark(
        id: 1,
        anchor: QuickMarkAnchor(
          fileId: 1,
          ptsUs: 2000000,
          dtsUs: 1990000,
          durationUs: 40000,
          analysisFrameIndex: 50,
        ),
        sourceRect: Rect.fromLTWH(0.1, 0.2, 0.3, 0.4),
        text: 'sky gradient steps',
        defectType: QuickMarkDefectTypes.banding,
        severity: 4,
      ),
      QuickMark(
        id: 2,
        anchor: QuickMarkAnchor(fileId: 2, ptsUs: 2000000, dtsUs: 2000000),
        sourceRect: Rect.fromLTWH(0.5, 0.5, 0.1, 0.1),
        origin: QuickMarkOrigin.metric,
        attributes: {'algorithm': 'vmaf', 'score': 31.2},
      ),
    ];

    final document = buildQuickMarkExportDocument(
      media: media,
      marks: marks,
      generatedAtMs: 1234,
    );

    expect(document['version'], quickMarkExportVersion);
    expect(document['generatedAtMs'], 1234);

    final mediaJson = document['media'] as List;
    expect(mediaJson, hasLength(2));
    expect(
      (mediaJson[0] as Map)['sourceId'],
      (mediaJson[1] as Map)['sourceId'],
      reason: 'both encodes share one source',
    );

    final marksJson = document['marks'] as List;
    final human = marksJson[0] as Map;
    expect(human['mediaHash'], 'hashA');
    expect(human['sourceId'], 'clip01');
    expect(human['origin'], 'human');
    expect(human['defectType'], 'banding');
    expect(human['severity'], 4);
    expect((human['anchor'] as Map)['ptsUs'], 2000000);
    expect((human['region'] as Map)['left'], 0.1);

    final metric = marksJson[1] as Map;
    expect(metric['origin'], 'metric');
    expect(metric['severity'], isNull);
    expect((metric['attributes'] as Map)['score'], 31.2);

    // The document must be plain JSON end to end.
    expect(() => jsonEncode(document), returnsNormally);
  });

  test('marks for unknown media export with null lineage', () {
    final document = buildQuickMarkExportDocument(
      media: const [],
      marks: const [
        QuickMark(
          id: 1,
          anchor: QuickMarkAnchor(fileId: 9, ptsUs: 0, dtsUs: 0),
          sourceRect: Rect.zero,
        ),
      ],
      generatedAtMs: 0,
    );

    final mark = (document['marks'] as List).single as Map;
    expect(mark['mediaHash'], isNull);
    expect(mark['sourceId'], isNull);
    expect(mark['fileId'], 9);
  });
}
