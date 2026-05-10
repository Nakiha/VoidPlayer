import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_cache.dart';

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
}
