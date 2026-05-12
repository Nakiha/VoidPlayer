import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/analysis/analysis_cache.dart';

Uint8List _minimalVac1() {
  const headerSize = 64;
  const sectionEntrySize = 48;
  const payloadOffset = headerSize + sectionEntrySize;
  const fileSize = payloadOffset + 2;
  final bytes = Uint8List(fileSize);
  final data = ByteData.sublistView(bytes);
  bytes.setAll(0, 'VAC1'.codeUnits);
  data.setUint16(4, AnalysisCache.currentVacVersion, Endian.little);
  data.setUint16(6, headerSize, Endian.little);
  data.setUint16(8, sectionEntrySize, Endian.little);
  data.setUint16(10, 1, Endian.little);
  data.setUint64(16, headerSize, Endian.little);
  data.setUint64(24, fileSize, Endian.little);
  bytes.setAll(headerSize, 'META'.codeUnits);
  data.setUint64(headerSize + 8, payloadOffset, Endian.little);
  data.setUint64(headerSize + 16, 2, Endian.little);
  bytes.setAll(payloadOffset, '{}'.codeUnits);
  return bytes;
}

Uint8List _minimalVac2() {
  const headerSize = 124;
  const sectionEntrySize = 56;
  const payloadOffset = headerSize + sectionEntrySize;
  const fileSize = payloadOffset + 2;
  final bytes = Uint8List(fileSize);
  final data = ByteData.sublistView(bytes);
  bytes.setAll(0, 'VAC2'.codeUnits);
  data.setUint16(4, AnalysisCache.currentVac2MajorVersion, Endian.little);
  data.setUint16(6, 0, Endian.little);
  data.setUint16(8, headerSize, Endian.little);
  data.setUint16(10, sectionEntrySize, Endian.little);
  data.setUint32(12, 1, Endian.little);
  data.setUint64(52, headerSize, Endian.little);
  data.setUint64(60, fileSize, Endian.little);
  bytes.setAll(headerSize, 'META'.codeUnits);
  data.setUint64(headerSize + 8, payloadOffset, Endian.little);
  data.setUint64(headerSize + 16, 2, Endian.little);
  bytes.setAll(payloadOffset, '{}'.codeUnits);
  return bytes;
}

void main() {
  test('reads VAC container version from header', () async {
    final dir = await Directory.systemTemp.createTemp('void_player_vac_test_');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final path = '${dir.path}${Platform.pathSeparator}sample.vac';
    final bytes = Uint8List(64);
    bytes[0] = 0x56;
    bytes[1] = 0x41;
    bytes[2] = 0x43;
    bytes[3] = 0x31;
    ByteData.sublistView(bytes).setUint16(4, 7, Endian.little);
    await File(path).writeAsBytes(bytes);

    expect(AnalysisCache.readVacVersion(path), 7);
  });

  test('ignores non-VAC files when reading container version', () async {
    final dir = await Directory.systemTemp.createTemp('void_player_vac_test_');
    addTearDown(() async {
      if (await dir.exists()) await dir.delete(recursive: true);
    });
    final path = '${dir.path}${Platform.pathSeparator}sample.vac';
    await File(path).writeAsBytes([0, 1, 2, 3, 4, 5]);

    expect(AnalysisCache.readVacVersion(path), isNull);
  });

  test('uses VAC2 base cache and ignores legacy VAC1 as a read path', () async {
    final hash = 'unit_vac2_${DateTime.now().microsecondsSinceEpoch}';
    addTearDown(() async {
      await AnalysisCache.deleteEntries([hash]);
    });

    final legacyPath = AnalysisCache.legacyAnalysisPath(hash);
    await Directory(p.dirname(legacyPath)).create(recursive: true);
    await File(legacyPath).writeAsBytes(_minimalVac1());

    expect(AnalysisCache.filesExist(hash), isFalse);
    expect(AnalysisCache.hasLegacyAnalysis(hash), isFalse);
    expect(AnalysisCache.hasOverlayChunks(hash), isFalse);
    expect(AnalysisCache.analysisPath(hash), AnalysisCache.vac2BasePath(hash));

    final basePath = AnalysisCache.vac2BasePath(hash);
    await Directory(p.dirname(basePath)).create(recursive: true);
    await File(basePath).writeAsBytes(_minimalVac2());

    expect(AnalysisCache.filesExist(hash), isTrue);
    expect(AnalysisCache.analysisPath(hash), basePath);
    expect(AnalysisCache.hashForAnalysisPath(basePath), hash);
    expect(AnalysisCache.hasIncompleteContainer(hash), isFalse);

    final overlayChunkPath = p.join(
      AnalysisCache.overlayChunksDir(hash),
      'overlay_00000000_00000000.vck',
    );
    await Directory(p.dirname(overlayChunkPath)).create(recursive: true);
    await File(overlayChunkPath).writeAsBytes([1, 2, 3]);
    expect(AnalysisCache.hasOverlayChunks(hash), isTrue);
  });

  test('deletes per-hash VAC2 cache directories', () async {
    final hash = 'unit_delete_${DateTime.now().microsecondsSinceEpoch}';
    final basePath = AnalysisCache.vac2BasePath(hash);
    await Directory(p.dirname(basePath)).create(recursive: true);
    await File(basePath).writeAsBytes(_minimalVac2());
    await File(
      AnalysisCache.legacyAnalysisPath(hash),
    ).writeAsBytes(_minimalVac1());

    final result = await AnalysisCache.deleteEntries([hash]);

    expect(result.deletedHashes, contains(hash));
    expect(File(basePath).existsSync(), isFalse);
    expect(Directory(AnalysisCache.hashDir(hash)).existsSync(), isFalse);
    expect(File(AnalysisCache.legacyAnalysisPath(hash)).existsSync(), isFalse);
  });
}
