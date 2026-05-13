import 'native_player/native_player_api.dart';
import 'native_player/native_player_events.dart';
import 'native_player/native_player_protocol.dart';

export 'native_player/native_player_api.dart';
export 'native_player/native_player_events.dart';
export 'native_player/native_player_protocol.dart';

class NativePlayerController {
  final NativePlayerApi _api;
  int? _textureId;
  bool _disposed = false;
  Future<CreatePlayerResult>? _createInFlight;
  Future<void>? _destroyInFlight;
  Future<void>? _disposeFuture;
  int? _viewportBackgroundColor;

  NativePlayerController({NativePlayerApi? api})
    : _api = api ?? const MethodChannelNativePlayerApi();

  int? get textureId => _textureId;
  bool get isDisposed => _disposed;
  bool get hasPlayer => _textureId != null;
  Stream<NativePlayerEvent> get events => _api.events;

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
    bool useHardwareDecode = true,
  }) {
    _ensureAlive();
    if (_textureId != null) {
      throw StateError('Player already created');
    }
    final existing = _createInFlight;
    if (existing != null) return existing;

    late final Future<CreatePlayerResult> future;
    future =
        _createPlayerImpl(
          videoPaths,
          width: width,
          height: height,
          useHardwareDecode: useHardwareDecode,
        ).whenComplete(() {
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
    required bool useHardwareDecode,
  }) async {
    final destroying = _destroyInFlight;
    if (destroying != null) {
      await destroying;
      _ensureAlive();
    }
    final result = await _api.createPlayer(
      videoPaths: videoPaths,
      width: width,
      height: height,
      useHardwareDecode: useHardwareDecode,
    );
    _textureId = result.textureId;
    if (_disposed) {
      _textureId = null;
      await _api.destroyPlayer();
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
    return _api.play();
  }

  Future<void> pause() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.pause();
  }

  Future<void> seek(int ptsUs, {int? requestId}) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.seek(ptsUs, requestId: requestId);
  }

  Future<void> setSpeed(double speed) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.setSpeed(speed);
  }

  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.setLoopRange(enabled: enabled, startUs: startUs, endUs: endUs);
  }

  Future<void> setAudibleTrack(int? fileId) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.setAudibleTrack(fileId);
  }

  Future<void> resize(int width, int height) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.resize(width: width, height: height);
  }

  Future<void> setViewportBackgroundColor(int colorValue) {
    _ensureAlive();
    _viewportBackgroundColor = colorValue;
    if (_textureId == null) return Future.value();
    return _api.setViewportBackgroundColor(colorValue);
  }

  Future<ViewportCapture> captureViewport({String? outputPath}) {
    _ensurePlayer(NativePlayerMethods.captureViewport);
    return _api.captureViewport(outputPath: outputPath);
  }

  Future<void> stepForward() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.stepForward();
  }

  Future<void> stepBackward() {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.stepBackward();
  }

  Future<int> currentPts() {
    if (!_hasPlayerForCommand()) return Future.value(0);
    return _api.currentPts();
  }

  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) {
    if (!_hasPlayerForCommand()) return Future.value(null);
    return _api.currentPresentedFrame(fileId);
  }

  Future<int> duration() {
    if (!_hasPlayerForCommand()) return Future.value(0);
    return _api.duration();
  }

  Future<bool> isPlaying() {
    if (!_hasPlayerForCommand()) return Future.value(false);
    return _api.isPlaying();
  }

  /// Atomically apply layout state and trigger redraw if paused.
  Future<void> applyLayout(LayoutState state) {
    if (!_hasPlayerForCommand()) return Future.value();
    return _api.applyLayout(state);
  }

  /// Get a snapshot of the current layout state.
  Future<LayoutState> getLayout() {
    _ensurePlayer(NativePlayerMethods.getLayout);
    return _api.getLayout();
  }

  /// Add a video track at the first empty slot.
  Future<TrackInfo> addTrack(
    String videoPath, {
    bool useHardwareDecode = true,
  }) {
    _ensurePlayer(NativePlayerMethods.addTrack);
    return _api.addTrack(videoPath, useHardwareDecode: useHardwareDecode);
  }

  /// Remove a track by file_id.
  Future<void> removeTrack(int fileId) {
    _ensurePlayer(NativePlayerMethods.removeTrack);
    return _api.removeTrack(fileId);
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
    return _api.setTrackOffset(fileId: fileId, offsetUs: offsetUs);
  }

  /// Get current track info list.
  Future<List<TrackInfo>> getTracks() {
    if (!_hasPlayerForCommand()) return Future.value(const []);
    return _api.getTracks();
  }

  /// Get diagnostics data (placeholder, requires native counters).
  Future<Map<String, dynamic>> getDiagnostics() {
    if (!_hasPlayerForCommand()) return Future.value(const {});
    return _api.getDiagnostics();
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
      await _api.destroyPlayer();
    }
  }
}
