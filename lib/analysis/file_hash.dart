import 'dart:io';
import 'dart:isolate';

import 'package:crypto/crypto.dart';

Future<String> computeFileSha256(String path) =>
    Isolate.run(() => computeFileSha256Sync(path));

String computeFileSha256Sync(String path) {
  final sink = _DigestSink();
  final input = sha256.startChunkedConversion(sink);
  final file = File(path).openSync();
  try {
    const chunkSize = 1024 * 1024;
    while (true) {
      final chunk = file.readSync(chunkSize);
      if (chunk.isEmpty) break;
      input.add(chunk);
    }
    input.close();
  } finally {
    file.closeSync();
  }
  return sink.digest.toString();
}

class _DigestSink implements Sink<Digest> {
  Digest? _digest;

  Digest get digest => _digest!;

  @override
  void add(Digest data) {
    _digest = data;
  }

  @override
  void close() {}
}
