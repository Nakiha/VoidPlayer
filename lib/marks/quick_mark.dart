import 'dart:ui';

import 'package:flutter/foundation.dart';

import '../native_player/native_player_protocol.dart';

enum QuickMarkShape { rectangle, arrow }

/// Who authored a mark. Human verdicts are the ground truth of the
/// review loop; agent and metric marks are candidates routed to a human.
enum QuickMarkOrigin { human, agent, metric }

/// Preset defect vocabulary for subjective quality review. `defectType` is a
/// free string so custom taxonomies need no schema change; these are the
/// suggestions surfaced in the editor UI.
abstract final class QuickMarkDefectTypes {
  static const String banding = 'banding';
  static const String blocking = 'blocking';
  static const String ringing = 'ringing';
  static const String mosquitoNoise = 'mosquito_noise';
  static const String blur = 'blur';
  static const String flicker = 'flicker';
  static const String colorShift = 'color_shift';

  static const List<String> presets = [
    banding,
    blocking,
    ringing,
    mosquitoNoise,
    blur,
    flicker,
    colorShift,
  ];
}

const int kQuickMarkSeverityMin = 1;
const int kQuickMarkSeverityMax = 5;

class QuickMarkAnchor {
  static const int noTimestampUs = -9223372036854775808;

  final int fileId;
  final int ptsUs;
  final int dtsUs;
  final int durationUs;
  final int? vac2FrameIndex;
  final int analysisFrameIndex;
  final int frameIdentityMode;
  final int sourcePacketIndex;
  final int sourcePacketSize;
  final int sourcePacketPos;
  final int sourcePacketPtsUs;
  final int sourcePacketDtsUs;

  const QuickMarkAnchor({
    required this.fileId,
    required this.ptsUs,
    required this.dtsUs,
    this.durationUs = 0,
    this.vac2FrameIndex,
    this.analysisFrameIndex = -1,
    this.frameIdentityMode = 0,
    this.sourcePacketIndex = -1,
    this.sourcePacketSize = 0,
    this.sourcePacketPos = -1,
    this.sourcePacketPtsUs = noTimestampUs,
    this.sourcePacketDtsUs = noTimestampUs,
  });

  factory QuickMarkAnchor.fromPresentedFrame({
    required int fileId,
    required PresentedFrameTiming? timing,
    required int fallbackPtsUs,
  }) {
    if (timing == null || !timing.isValid) {
      return QuickMarkAnchor(
        fileId: fileId,
        ptsUs: fallbackPtsUs,
        dtsUs: fallbackPtsUs,
      );
    }
    final ptsUs = timing.ptsUs >= 0 ? timing.ptsUs : fallbackPtsUs;
    final dtsUs = timing.dtsUs == PresentedFrameTiming.noTimestampUs
        ? ptsUs
        : timing.dtsUs;
    return QuickMarkAnchor(
      fileId: fileId,
      ptsUs: ptsUs,
      dtsUs: dtsUs,
      durationUs: timing.durationUs,
      analysisFrameIndex: timing.analysisFrameIndex,
      frameIdentityMode: timing.frameIdentityMode,
      sourcePacketIndex: timing.sourcePacketIndex,
      sourcePacketSize: timing.sourcePacketSize,
      sourcePacketPos: timing.sourcePacketPos,
      sourcePacketPtsUs: timing.sourcePacketPtsUs,
      sourcePacketDtsUs: timing.sourcePacketDtsUs,
    );
  }

  bool get hasVac2Frame => vac2FrameIndex != null && vac2FrameIndex! >= 0;

  bool get hasSourcePacketIdentity =>
      sourcePacketPos >= 0 || sourcePacketIndex >= 0;

  bool get hasAnalysisFrame => analysisFrameIndex >= 0;

  bool get hasStrongIdentity =>
      hasVac2Frame || hasAnalysisFrame || hasSourcePacketIdentity;

