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

class NativeBookmarkActivation {
  const NativeBookmarkActivation({
    required this.path,
    required this.requestedPath,
    required this.activated,
    required this.stale,
    this.securityScopedBookmarkBase64,
    this.error,
  });

  final String path;
  final String requestedPath;
  final bool activated;
  final bool stale;
  final String? securityScopedBookmarkBase64;
  final String? error;

  static NativeBookmarkActivation fromPlatformValue(Object? value) {
    if (value is! Map) {
      throw FormatException(
        'Unsupported bookmark activation payload: ${value.runtimeType}',
      );
    }
    final path = value['path'];
    final requestedPath = value['requestedPath'];
    final bookmark = value['securityScopedBookmarkBase64'];
    final error = value['error'];
    return NativeBookmarkActivation(
      path: path is String ? path : '',
      requestedPath: requestedPath is String ? requestedPath : '',
      activated: value['activated'] == true,
      stale: value['stale'] == true,
      securityScopedBookmarkBase64: bookmark is String && bookmark.isNotEmpty
          ? bookmark
          : null,
      error: error is String && error.isNotEmpty ? error : null,
    );
  }
}

abstract interface class NativeFilePicker {
  bool get isAvailable;

  Future<List<NativePickedFile>?> pickFileEntries({bool allowMultiple = true});

  Future<List<String>?> pickFiles({bool allowMultiple = true});

  Future<List<NativeBookmarkActivation>> activateSecurityScopedBookmarks(
    Map<String, String> bookmarksByPath,
  );
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

  @override
  Future<List<NativeBookmarkActivation>> activateSecurityScopedBookmarks(
    Map<String, String> bookmarksByPath,
  ) async {
    if (bookmarksByPath.isEmpty) return const [];
    try {
      final result = await _channel.invokeMethod<List<dynamic>>(
        'activateSecurityScopedBookmarks',
        {'bookmarks': bookmarksByPath},
      );
      if (result == null || result.isEmpty) return const [];
      return result
          .map(NativeBookmarkActivation.fromPlatformValue)
          .toList(growable: false);
    } on MissingPluginException {
      return const [];
    }
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

  @override
  Future<List<NativeBookmarkActivation>> activateSecurityScopedBookmarks(
    Map<String, String> bookmarksByPath,
  ) async {
    return const [];
  }
}
