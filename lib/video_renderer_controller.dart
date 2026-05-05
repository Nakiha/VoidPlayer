import 'package:flutter/services.dart';

import 'native_player/native_player_protocol.dart';

export 'native_player/native_player_protocol.dart';

class NativePlayerController {
  static const MethodChannel _channel = MethodChannel(NativePlayerChannel.name);

  int? _textureId;
  bool _disposed = false;
  Future<CreatePlayerResult>? _createInFlight;
  Future<void>? _destroyInFlight;
  Future<void>? _disposeFuture;
  int? _viewportBackgroundColor;

  int? get textureId => _textureId;
  bool get isDisposed => _disposed;
  bool get hasPlayer => _textureId != null;

  void _ensureAlive() {
    if (_disposed) {
      throw StateError('NativePlayerController is disposed');
    }
  }

  void _ensurePlayer(String method) {
    _ensureAlive();
    if (_textureId == null) {
      throw StateError('$method called before createPlayer');
    }
  }

  bool _hasPlayerForCommand() {
    _ensureAlive();
    return _textureId != null;
  }

  Future<CreatePlayerResult> createPlayer(
    List<String> videoPaths, {
    int width = 1920,
    int height = 1080,
  }) {
    _ensureAlive();
    if (_textureId != null) {
      throw StateError('Player already created');
    }
    final existing = _createInFlight;
    if (existing != null) return existing;

    late final Future<CreatePlayerResult> future;
    future = _createPlayerImpl(videoPaths, width: width, height: height)
        .whenComplete(() {
          if (identical(_createInFlight, future)) {
            _createInFlight = null;
          }
        });
    _createInFlight = future;
    return future;
  }