  bool matchesPresentedFrame(QuickMarkAnchor current) {
    if (fileId != current.fileId) return false;
    if (hasVac2Frame && current.hasVac2Frame) {
      return vac2FrameIndex == current.vac2FrameIndex;
    }
    if (hasAnalysisFrame && current.hasAnalysisFrame) {
      return analysisFrameIndex == current.analysisFrameIndex;
    }
    if (hasSourcePacketIdentity && current.hasSourcePacketIdentity) {
      final samePosition =
          sourcePacketPos >= 0 && sourcePacketPos == current.sourcePacketPos;
      final sameIndex =
          sourcePacketIndex >= 0 &&
          sourcePacketIndex == current.sourcePacketIndex;
      if ((samePosition || sameIndex) &&
          (sourcePacketSize <= 0 ||
              current.sourcePacketSize <= 0 ||
              sourcePacketSize == current.sourcePacketSize)) {
        return true;
      }
      return false;
    }
    return ptsUs == current.ptsUs && dtsUs == current.dtsUs;
  }

  bool matchesPresentedFrameOrTime(
    QuickMarkAnchor current, {
    required int fallbackToleranceUs,
  }) {
    if (fileId != current.fileId) return false;
    if (hasStrongIdentity && current.hasStrongIdentity) {
      return matchesPresentedFrame(current);
    }
    return (ptsUs - current.ptsUs).abs() <= fallbackToleranceUs;
  }

  QuickMarkAnchor copyWith({
    int? fileId,
    int? ptsUs,
    int? dtsUs,
    int? durationUs,
    Object? vac2FrameIndex = _quickMarkUnset,
    int? analysisFrameIndex,
    int? frameIdentityMode,
    int? sourcePacketIndex,
    int? sourcePacketSize,
    int? sourcePacketPos,
    int? sourcePacketPtsUs,
    int? sourcePacketDtsUs,
  }) {
    return QuickMarkAnchor(
      fileId: fileId ?? this.fileId,
      ptsUs: ptsUs ?? this.ptsUs,
      dtsUs: dtsUs ?? this.dtsUs,
      durationUs: durationUs ?? this.durationUs,
      vac2FrameIndex: vac2FrameIndex == _quickMarkUnset
          ? this.vac2FrameIndex
          : vac2FrameIndex as int?,
      analysisFrameIndex: analysisFrameIndex ?? this.analysisFrameIndex,
      frameIdentityMode: frameIdentityMode ?? this.frameIdentityMode,
      sourcePacketIndex: sourcePacketIndex ?? this.sourcePacketIndex,
      sourcePacketSize: sourcePacketSize ?? this.sourcePacketSize,
      sourcePacketPos: sourcePacketPos ?? this.sourcePacketPos,
      sourcePacketPtsUs: sourcePacketPtsUs ?? this.sourcePacketPtsUs,
      sourcePacketDtsUs: sourcePacketDtsUs ?? this.sourcePacketDtsUs,
    );
  }

  @override
  bool operator ==(Object other) {
    return identical(this, other) ||
        other is QuickMarkAnchor &&
            other.fileId == fileId &&
            other.ptsUs == ptsUs &&
            other.dtsUs == dtsUs &&
            other.durationUs == durationUs &&
            other.vac2FrameIndex == vac2FrameIndex &&
            other.analysisFrameIndex == analysisFrameIndex &&
            other.frameIdentityMode == frameIdentityMode &&
            other.sourcePacketIndex == sourcePacketIndex &&
            other.sourcePacketSize == sourcePacketSize &&
            other.sourcePacketPos == sourcePacketPos &&
            other.sourcePacketPtsUs == sourcePacketPtsUs &&
            other.sourcePacketDtsUs == sourcePacketDtsUs;
  }

  @override
  int get hashCode => Object.hash(
    fileId,
    ptsUs,
    dtsUs,
    durationUs,
    vac2FrameIndex,
    analysisFrameIndex,
    frameIdentityMode,
    sourcePacketIndex,
    sourcePacketSize,
    sourcePacketPos,
    sourcePacketPtsUs,
    sourcePacketDtsUs,
  );
}

const Object _quickMarkUnset = Object();

