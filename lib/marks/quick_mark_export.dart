import 'dart:ui';

import 'quick_mark.dart';

/// Version of the verdict export document consumed by agents.
///
/// The document is the outbound half of the review loop: an agent loads
/// encode candidates, a human records region verdicts, and the agent reads
/// them back through this format (via the EXPORT_MARKS automation command or
/// the agent protocol's getMarks).
const int quickMarkExportVersion = 1;

/// Media descriptor for one loaded track in an export document.
class QuickMarkExportMedia {
  final int fileId;
  final int slotIndex;
  final String path;
  final String? mediaHash;
  final String? sourceId;

  const QuickMarkExportMedia({
    required this.fileId,
    required this.slotIndex,
    required this.path,
    required this.mediaHash,
    required this.sourceId,
  });
}

Map<String, Object?> buildQuickMarkExportDocument({
  required List<QuickMarkExportMedia> media,
  required List<QuickMark> marks,
  required int generatedAtMs,
}) {
  final mediaByFileId = {for (final entry in media) entry.fileId: entry};
  return {
    'version': quickMarkExportVersion,
    'generatedAtMs': generatedAtMs,
    'media': [
      for (final entry in media)
        {
          'fileId': entry.fileId,
          'slotIndex': entry.slotIndex,
          'path': entry.path,
          'mediaHash': entry.mediaHash,
          'sourceId': entry.sourceId,
        },
    ],
    'marks': [
      for (final mark in marks)
        _markToExportJson(mark, mediaByFileId[mark.fileId]),
    ],
  };
}

Map<String, Object?> _markToExportJson(
  QuickMark mark,
  QuickMarkExportMedia? media,
) {
  return {
    'id': mark.id,
    'fileId': mark.fileId,
    'mediaHash': media?.mediaHash,
    'sourceId': media?.sourceId,
    'anchor': {
      'ptsUs': mark.anchor.ptsUs,
      'dtsUs': mark.anchor.dtsUs,
      'durationUs': mark.anchor.durationUs,
      if (mark.anchor.vac2FrameIndex != null)
        'vac2FrameIndex': mark.anchor.vac2FrameIndex,
      'analysisFrameIndex': mark.anchor.analysisFrameIndex,
      'sourcePacketIndex': mark.anchor.sourcePacketIndex,
      'sourcePacketPos': mark.anchor.sourcePacketPos,
    },
    'region': _rectToJson(mark.sourceRect),
    'shape': mark.shape.name,
    'text': mark.text,
    'origin': mark.origin.name,
    'defectType': mark.defectType,
    'severity': mark.severity,
    if (mark.attributes.isNotEmpty) 'attributes': mark.attributes,
  };
}

Map<String, Object?> _rectToJson(Rect rect) => {
  'left': rect.left,
  'top': rect.top,
  'width': rect.width,
  'height': rect.height,
};
