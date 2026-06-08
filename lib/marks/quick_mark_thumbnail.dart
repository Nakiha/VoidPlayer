import 'dart:ui';

import 'quick_mark.dart';

enum QuickMarkThumbnailStatus { queued, ready, failed }

class QuickMarkThumbnail {
  final int markId;
  final String sourceKey;
  final QuickMarkThumbnailStatus status;
  final String? assetPath;
  final String? error;

  const QuickMarkThumbnail({
    required this.markId,
    required this.sourceKey,
    required this.status,
    this.assetPath,
    this.error,
  });

  bool get hasAsset =>
      status == QuickMarkThumbnailStatus.ready &&
      assetPath != null &&
      assetPath!.isNotEmpty;

  QuickMarkThumbnail copyWith({
    String? sourceKey,
    QuickMarkThumbnailStatus? status,
    Object? assetPath = _unset,
    Object? error = _unset,
  }) {
    return QuickMarkThumbnail(
      markId: markId,
      sourceKey: sourceKey ?? this.sourceKey,
      status: status ?? this.status,
      assetPath: assetPath == _unset ? this.assetPath : assetPath as String?,
      error: error == _unset ? this.error : error as String?,
    );
  }

  @override
  bool operator ==(Object other) {
    return identical(this, other) ||
        other is QuickMarkThumbnail &&
            other.markId == markId &&
            other.sourceKey == sourceKey &&
            other.status == status &&
            other.assetPath == assetPath &&
            other.error == error;
  }

  @override
  int get hashCode => Object.hash(markId, sourceKey, status, assetPath, error);
}

const Object _unset = Object();

class QuickMarkThumbnailStore {
  const QuickMarkThumbnailStore._();

  static Map<int, QuickMarkThumbnail> reconcile({
    required List<QuickMark> marks,
    required Map<int, QuickMarkThumbnail> current,
  }) {
    final next = <int, QuickMarkThumbnail>{};
    for (final mark in marks) {
      final sourceKey = sourceKeyForMark(mark);
      final existing = current[mark.id];
      if (existing != null && existing.sourceKey == sourceKey) {
        next[mark.id] = existing;
        continue;
      }
      next[mark.id] = QuickMarkThumbnail(
        markId: mark.id,
        sourceKey: sourceKey,
        status: QuickMarkThumbnailStatus.queued,
      );
    }
    return Map.unmodifiable(next);
  }

  static String sourceKeyForMark(QuickMark mark) {
    final buffer = StringBuffer()
      ..write('v1')
      ..write('|id=')
      ..write(mark.id)
      ..write('|pts=')
      ..write(mark.anchor.ptsUs)
      ..write('|dts=')
      ..write(mark.anchor.dtsUs)
      ..write('|dur=')
      ..write(mark.anchor.durationUs)
      ..write('|vac2=')
      ..write(mark.anchor.vac2FrameIndex ?? -1)
      ..write('|afi=')
      ..write(mark.anchor.analysisFrameIndex)
      ..write('|fim=')
      ..write(mark.anchor.frameIdentityMode)
      ..write('|spi=')
      ..write(mark.anchor.sourcePacketIndex)
      ..write('|sps=')
      ..write(mark.anchor.sourcePacketSize)
      ..write('|spp=')
      ..write(mark.anchor.sourcePacketPos)
      ..write('|sppts=')
      ..write(mark.anchor.sourcePacketPtsUs)
      ..write('|spdts=')
      ..write(mark.anchor.sourcePacketDtsUs)
      ..write('|shape=')
      ..write(mark.shape.name)
      ..write('|rect=')
      ..write(_rectKey(mark.sourceRect))
      ..write('|start=')
      ..write(_offsetKey(mark.sourceStart))
      ..write('|end=')
      ..write(_offsetKey(mark.sourceEnd))
      ..write('|color=')
      ..write(mark.color.toARGB32())
      ..write('|stroke=')
      ..write(_doubleKey(mark.strokeWidth));
    return buffer.toString();
  }

  static String _rectKey(Rect rect) {
    return [
      _doubleKey(rect.left),
      _doubleKey(rect.top),
      _doubleKey(rect.width),
      _doubleKey(rect.height),
    ].join(',');
  }

  static String _offsetKey(Offset? offset) {
    if (offset == null) return 'null';
    return '${_doubleKey(offset.dx)},${_doubleKey(offset.dy)}';
  }

  static String _doubleKey(double value) => value.toStringAsFixed(6);
}
