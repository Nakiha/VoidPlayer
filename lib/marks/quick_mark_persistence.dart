import 'dart:convert';
import 'dart:io';
import 'dart:ui';

import 'package:path/path.dart' as p;

import '../app_log.dart';
import '../app_paths.dart';
import '../utils/atomic_file.dart';
import 'quick_mark.dart';

class QuickMarkMediaRef {
  final int fileId;
  final String path;
  final String mediaId;

  QuickMarkMediaRef({required this.fileId, required this.path, String? mediaId})
    : mediaId = mediaId ?? mediaIdForPath(path);

  static String mediaIdForPath(String path) {
    final trimmed = path.trim();
    if (trimmed.isEmpty) return '';
    if (trimmed.contains('://')) return trimmed;
    return p.normalize(trimmed);
  }
}

abstract class QuickMarkRepository {
  Future<List<QuickMark>> loadForMediaRefs(List<QuickMarkMediaRef> mediaRefs);
  Future<void> saveForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
    List<QuickMark> marks,
  );
}

class NoopQuickMarkRepository implements QuickMarkRepository {
  const NoopQuickMarkRepository();

  @override
  Future<List<QuickMark>> loadForMediaRefs(List<QuickMarkMediaRef> mediaRefs) =>
      Future.value(const []);

  @override
  Future<void> saveForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
    List<QuickMark> marks,
  ) => Future.value();
}

class FileQuickMarkRepository implements QuickMarkRepository {
  static const String _format = 'voidplayer.quick_marks';
  static const int _version = 1;

  final File file;

  FileQuickMarkRepository(this.file);

  factory FileQuickMarkRepository.defaultLocation() {
    return FileQuickMarkRepository(
      File(p.join(AppPaths.current.rootDir, 'annotations', 'local.vpmarks')),
    );
  }

  @override
  Future<List<QuickMark>> loadForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
  ) async {
    if (mediaRefs.isEmpty) return const [];
    final document = await _readDocument();
    final mediaById = _firstMediaRefById(mediaRefs);
    final marks = <QuickMark>[];
    for (final entry in document.marks) {
      final mediaRef = mediaById[entry.mediaId];
      if (mediaRef == null) continue;
      marks.add(
        entry.mark.copyWith(
          anchor: entry.mark.anchor.copyWith(fileId: mediaRef.fileId),
        ),
      );
    }
    return marks;
  }

  @override
  Future<void> saveForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
    List<QuickMark> marks,
  ) async {
    if (mediaRefs.isEmpty) return;
    final document = await _readDocument();
    final mediaByFileId = {for (final ref in mediaRefs) ref.fileId: ref};
    final activeMediaIds = mediaRefs.map((ref) => ref.mediaId).toSet();
    final nextMarks = [
      for (final entry in document.marks)
        if (!activeMediaIds.contains(entry.mediaId)) entry,
      for (final mark in marks)
        if (mediaByFileId[mark.fileId] case final mediaRef?)
          _StoredQuickMark(
            mediaId: mediaRef.mediaId,
            mark: mark.copyWith(anchor: mark.anchor.copyWith(fileId: 0)),
          ),
    ];
    final nextDocument = _QuickMarkDocument(marks: nextMarks);
    await AtomicFileWriter.writeString(
      file,
      const JsonEncoder.withIndent('  ').convert(nextDocument.toJson()),
    );
  }

  Map<String, QuickMarkMediaRef> _firstMediaRefById(
    List<QuickMarkMediaRef> refs,
  ) {
    final result = <String, QuickMarkMediaRef>{};
    for (final ref in refs) {
      result.putIfAbsent(ref.mediaId, () => ref);
    }
    return result;
  }

  Future<_QuickMarkDocument> _readDocument() async {
    if (!await file.exists()) return const _QuickMarkDocument();
    try {
      final decoded = jsonDecode(await file.readAsString());
      if (decoded is Map<String, Object?>) {
        return _QuickMarkDocument.fromJson(decoded);
      }
      log.warning('[QuickMark] ${file.path} root is not a JSON object');
    } on FormatException catch (e, stack) {
      log.warning('[QuickMark] failed to parse ${file.path}', e, stack);
    } on FileSystemException catch (e, stack) {
      log.warning('[QuickMark] failed to read ${file.path}', e, stack);
    }
    return const _QuickMarkDocument();
  }
}

