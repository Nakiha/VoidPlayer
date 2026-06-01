import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:void_player/analysis/analysis_cache.dart';

Uint8List _minimalVac2() {
  const headerSize = 124;
  const sectionEntrySize = 56;
  const payloadOffset = headerSize + sectionEntrySize;
  const fileSize = payloadOffset + 2;
  final bytes = Uint8List(fileSize);
  final data = ByteData.sublistView(bytes);
  bytes.setAll(0, 'VAC2'.codeUnits);
  data.setUint16(4, AnalysisCache.currentVac2MajorVersion, Endian.little);
  data.setUint16(6, AnalysisCache.currentVac2MinorVersion, Endian.little);
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
  test('uses VAC2 base cache layout', () async {
    final hash = 'unit_vac2_${DateTime.now().microsecondsSinceEpoch}';
    addTearDown(() async {
      await AnalysisCache.deleteEntries([hash]);
    });

    expect(AnalysisCache.filesExist(hash), isFalse);
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
      '2_f0000000000000001_b0000000000000001_g0000000000000003_00000008_00000016.vck',
    );
    await Directory(p.dirname(overlayChunkPath)).create(recursive: true);
    await File(overlayChunkPath).writeAsBytes([1, 2, 3]);
    expect(AnalysisCache.hasOverlayChunks(hash), isTrue);
    expect(AnalysisCache.hasOverlayChunkForFrame(hash, 7), isFalse);
    expect(AnalysisCache.hasOverlayChunkForFrame(hash, 8), isTrue);
    expect(AnalysisCache.hasOverlayChunkForFrame(hash, 16), isTrue);
    expect(AnalysisCache.hasOverlayChunkForFrame(hash, 17), isFalse);
    expect(AnalysisCache.overlayChunkRanges(hash), [
      (startFrame: 8, endFrame: 16),
    ]);

    final adjacentOverlayChunkPath = p.join(
      AnalysisCache.overlayChunksDir(hash),
      '2_f0000000000000001_b0000000000000001_g0000000000000003_00000017_00000020.vck',
    );
    await File(adjacentOverlayChunkPath).writeAsBytes([4, 5, 6]);
    expect(AnalysisCache.overlayChunkRanges(hash), [
      (startFrame: 8, endFrame: 20),
    ]);
  });

  test('deletes per-hash VAC2 cache directories', () async {
    final hash = 'unit_delete_${DateTime.now().microsecondsSinceEpoch}';
    final basePath = AnalysisCache.vac2BasePath(hash);
    await Directory(p.dirname(basePath)).create(recursive: true);
    await File(basePath).writeAsBytes(_minimalVac2());
    final orphanVac1Path = p.join(AnalysisCache.dataDir, '$hash.vac');
    await File(orphanVac1Path).writeAsBytes([1, 2, 3]);

    final result = await AnalysisCache.deleteEntries([hash]);

    expect(result.deletedHashes, contains(hash));
    expect(File(basePath).existsSync(), isFalse);
    expect(Directory(AnalysisCache.hashDir(hash)).existsSync(), isFalse);
    expect(File(orphanVac1Path).existsSync(), isFalse);
  });

  test('clears derived chunks while keeping VAC2 base', () async {
    final hash = 'unit_chunks_${DateTime.now().microsecondsSinceEpoch}';
    final basePath = AnalysisCache.vac2BasePath(hash);
    await Directory(p.dirname(basePath)).create(recursive: true);
    await File(basePath).writeAsBytes(_minimalVac2());

    final overlayChunkPath = p.join(
      AnalysisCache.overlayChunksDir(hash),
      '2_f0000000000000001_b0000000000000001_g0000000000000003_00000000_00000000.vck',
    );
    final exactChunkPath = p.join(
      AnalysisCache.chunksDir(hash),
      'frame_summary_exact',
      'exact.vck',
    );
    await Directory(p.dirname(overlayChunkPath)).create(recursive: true);
    await File(overlayChunkPath).writeAsBytes([1, 2, 3]);
    await Directory(p.dirname(exactChunkPath)).create(recursive: true);
    await File(exactChunkPath).writeAsBytes([4, 5, 6]);

    addTearDown(() async {
      await AnalysisCache.deleteEntries([hash]);
    });

    final result = await AnalysisCache.clearDerivedChunks(hashes: [hash]);

    expect(result.deletedHashes, contains(hash));
    expect(File(basePath).existsSync(), isTrue);
    expect(Directory(AnalysisCache.chunksDir(hash)).existsSync(), isFalse);
  });
}
