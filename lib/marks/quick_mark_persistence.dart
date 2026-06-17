import 'dart:convert';
import 'dart:io';
import 'dart:ui';

import 'package:crypto/crypto.dart';
import 'package:path/path.dart' as p;
import 'package:sqlite3/sqlite3.dart';

import '../app_log.dart';
import '../app_paths.dart';
import '../storage/storage_catalog.dart';
import 'quick_mark.dart';
import 'quick_mark_media_hash.dart';

class QuickMarkMediaRef {
  final int fileId;
  final String path;
  final String mediaId;
  final String? mediaHash;

  QuickMarkMediaRef({
    required this.fileId,
    required this.path,
    String? mediaId,
    this.mediaHash,
  }) : mediaId = mediaId ?? mediaIdForPath(path);

  static String mediaIdForPath(String path) {
    final trimmed = path.trim();
    if (trimmed.isEmpty) return '';
    if (trimmed.contains('://')) return trimmed;
    return p.normalize(trimmed);
  }

  static String fallbackHashForMediaId(String mediaId) {
    return sha256.convert(utf8.encode(mediaId)).toString();
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

class SqliteQuickMarkRepository implements QuickMarkRepository {
  final String databasePath;

  SqliteQuickMarkRepository({required this.databasePath});

  factory SqliteQuickMarkRepository.defaultLocation() {
    final paths = AppPaths.current;
    return SqliteQuickMarkRepository(databasePath: paths.storageDatabaseFile);
  }

  @override
  Future<List<QuickMark>> loadForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
  ) async {
    if (mediaRefs.isEmpty) return const [];
    final refs = await _resolveMediaRefs(mediaRefs);
    final db = _open();
    try {
      _upsertMediaRefs(db, refs);
      final refsByHash = <String, List<QuickMarkMediaRef>>{};
      for (final ref in refs) {
        refsByHash.putIfAbsent(ref.mediaHash!, () => []).add(ref);
      }
      final marks = <QuickMark>[];
      for (final entry in refsByHash.entries) {
        final mediaHash = entry.key;
        _healFallbackRows(db, mediaHash, entry.value.first);
        final rows = db.select(
          'SELECT payload_json FROM marks '
          'WHERE media_hash = ? ORDER BY mark_id ASC',
          [mediaHash],
        );
        for (final row in rows) {
          final QuickMark mark;
          try {
            final decoded = jsonDecode(row['payload_json'] as String);
            if (decoded is! Map) continue;
            mark = _quickMarkFromJson(Map<String, Object?>.from(decoded));
          } catch (error, stack) {
            logWarning(
              '[QuickMarkRepository] skipped invalid mark payload: '
              'mediaHash=$mediaHash',
              error,
              stack,
            );
            continue;
          }
          for (final ref in entry.value) {
            marks.add(
              mark.copyWith(anchor: mark.anchor.copyWith(fileId: ref.fileId)),
            );
          }
        }
      }
      return marks;
    } finally {
      db.close();
    }
  }

  /// Moves mark rows persisted under the fallback hash (sha256 of the media
  /// id, used while the file content was unreadable) over to the content
  /// hash, so annotations made during degraded sessions stay visible.
  void _healFallbackRows(
    Database db,
    String contentHash,
    QuickMarkMediaRef ref,
  ) {
    final fallbackHash = QuickMarkMediaRef.fallbackHashForMediaId(ref.mediaId);
    if (fallbackHash == contentHash) return;
    final orphaned = db.select(
      'SELECT COUNT(*) AS n FROM marks WHERE media_hash = ?',
      [fallbackHash],
    );
    if ((orphaned.first['n'] as int) == 0) return;
    db.execute('BEGIN IMMEDIATE');
    try {
      db.execute(
        'INSERT OR IGNORE INTO marks '
        '(media_hash, mark_id, payload_json, updated_at_ms) '
        'SELECT ?, mark_id, payload_json, updated_at_ms FROM marks '
        'WHERE media_hash = ?',
        [contentHash, fallbackHash],
      );
      db.execute('DELETE FROM marks WHERE media_hash = ?', [fallbackHash]);
      db.execute('DELETE FROM media WHERE hash = ?', [fallbackHash]);
      db.execute('COMMIT');
      logWarning(
        '[QuickMarkRepository] migrated marks from fallback hash: '
        'mediaId=${ref.mediaId}',
      );
    } catch (error, stack) {
      db.execute('ROLLBACK');
      logWarning(
        '[QuickMarkRepository] fallback hash migration failed: '
        'mediaId=${ref.mediaId}',
        error,
        stack,
      );
    }
  }

  @override
  Future<void> saveForMediaRefs(
    List<QuickMarkMediaRef> mediaRefs,
    List<QuickMark> marks,
  ) async {
    if (mediaRefs.isEmpty) return;
    final refs = await _resolveMediaRefs(mediaRefs);
    final db = _open();
    try {
      db.execute('BEGIN IMMEDIATE');
      try {
        _upsertMediaRefs(db, refs);
        // When several refs share one quick mark hash, the first ref owns the
        // persisted set; otherwise the per-hash delete-and-reinsert below
        // would multiply the distributed copies on every round trip.
        final ownerByHash = <String, QuickMarkMediaRef>{};
        for (final ref in refs) {
          ownerByHash.putIfAbsent(ref.mediaHash!, () => ref);
        }
        final refsByFileId = {
          for (final ref in ownerByHash.values) ref.fileId: ref,
        };
        for (final hash in ownerByHash.keys) {
          db.execute('DELETE FROM marks WHERE media_hash = ?', [hash]);
        }
        final insert = db.prepare(
          'INSERT OR REPLACE INTO marks '
          '(media_hash, mark_id, payload_json, updated_at_ms) '
          'VALUES (?, ?, ?, ?)',
        );
        try {
          final now = DateTime.now().millisecondsSinceEpoch;
          for (final mark in marks) {
            final ref = refsByFileId[mark.fileId];
            if (ref == null) continue;
            final storedMark = mark.copyWith(
              anchor: mark.anchor.copyWith(fileId: 0),
            );
            insert.execute([
              ref.mediaHash,
              mark.id,
              jsonEncode(_quickMarkToJson(storedMark)),
              now,
            ]);
          }
        } finally {
          insert.close();
        }
        db.execute('COMMIT');
      } catch (error, stack) {
        db.execute('ROLLBACK');
        Error.throwWithStackTrace(error, stack);
      }
    } finally {
      db.close();
    }
  }

  Database _open() {
    Directory(p.dirname(databasePath)).createSync(recursive: true);
    final db = sqlite3.open(databasePath);
    db.execute('PRAGMA foreign_keys = ON');
    db.execute('PRAGMA journal_mode = WAL');
    StorageCatalog.ensureSchema(db);
    return db;
  }

  Future<List<QuickMarkMediaRef>> _resolveMediaRefs(
    List<QuickMarkMediaRef> refs,
  ) async {
    final resolved = <QuickMarkMediaRef>[];
    for (final ref in refs) {
      if (ref.mediaHash != null && ref.mediaHash!.isNotEmpty) {
        resolved.add(ref);
        continue;
      }
      resolved.add(
        QuickMarkMediaRef(
          fileId: ref.fileId,
          path: ref.path,
          mediaId: ref.mediaId,
          mediaHash: await _hashForRef(ref),
        ),
      );
    }
    return resolved;
  }

  Future<String> _hashForRef(QuickMarkMediaRef ref) async {
    Object? failure;
    StackTrace? failureStack;
    try {
      final file = File(ref.path);
      if (await file.exists()) return computeQuickMarkMediaHash(ref.path);
    } catch (error, stack) {
      failure = error;
      failureStack = stack;
    }
    if (!ref.mediaId.contains('://')) {
      logWarning(
        '[QuickMarkRepository] media hash fallback: '
        'path=${ref.path} mediaId=${ref.mediaId}',
        failure,
        failureStack,
      );
    }
    return QuickMarkMediaRef.fallbackHashForMediaId(ref.mediaId);
  }

  void _upsertMediaRefs(Database db, List<QuickMarkMediaRef> refs) {
    final now = DateTime.now().millisecondsSinceEpoch;
    final statement = db.prepare(
      'INSERT INTO media '
      '(hash, media_id, path, name, size, mtime_ms, first_seen_ms, last_accessed_ms) '
      'VALUES (?, ?, ?, ?, ?, ?, ?, ?) '
      'ON CONFLICT(hash) DO UPDATE SET '
      'media_id = excluded.media_id, path = excluded.path, name = excluded.name, '
      'size = excluded.size, mtime_ms = excluded.mtime_ms, '
      'last_accessed_ms = excluded.last_accessed_ms',
    );
    try {
      for (final ref in refs) {
        final stat = _safeFileStat(ref.path);
        statement.execute([
          ref.mediaHash,
          ref.mediaId,
          ref.path,
          p.basename(ref.path),
          stat.size,
          stat.modifiedMs,
          now,
          now,
        ]);
      }
    } finally {
      statement.close();
    }
  }

  ({int size, int modifiedMs}) _safeFileStat(String path) {
    try {
      final stat = File(path).statSync();
      if (stat.type == FileSystemEntityType.file) {
        return (
          size: stat.size,
          modifiedMs: stat.modified.millisecondsSinceEpoch,
        );
      }
    } catch (error, stack) {
      logFine(
        '[QuickMarkRepository] file stat fallback: path=$path',
        error,
        stack,
      );
    }
    return (size: 0, modifiedMs: 0);
  }
}

Map<String, Object?> _quickMarkToJson(QuickMark mark) => {
  'version': StorageCatalog.markPayloadVersion,
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
  'origin': mark.origin.name,
  if (mark.defectType != null) 'defectType': mark.defectType,
  if (mark.severity != null) 'severity': mark.severity,
  if (mark.attributes.isNotEmpty) 'attributes': mark.attributes,
};

QuickMark _quickMarkFromJson(Map<String, Object?> json) {
  final version = _readInt(json, 'version') ?? 1;
  if (version > StorageCatalog.markPayloadVersion) {
    throw const FormatException('unsupported quick mark payload version');
  }
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
    origin: _originFromJson(json['origin']),
    defectType: json['defectType'] is String
        ? json['defectType'] as String
        : null,
    severity: _severityFromJson(json['severity']),
    attributes: _attributesFromJson(json['attributes']),
  );
}

QuickMarkOrigin _originFromJson(Object? raw) {
  if (raw is String) {
    for (final origin in QuickMarkOrigin.values) {
      if (origin.name == raw) return origin;
    }
  }
  return QuickMarkOrigin.human;
}

int? _severityFromJson(Object? raw) {
  final value = raw is int ? raw : (raw is num ? raw.toInt() : null);
  if (value == null) return null;
  if (value < kQuickMarkSeverityMin || value > kQuickMarkSeverityMax) {
    return null;
  }
  return value;
}

Map<String, Object?> _attributesFromJson(Object? raw) {
  if (raw is! Map) return const {};
  return Map.unmodifiable(
    Map<String, Object?>.fromEntries(
      raw.entries
          .where((entry) => entry.key is String)
          .map((entry) => MapEntry(entry.key as String, entry.value)),
    ),
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