class _QuickMarkDocument {
  final List<_StoredQuickMark> marks;

  const _QuickMarkDocument({this.marks = const []});

  factory _QuickMarkDocument.fromJson(Map<String, Object?> json) {
    if (json['format'] != FileQuickMarkRepository._format) {
      return const _QuickMarkDocument();
    }
    final version = json['version'];
    if (version is! int || version > FileQuickMarkRepository._version) {
      return const _QuickMarkDocument();
    }
    final rawMarks = json['marks'];
    if (rawMarks is! List) return const _QuickMarkDocument();
    final marks = <_StoredQuickMark>[];
    for (final raw in rawMarks) {
      if (raw is! Map) continue;
      try {
        marks.add(_StoredQuickMark.fromJson(Map<String, Object?>.from(raw)));
      } on FormatException {
        continue;
      }
    }
    return _QuickMarkDocument(marks: marks);
  }

  Map<String, Object?> toJson() => {
    'format': FileQuickMarkRepository._format,
    'version': FileQuickMarkRepository._version,
    'updatedAt': DateTime.now().toUtc().toIso8601String(),
    'marks': [for (final mark in marks) mark.toJson()],
  };
}

class _StoredQuickMark {
  final String mediaId;
  final QuickMark mark;

  const _StoredQuickMark({required this.mediaId, required this.mark});

  factory _StoredQuickMark.fromJson(Map<String, Object?> json) {
    final mediaId = json['mediaId'];
    final markJson = json['mark'];
    if (mediaId is! String || mediaId.isEmpty || markJson is! Map) {
      throw const FormatException('invalid stored quick mark');
    }
    return _StoredQuickMark(
      mediaId: mediaId,
      mark: _quickMarkFromJson(Map<String, Object?>.from(markJson)),
    );
  }

  Map<String, Object?> toJson() => {
    'mediaId': mediaId,
    'mark': _quickMarkToJson(mark),
  };
}

Map<String, Object?> _quickMarkToJson(QuickMark mark) => {
  'id': mark.id,
  'anchor': _anchorToJson(mark.anchor),
  'sourceRect': _rectToJson(mark.sourceRect),
  if (mark.sourceStart != null) 'sourceStart': _offsetToJson(mark.sourceStart!),
  if (mark.sourceEnd != null) 'sourceEnd': _offsetToJson(mark.sourceEnd!),
  'colorArgb': mark.color.toARGB32(),
  'strokeWidth': mark.strokeWidth,
  'shape': mark.shape.name,
  'text': mark.text,
  'textBold': mark.textBold,
  'textFontSize': mark.textFontSize,
  'syncAcrossTracks': mark.syncAcrossTracks,
};

QuickMark _quickMarkFromJson(Map<String, Object?> json) {
  final id = _readInt(json, 'id');
  final anchorJson = json['anchor'];
  final rectJson = json['sourceRect'];
  if (id == null || anchorJson is! Map || rectJson is! Map) {
    throw const FormatException('invalid quick mark payload');
  }
  return QuickMark(
    id: id,
    anchor: _anchorFromJson(Map<String, Object?>.from(anchorJson)),
    sourceRect: _rectFromJson(Map<String, Object?>.from(rectJson)),
    sourceStart: _optionalOffsetFromJson(json['sourceStart']),
    sourceEnd: _optionalOffsetFromJson(json['sourceEnd']),
    color: Color(_readInt(json, 'colorArgb') ?? 0xFFFF3B30),
    strokeWidth: _readDouble(json, 'strokeWidth') ?? 3.0,
    shape: _shapeFromJson(json['shape']),
    text: json['text'] is String ? json['text'] as String : '',
    textBold: json['textBold'] is bool ? json['textBold'] as bool : true,
    textFontSize: _readDouble(json, 'textFontSize') ?? 14.0,
    syncAcrossTracks: json['syncAcrossTracks'] is bool
        ? json['syncAcrossTracks'] as bool
        : true,
  );
}

