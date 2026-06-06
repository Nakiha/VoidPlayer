import 'dart:ui';

enum QuickMarkShape { rectangle, arrow }

class QuickMark {
  final int id;
  final int fileId;
  final int ptsUs;
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

  const QuickMark({
    required this.id,
    required this.fileId,
    required this.ptsUs,
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
  });

  Offset get effectiveSourceStart => sourceStart ?? sourceRect.topLeft;
  Offset get effectiveSourceEnd => sourceEnd ?? sourceRect.bottomRight;

  QuickMark copyWith({
    int? id,
    int? fileId,
    int? ptsUs,
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
  }) {
    return QuickMark(
      id: id ?? this.id,
      fileId: fileId ?? this.fileId,
      ptsUs: ptsUs ?? this.ptsUs,
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
    );
  }

  @override
  bool operator ==(Object other) {
    return identical(this, other) ||
        other is QuickMark &&
            other.id == id &&
            other.fileId == fileId &&
            other.ptsUs == ptsUs &&
            other.sourceRect == sourceRect &&
            other.sourceStart == sourceStart &&
            other.sourceEnd == sourceEnd &&
            other.color == color &&
            other.strokeWidth == strokeWidth &&
            other.shape == shape &&
            other.text == text &&
            other.textBold == textBold &&
            other.textFontSize == textFontSize &&
            other.syncAcrossTracks == syncAcrossTracks;
  }

  @override
  int get hashCode => Object.hash(
    id,
    fileId,
    ptsUs,
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
  );
}
