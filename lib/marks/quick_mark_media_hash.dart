import 'dart:io';
import 'dart:isolate';

import 'package:crypto/crypto.dart';

const int kQuickMarkMediaHashPrefixBytes = 1024 * 1024;

/// Fast media identity used by quick marks.
///
/// Quick marks need a stable-enough key for reopening/moving files, but doing a
/// full-file SHA-256 while opening large videos is too expensive. This hashes
/// only the first MiB and intentionally treats later bytes as unchanged.
Future<String> computeQuickMarkMediaHash(String path) =>
    Isolate.run(() => computeQuickMarkMediaHashSync(path));

String computeQuickMarkMediaHashSync(String path) {
  final file = File(path);
  final length = file.lengthSync();
  final sink = _DigestSink();
  final input = sha256.startChunkedConversion(sink);
  final handle = file.openSync();
  try {
    var remaining = length < kQuickMarkMediaHashPrefixBytes
        ? length
        : kQuickMarkMediaHashPrefixBytes;
    while (remaining > 0) {
      final chunkSize = remaining > 1024 * 1024
          ? 1024 * 1024
          : remaining.toInt();
      final chunk = handle.readSync(chunkSize);
      if (chunk.isEmpty) break;
      input.add(chunk);
      remaining -= chunk.length;
    }
    input.close();
  } finally {
    handle.closeSync();
  }
  return 'qmf1_${sink.digest}';
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
