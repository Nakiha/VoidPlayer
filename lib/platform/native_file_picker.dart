import 'package:flutter/services.dart';

class NativePickedFile {
  const NativePickedFile({
    required this.path,
    this.securityScopedBookmarkBase64,
  });

  final String path;
  final String? securityScopedBookmarkBase64;

  static NativePickedFile fromPlatformValue(Object? value) {
    if (value is String) {
      return NativePickedFile(path: value);
    }
    if (value is Map) {
      final path = value['path'];
      if (path is! String) {
        throw FormatException('Native picked file is missing a string path');
      }
      final bookmark = value['securityScopedBookmarkBase64'];
      return NativePickedFile(
        path: path,
        securityScopedBookmarkBase64: bookmark is String && bookmark.isNotEmpty
            ? bookmark
            : null,
      );
    }
    throw FormatException(
      'Unsupported native picked file payload: ${value.runtimeType}',
    );
  }
}

abstract interface class NativeFilePicker {
  bool get isAvailable;

  Future<List<NativePickedFile>?> pickFileEntries({bool allowMultiple = true});

  Future<List<String>?> pickFiles({bool allowMultiple = true});
}

class MethodChannelNativeFilePicker implements NativeFilePicker {
  const MethodChannelNativeFilePicker([
    this._channel = const MethodChannel('video_renderer'),
  ]);

  final MethodChannel _channel;

  @override
  bool get isAvailable => true;

  @override
  Future<List<NativePickedFile>?> pickFileEntries({
    bool allowMultiple = true,
  }) async {
    final result = await _channel.invokeMethod<List<dynamic>>('pickFiles', {
      'allowMultiple': allowMultiple,
    });
    if (result == null || result.isEmpty) return null;
    final entries = result
        .map(NativePickedFile.fromPlatformValue)
        .where((entry) => entry.path.isNotEmpty)
        .toList(growable: false);
    return entries.isEmpty ? null : entries;
  }

  @override
  Future<List<String>?> pickFiles({bool allowMultiple = true}) async {
    final entries = await pickFileEntries(allowMultiple: allowMultiple);
    if (entries == null || entries.isEmpty) return null;
    return entries.map((entry) => entry.path).toList(growable: false);
  }
}

class UnsupportedNativeFilePicker implements NativeFilePicker {
  const UnsupportedNativeFilePicker({
    this.message = 'Native file picking is not available on this platform yet.',
  });

  final String message;

  @override
  bool get isAvailable => false;

  @override
  Future<List<NativePickedFile>?> pickFileEntries({bool allowMultiple = true}) {
    throw UnsupportedError(message);
  }

  @override
  Future<List<String>?> pickFiles({bool allowMultiple = true}) {
    throw UnsupportedError(message);
  }
}