Map<String, Object?> _anchorToJson(QuickMarkAnchor anchor) => {
  'ptsUs': anchor.ptsUs,
  'dtsUs': anchor.dtsUs,
  'durationUs': anchor.durationUs,
  if (anchor.vac2FrameIndex != null) 'vac2FrameIndex': anchor.vac2FrameIndex,
  'analysisFrameIndex': anchor.analysisFrameIndex,
  'frameIdentityMode': anchor.frameIdentityMode,
  'sourcePacketIndex': anchor.sourcePacketIndex,
  'sourcePacketSize': anchor.sourcePacketSize,
  'sourcePacketPos': anchor.sourcePacketPos,
  'sourcePacketPtsUs': anchor.sourcePacketPtsUs,
  'sourcePacketDtsUs': anchor.sourcePacketDtsUs,
};

QuickMarkAnchor _anchorFromJson(Map<String, Object?> json) {
  return QuickMarkAnchor(
    fileId: 0,
    ptsUs: _readInt(json, 'ptsUs') ?? 0,
    dtsUs: _readInt(json, 'dtsUs') ?? (_readInt(json, 'ptsUs') ?? 0),
    durationUs: _readInt(json, 'durationUs') ?? 0,
    vac2FrameIndex: _readInt(json, 'vac2FrameIndex'),
    analysisFrameIndex: _readInt(json, 'analysisFrameIndex') ?? -1,
    frameIdentityMode: _readInt(json, 'frameIdentityMode') ?? 0,
    sourcePacketIndex: _readInt(json, 'sourcePacketIndex') ?? -1,
    sourcePacketSize: _readInt(json, 'sourcePacketSize') ?? 0,
    sourcePacketPos: _readInt(json, 'sourcePacketPos') ?? -1,
    sourcePacketPtsUs:
        _readInt(json, 'sourcePacketPtsUs') ?? QuickMarkAnchor.noTimestampUs,
    sourcePacketDtsUs:
        _readInt(json, 'sourcePacketDtsUs') ?? QuickMarkAnchor.noTimestampUs,
  );
}

Map<String, Object?> _rectToJson(Rect rect) => {
  'left': rect.left,
  'top': rect.top,
  'width': rect.width,
  'height': rect.height,
};

Rect _rectFromJson(Map<String, Object?> json) {
  return Rect.fromLTWH(
    _readDouble(json, 'left') ?? 0,
    _readDouble(json, 'top') ?? 0,
    _readDouble(json, 'width') ?? 0,
    _readDouble(json, 'height') ?? 0,
  );
}

Map<String, Object?> _offsetToJson(Offset offset) => {
  'dx': offset.dx,
  'dy': offset.dy,
};

Offset? _optionalOffsetFromJson(Object? raw) {
  if (raw is! Map) return null;
  final json = Map<String, Object?>.from(raw);
  final dx = _readDouble(json, 'dx');
  final dy = _readDouble(json, 'dy');
  if (dx == null || dy == null) return null;
  return Offset(dx, dy);
}

QuickMarkShape _shapeFromJson(Object? raw) {
  if (raw is String) {
    for (final shape in QuickMarkShape.values) {
      if (shape.name == raw) return shape;
    }
  }
  return QuickMarkShape.rectangle;
}

int? _readInt(Map<String, Object?> json, String key) {
  final value = json[key];
  if (value is int) return value;
  if (value is num) return value.toInt();
  return null;
}

double? _readDouble(Map<String, Object?> json, String key) {
  final value = json[key];
  if (value is num) return value.toDouble();
  return null;
}