class QuickMark {
  final int id;
  final QuickMarkAnchor anchor;
  final Rect sourceRect;
  final Offset? sourceStart;
  final Offset? sourceEnd;
  final Color color;
  final double strokeWidth;
  final QuickMarkShape shape;
  final String text;
  final bool textBold;
  final double textFontSize;
  final bool syncAcrossTracks;
  final QuickMarkOrigin origin;
  final String? defectType;
  final int? severity;
  final Map<String, Object?> attributes;

  const QuickMark({
    required this.id,
    required this.anchor,
    required this.sourceRect,
    this.sourceStart,
    this.sourceEnd,
    this.color = const Color(0xFFFF3B30),
    this.strokeWidth = 3.0,
    this.shape = QuickMarkShape.rectangle,
    this.text = '',
    this.textBold = true,
    this.textFontSize = 14.0,
    this.syncAcrossTracks = true,
    this.origin = QuickMarkOrigin.human,
    this.defectType,
    this.severity,
    this.attributes = const {},
  }) : assert(
         severity == null ||
             (severity >= kQuickMarkSeverityMin &&
                 severity <= kQuickMarkSeverityMax),
       );

  int get fileId => anchor.fileId;
  int get ptsUs => anchor.ptsUs;

  /// Marks with `scope: 'time'` carry no spatial evidence (for example a
  /// relative-outlier quality event). They must not be painted or hit-tested
  /// as an on-video rectangle; the timeline and sidebar still expose them.
  bool get isTimeOnly => attributes['scope'] == 'time';

  Offset get effectiveSourceStart => sourceStart ?? sourceRect.topLeft;
  Offset get effectiveSourceEnd => sourceEnd ?? sourceRect.bottomRight;

  QuickMark copyWith({
    int? id,
    QuickMarkAnchor? anchor,
    Rect? sourceRect,
    Offset? sourceStart,
    Offset? sourceEnd,
    Color? color,
    double? strokeWidth,
    QuickMarkShape? shape,
    String? text,
    bool? textBold,
    double? textFontSize,
    bool? syncAcrossTracks,
    QuickMarkOrigin? origin,
    Object? defectType = _quickMarkUnset,
    Object? severity = _quickMarkUnset,
    Map<String, Object?>? attributes,
  }) {
    return QuickMark(
      id: id ?? this.id,
      anchor: anchor ?? this.anchor,
      sourceRect: sourceRect ?? this.sourceRect,
      sourceStart: sourceStart ?? this.sourceStart,
      sourceEnd: sourceEnd ?? this.sourceEnd,
      color: color ?? this.color,
      strokeWidth: strokeWidth ?? this.strokeWidth,
      shape: shape ?? this.shape,
      text: text ?? this.text,
      textBold: textBold ?? this.textBold,
      textFontSize: textFontSize ?? this.textFontSize,
      syncAcrossTracks: syncAcrossTracks ?? this.syncAcrossTracks,
      origin: origin ?? this.origin,
      defectType: defectType == _quickMarkUnset
          ? this.defectType
          : defectType as String?,
      severity: severity == _quickMarkUnset ? this.severity : severity as int?,
      attributes: attributes ?? this.attributes,
    );
  }

  @override
  bool operator ==(Object other) {
    return identical(this, other) ||
        other is QuickMark &&
            other.id == id &&
            other.anchor == anchor &&
            other.sourceRect == sourceRect &&
            other.sourceStart == sourceStart &&
            other.sourceEnd == sourceEnd &&
            other.color == color &&
            other.strokeWidth == strokeWidth &&
            other.shape == shape &&
            other.text == text &&
            other.textBold == textBold &&
            other.textFontSize == textFontSize &&
            other.syncAcrossTracks == syncAcrossTracks &&
            other.origin == origin &&
            other.defectType == defectType &&
            other.severity == severity &&
            mapEquals(other.attributes, attributes);
  }

  @override
  int get hashCode => Object.hash(
    id,
    anchor,
    sourceRect,
    sourceStart,
    sourceEnd,
    color,
    strokeWidth,
    shape,
    text,
    textBold,
    textFontSize,
    syncAcrossTracks,
    origin,
    defectType,
    severity,
    Object.hashAllUnordered(attributes.keys),
  );
}