  Future<CreatePlayerResult> _createPlayerImpl(
    List<String> videoPaths, {
    required int width,
    required int height,
  }) async {
    final destroying = _destroyInFlight;
    if (destroying != null) {
      await destroying;
      _ensureAlive();
    }
    final map = await _channel
        .invokeMethod<Map<dynamic, dynamic>>(NativePlayerMethods.createPlayer, {
          NativePlayerKeys.videoPaths: videoPaths,
          NativePlayerKeys.width: width,
          NativePlayerKeys.height: height,
        });
    final result = CreatePlayerResult.fromMap(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.createPlayer),
    );
    _textureId = result.textureId;
    if (_disposed) {
      _textureId = null;
      await _channel.invokeMethod<void>(NativePlayerMethods.destroyPlayer);
      throw StateError('NativePlayerController is disposed');
    }
    final backgroundColor = _viewportBackgroundColor;
    if (backgroundColor != null) {
      await setViewportBackgroundColor(backgroundColor);
    }
    _ensureAlive();
    return result;
  }

  Future<void> play() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.play);
  }

  Future<void> pause() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.pause);
  }

  Future<void> seek(int ptsUs) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.seek, {
      NativePlayerKeys.ptsUs: ptsUs,
    });
  }

  Future<void> setSpeed(double speed) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.setSpeed, {
      NativePlayerKeys.speed: speed,
    });
  }

  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.setLoopRange, {
      NativePlayerKeys.enabled: enabled,
      NativePlayerKeys.startUs: startUs,
      NativePlayerKeys.endUs: endUs,
    });
  }

  Future<void> setAudibleTrack(int? fileId) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.setAudibleTrack, {
      NativePlayerKeys.fileId: fileId ?? -1,
    });
  }

  Future<void> resize(int width, int height) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.resize, {
      NativePlayerKeys.width: width,
      NativePlayerKeys.height: height,
    });
  }

  Future<void> setViewportBackgroundColor(int colorValue) {
    _ensureAlive();
    _viewportBackgroundColor = colorValue;
    if (_textureId == null) return Future.value();
    return _channel.invokeMethod<void>(
      NativePlayerMethods.setViewportBackgroundColor,
      {NativePlayerKeys.color: colorValue},
    );
  }

  Future<ViewportCapture> captureViewport({String? outputPath}) async {
    _ensurePlayer(NativePlayerMethods.captureViewport);
    final args = <String, dynamic>{};
    if (outputPath != null) {
      args[NativePlayerKeys.outputPath] = outputPath;
    }
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.captureViewport,
      args,
    );
    return ViewportCapture.fromMap(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.captureViewport),
    );
  }

  Future<void> stepForward() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.stepForward);
  }

  Future<void> stepBackward() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(NativePlayerMethods.stepBackward);
  }

  Future<int> currentPts() async {
    if (!_hasPlayerForCommand()) return 0;
    return await _channel.invokeMethod<int>(NativePlayerMethods.currentPts) ??
        0;
  }

  Future<int> duration() async {
    if (!_hasPlayerForCommand()) return 0;
    return await _channel.invokeMethod<int>(NativePlayerMethods.duration) ?? 0;
  }

  Future<bool> isPlaying() async {
    if (!_hasPlayerForCommand()) return false;
    return await _channel.invokeMethod<bool>(NativePlayerMethods.isPlaying) ??
        false;
  }

  /// Atomically apply layout state and trigger redraw if paused.
  Future<void> applyLayout(LayoutState state) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _channel.invokeMethod<void>(
      NativePlayerMethods.applyLayout,
      state.toMap(),
    );
  }

  /// Get a snapshot of the current layout state.
  Future<LayoutState> getLayout() async {
    _ensurePlayer(NativePlayerMethods.getLayout);
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.getLayout,
    );
    return LayoutState.fromMap(map ?? {});
  }

  /// Add a video track at the first empty slot.
  Future<TrackInfo> addTrack(String videoPath) async {
    _ensurePlayer(NativePlayerMethods.addTrack);
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.addTrack,
      {NativePlayerKeys.path: videoPath},
    );
    return NativePlayerPayloads.trackInfoFromValue(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.addTrack),
      NativePlayerMethods.addTrack,
    );
  }

  /// Remove a track by file_id.
  Future<void> removeTrack(int fileId) {
    _ensurePlayer(NativePlayerMethods.removeTrack);
    return _channel.invokeMethod<void>(NativePlayerMethods.removeTrack, {
      NativePlayerKeys.fileId: fileId,
    });
  }

  /// Destroy the native player and texture while keeping this controller
  /// reusable for a future [createPlayer] call.
  Future<void> destroyPlayerOnly() {
    _ensureAlive();
    return _destroyPlayer(markDisposed: false);
  }

  /// Set per-track sync offset in microseconds.
  Future<void> setTrackOffset({required int fileId, required int offsetUs}) {
    _ensurePlayer(NativePlayerMethods.setTrackOffset);
    return _channel.invokeMethod<void>(NativePlayerMethods.setTrackOffset, {
      NativePlayerKeys.fileId: fileId,
      NativePlayerKeys.offsetUs: offsetUs,
    });
  }

  /// Get current track info list.
  Future<List<TrackInfo>> getTracks() async {
    if (!_hasPlayerForCommand()) return const [];
    final list = await _channel.invokeMethod<List<dynamic>>(
      NativePlayerMethods.getTracks,
    );
    return list
            ?.map(
              (e) => NativePlayerPayloads.trackInfoFromValue(
                e,
                NativePlayerMethods.getTracks,
              ),
            )
            .toList() ??
        [];
  }

  /// Get diagnostics data (placeholder, requires native counters).
  Future<Map<String, dynamic>> getDiagnostics() async {
    if (!_hasPlayerForCommand()) return const {};
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.getDiagnostics,
    );
    return Map<String, dynamic>.from(map ?? {});
  }

  Future<void> dispose() async {
    final existing = _disposeFuture;
    if (existing != null) return existing;
    _disposed = true;
    _disposeFuture = _destroyPlayer(markDisposed: true);
    return _disposeFuture!;
  }

  Future<void> _destroyPlayer({required bool markDisposed}) {
    if (markDisposed) {
      _disposed = true;
    } else {
      _ensureAlive();
    }
    final existing = _destroyInFlight;
    if (existing != null) return existing;
    late final Future<void> future;
    future = _destroyPlayerImpl().whenComplete(() {
      if (identical(_destroyInFlight, future)) {
        _destroyInFlight = null;
      }
    });
    _destroyInFlight = future;
    return future;
  }

  Future<void> _destroyPlayerImpl() async {
    final creating = _createInFlight;
    if (creating != null) {
      try {
        await creating;
      } catch (_) {}
    }
    final textureId = _textureId;
    _textureId = null;
    if (textureId != null) {
      await _channel.invokeMethod<void>(NativePlayerMethods.destroyPlayer);
    }
  }
}
