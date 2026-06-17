import 'app_log.dart';
import 'native_player/native_player_api.dart';
import 'native_player/native_player_events.dart';
import 'native_player/native_player_protocol.dart';

export 'native_player/native_player_api.dart';
export 'native_player/native_player_events.dart';
export 'native_player/native_player_protocol.dart';

class NativePlayerController {
  final NativePlayerApi _api;
  final bool strictCommandOrder;
  final void Function(String method, String reason)? onNoopCommand;
  int? _textureId;
  bool _disposed = false;
  Future<CreatePlayerResult>? _createInFlight;
  Future<void>? _destroyInFlight;
  Future<void>? _disposeFuture;
  int? _viewportBackgroundColor;

  NativePlayerController({
    NativePlayerApi? api,
    this.strictCommandOrder = false,
    this.onNoopCommand,
  }) : _api = api ?? const MethodChannelNativePlayerApi();

  int? get textureId => _textureId;
  bool get isDisposed => _disposed;
  bool get hasPlayer => _textureId != null;
  bool get canAcceptCommands => !_disposed && _textureId != null;
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

  bool _hasPlayerForCommand(String method) {
    if (canAcceptCommands) return true;
    if (strictCommandOrder) {
      _ensurePlayer(method);
    }
    _reportNoopCommand(
      method,
      _disposed ? 'controller disposed' : 'player not created',
    );
    return false;
  }

  void _reportNoopCommand(String method, String reason) {
    logFine('NativePlayerController.$method ignored: $reason');
    onNoopCommand?.call(method, reason);
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
      viewportBackgroundColor: _viewportBackgroundColor,
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
    if (!_hasPlayerForCommand(NativePlayerMethods.play)) return Future.value();
    return _api.play();
  }

  Future<void> pause() {
    if (!_hasPlayerForCommand(NativePlayerMethods.pause)) return Future.value();
    return _api.pause();
  }

  Future<Map<String, dynamic>> debugFlutterSurfaceInfo() {
    _ensureAlive();
    return _api.debugFlutterSurfaceInfo();
  }

  Future<Map<String, dynamic>> debugNativeCompositor() {
    _ensureAlive();
    return _api.debugNativeCompositor();
  }

  Future<void> seek(int ptsUs, {int? requestId}) {
    if (!_hasPlayerForCommand(NativePlayerMethods.seek)) return Future.value();
    return _api.seek(ptsUs, requestId: requestId);
  }

  Future<void> setSpeed(double speed) {
    if (!_hasPlayerForCommand(NativePlayerMethods.setSpeed)) {
      return Future.value();
    }
    return _api.setSpeed(speed);
  }

  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) {
    if (!_hasPlayerForCommand(NativePlayerMethods.setLoopRange)) {
      return Future.value();
    }
    return _api.setLoopRange(enabled: enabled, startUs: startUs, endUs: endUs);
  }

  Future<void> setAudibleTrack(int? fileId) {
    if (!_hasPlayerForCommand(NativePlayerMethods.setAudibleTrack)) {
      return Future.value();
    }
    return _api.setAudibleTrack(fileId);
  }

  Future<void> resize(int width, int height) {
    if (!_hasPlayerForCommand(NativePlayerMethods.resize)) {
      return Future.value();
    }
    return _api.resize(width: width, height: height);
  }

  Future<void> prewarmNativePresentationTargetSize(int width, int height) {
    if (!_hasPlayerForCommand(
      NativePlayerMethods.prewarmNativePresentationTargetSize,
    )) {
      return Future.value();
    }
    return _api.prewarmNativePresentationTargetSize(
      width: width,
      height: height,
    );
  }

  Future<void> setNativeCompositorViewportRect({
    required int left,
    required int top,
    required int width,
    required int height,
    required int surfaceWidth,
    required int surfaceHeight,
  }) {
    _ensureAlive();
    return _api.setNativeCompositorViewportRect(
      left: left,
      top: top,
      width: width,
      height: height,
      surfaceWidth: surfaceWidth,
      surfaceHeight: surfaceHeight,
    );
  }

  Future<void> requestNativeCompositorFlutterFrame({required String reason}) {
    if (!_hasPlayerForCommand(
      NativePlayerMethods.requestNativeCompositorFlutterFrame,
    )) {
      return Future.value();
    }
    return _api.requestNativeCompositorFlutterFrame(reason: reason);
  }

  Future<void> ackNativeCompositorFlutterState({
    required int serial,
    required bool transparentViewport,
  }) {
    _ensureAlive();
    return _api.ackNativeCompositorFlutterState(
      serial: serial,
      transparentViewport: transparentViewport,
    );
  }

  Future<void> debugFailNativeCompositor({
    String reason = 'ui-test-forced-failure',
  }) {
    _ensureAlive();
    return _api.debugFailNativeCompositor(reason: reason);
  }

  Future<void> debugSimulateWindowsDeviceLoss({
    required String target,
    String reason = 'debug-simulated-device-loss',
  }) {
    _ensureAlive();
    return _api.debugSimulateWindowsDeviceLoss(target: target, reason: reason);
  }

  Future<void> resetNativePerfCounters() {
    _ensureAlive();
    return _api.resetNativePerfCounters();
  }

  Future<void> beginNativeInteractionSample({required String label}) {
    _ensureAlive();
    return _api.beginNativeInteractionSample(label: label);
  }

  Future<void> endNativeInteractionSample({required String label}) {
    _ensureAlive();
    return _api.endNativeInteractionSample(label: label);
  }

  Future<void> setNativeCompositorViewportTransform({
    required bool enabled,
    required double scaleX,
    required double scaleY,
    required double translateX,
    required double translateY,
    required int mode,
    required double splitPos,
    required int activeTrackCount,
  }) {
    _ensureAlive();
    return _api.setNativeCompositorViewportTransform(
      enabled: enabled,
      scaleX: scaleX,
      scaleY: scaleY,
      translateX: translateX,
      translateY: translateY,
      mode: mode,
      splitPos: splitPos,
      activeTrackCount: activeTrackCount,
    );
  }

  Future<void> prepareNativeCompositorSourceCache({
    required List<int> sourceSlots,
    required List<int> sourceOrder,
    required int mode,
    required double splitPos,
    required int activeTrackCount,
    required List<double> displayOffsetX,
    required List<double> displayOffsetY,
    required List<double> invDisplaySizeX,
    required List<double> invDisplaySizeY,
    required List<double> viewOffsetUvX,
    required List<double> viewOffsetUvY,
  }) {
    _ensureAlive();
    return _api.prepareNativeCompositorSourceCache(
      sourceSlots: sourceSlots,
      sourceOrder: sourceOrder,
      mode: mode,
      splitPos: splitPos,
      activeTrackCount: activeTrackCount,
      displayOffsetX: displayOffsetX,
      displayOffsetY: displayOffsetY,
      invDisplaySizeX: invDisplaySizeX,
      invDisplaySizeY: invDisplaySizeY,
      viewOffsetUvX: viewOffsetUvX,
      viewOffsetUvY: viewOffsetUvY,
    );
  }

  Future<void> clearNativeCompositorSourceCache({required String reason}) {
    if (_disposed) return Future.value();
    return _api.clearNativeCompositorSourceCache(reason: reason);
  }

  Future<void> setNativeAnalysisOverlay(Map<String, Object?> state) {
    if (_disposed) return Future.value();
    if (!canAcceptCommands) return Future.value();
    return _api.setNativeAnalysisOverlay(state);
  }

  Future<void> setViewportBackgroundColor(int colorValue) {
    if (_disposed) {
      _reportNoopCommand(
        NativePlayerMethods.setViewportBackgroundColor,
        'controller disposed',
      );
      return Future.value();
    }
    _viewportBackgroundColor = colorValue;
    if (!canAcceptCommands) return Future.value();
    return _api.setViewportBackgroundColor(colorValue);
  }

  Future<ViewportCapture> captureViewport({String? outputPath}) {
    _ensurePlayer(NativePlayerMethods.captureViewport);
    return _api.captureViewport(outputPath: outputPath);
  }

  Future<ViewportCapture> captureViewportRegion({
    required int x,
    required int y,
    required int width,
    required int height,
    required int maxSize,
    String? outputPath,
  }) {
    _ensurePlayer(NativePlayerMethods.captureViewportRegion);
    return _api.captureViewportRegion(
      x: x,
      y: y,
      width: width,
      height: height,
      maxSize: maxSize,
      outputPath: outputPath,
    );
  }

  Future<ViewportCapture> captureWindow({String? outputPath}) {
    return _api.captureWindow(outputPath: outputPath);
  }

  Future<void> stepForward() {
    if (!_hasPlayerForCommand(NativePlayerMethods.stepForward)) {
      return Future.value();
    }
    return _api.stepForward();
  }

  Future<void> stepBackward() {
    if (!_hasPlayerForCommand(NativePlayerMethods.stepBackward)) {
      return Future.value();
    }
    return _api.stepBackward();
  }

  Future<int> currentPts() {
    if (!_hasPlayerForCommand(NativePlayerMethods.currentPts)) {
      return Future.value(0);
    }
    return _api.currentPts();
  }

  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) {
    if (!_hasPlayerForCommand(NativePlayerMethods.currentPresentedFrame)) {
      return Future.value(null);
    }
    return _api.currentPresentedFrame(fileId);
  }

  Future<int> duration() {
    if (!_hasPlayerForCommand(NativePlayerMethods.duration)) {
      return Future.value(0);
    }
    return _api.duration();
  }

  Future<bool> isPlaying() {
    if (!_hasPlayerForCommand(NativePlayerMethods.isPlaying)) {
      return Future.value(false);
    }
    return _api.isPlaying();
  }

  Future<PlaybackSnapshot> getPlaybackSnapshot({
    bool includePresentedFrames = false,
  }) {
    if (!_hasPlayerForCommand(NativePlayerMethods.getPlaybackSnapshot)) {
      return Future.value(PlaybackSnapshot.empty);
    }
    return _api.getPlaybackSnapshot(
      includePresentedFrames: includePresentedFrames,
    );
  }

  /// Atomically apply layout state and trigger redraw if paused.
  Future<void> applyLayout(LayoutState state) {
    if (!_hasPlayerForCommand(NativePlayerMethods.applyLayout)) {
      return Future.value();
    }
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
    if (!_hasPlayerForCommand(NativePlayerMethods.getTracks)) {
      return Future.value(const []);
    }
    return _api.getTracks();
  }

  /// Get diagnostics data (placeholder, requires native counters).
  Future<Map<String, dynamic>> getDiagnostics() {
    if (!_hasPlayerForCommand(NativePlayerMethods.getDiagnostics)) {
      return Future.value(const {});
    }
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
      } catch (error, stack) {
        log.fine('createPlayer failed before destroy completed', error, stack);
      }
    }
    final textureId = _textureId;
    _textureId = null;
    if (textureId != null) {
      await _api.destroyPlayer();
    }
  }
}
