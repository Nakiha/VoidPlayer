import 'package:flutter/services.dart';

import '../app_log.dart';
import 'native_player_events.dart';
import 'native_player_protocol.dart';

abstract interface class NativePlayerApi {
  Stream<NativePlayerEvent> get events;

  Future<CreatePlayerResult> createPlayer({
    required List<String> videoPaths,
    required int width,
    required int height,
    required bool useHardwareDecode,
    int? viewportBackgroundColor,
  });

  Future<void> destroyPlayer();
  Future<void> play();
  Future<void> pause();
  Future<void> seek(int ptsUs, {int? requestId});
  Future<void> setSpeed(double speed);
  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  });
  Future<void> setAudibleTrack(int? fileId);
  Future<void> resize({required int width, required int height});
  Future<void> prewarmNativePresentationTargetSize({
    required int width,
    required int height,
  });
  Future<void> setNativeCompositorViewportRect({
    required int left,
    required int top,
    required int width,
    required int height,
    required int surfaceWidth,
    required int surfaceHeight,
  });
  Future<void> requestNativeCompositorFlutterFrame({required String reason});
  Future<void> boostNativeCompositorFlutterInteraction({
    required String reason,
  });
  Future<void> ackNativeCompositorFlutterState({
    required int serial,
    required bool transparentViewport,
  });
  Future<void> debugFailNativeCompositor({required String reason});
  Future<void> debugSimulateWindowsDeviceLoss({
    required String target,
    required String reason,
  });
  Future<void> resetNativePerfCounters();
  Future<void> beginNativeInteractionSample({required String label});
  Future<void> endNativeInteractionSample({required String label});
  Future<void> setNativeCompositorViewportTransform({
    required bool enabled,
    required double scaleX,
    required double scaleY,
    required double translateX,
    required double translateY,
    required int mode,
    required double splitPos,
    required int activeTrackCount,
  });
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
  });
  Future<void> setNativeAnalysisOverlay(Map<String, Object?> state);
  Future<void> clearNativeCompositorSourceCache({required String reason});
  Future<void> setViewportBackgroundColor(int colorValue);
  Future<ViewportCapture> captureViewport({String? outputPath});
  Future<ViewportCapture> captureViewportRegion({
    required int x,
    required int y,
    required int width,
    required int height,
    required int maxSize,
    String? outputPath,
  });
  Future<ViewportCapture> captureWindow({String? outputPath});
  Future<Map<String, dynamic>> debugFlutterSurfaceInfo();
  Future<Map<String, dynamic>> debugNativeCompositor();
  Future<void> stepForward();
  Future<void> stepBackward();
  Future<int> currentPts();
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId);
  Future<int> duration();
  Future<bool> isPlaying();
  Future<PlaybackSnapshot> getPlaybackSnapshot({
    bool includePresentedFrames = false,
  });
  Future<void> applyLayout(LayoutState state);
  Future<LayoutState> getLayout();
  Future<TrackInfo> addTrack(
    String videoPath, {
    required bool useHardwareDecode,
  });
  Future<void> removeTrack(int fileId);
  Future<void> setTrackOffset({required int fileId, required int offsetUs});
  Future<List<TrackInfo>> getTracks();
  Future<Map<String, dynamic>> getDiagnostics();
}

class MethodChannelNativePlayerApi implements NativePlayerApi {
  final MethodChannel _channel;
  final NativePlayerEventStream _eventStream;
  static int _nextCompositorTraceId = 1;

  const MethodChannelNativePlayerApi([
    this._channel = const MethodChannel(NativePlayerChannel.name),
    this._eventStream = const NativePlayerEventStream(),
  ]);

  @override
  Stream<NativePlayerEvent> get events => _eventStream.events;

  static Map<String, dynamic> _withCompositorTrace(Map<String, dynamic> args) {
    args[NativePlayerKeys.traceId] = _nextCompositorTraceId++;
    args[NativePlayerKeys.traceSentUs] = DateTime.now().microsecondsSinceEpoch;
    return args;
  }

  Future<void> _invokeTimedVoid(
    String method, [
    Map<String, dynamic>? args,
  ]) async {
    final stopwatch = Stopwatch()..start();
    try {
      await _channel.invokeMethod<void>(method, args);
    } finally {
      stopwatch.stop();
      final elapsedUs = stopwatch.elapsedMicroseconds;
      if (elapsedUs >= 4000) {
        logFine(
          'NativePlayer MethodChannel profiler method=$method '
          'elapsedMs=${(elapsedUs / 1000.0).toStringAsFixed(2)}',
        );
      }
    }
  }

  @override
  Future<CreatePlayerResult> createPlayer({
    required List<String> videoPaths,
    required int width,
    required int height,
    required bool useHardwareDecode,
    int? viewportBackgroundColor,
  }) async {
    final args = <String, dynamic>{
      NativePlayerKeys.videoPaths: videoPaths,
      NativePlayerKeys.width: width,
      NativePlayerKeys.height: height,
      NativePlayerKeys.useHardwareDecode: useHardwareDecode,
    };
    if (viewportBackgroundColor != null) {
      args[NativePlayerKeys.color] = viewportBackgroundColor;
    }
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.createPlayer,
      args,
    );
    return CreatePlayerResult.fromMap(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.createPlayer),
    );
  }

  @override
  Future<void> destroyPlayer() {
    return _channel.invokeMethod<void>(NativePlayerMethods.destroyPlayer);
  }

  @override
  Future<void> play() {
    return _channel.invokeMethod<void>(NativePlayerMethods.play);
  }

  @override
  Future<void> pause() {
    return _channel.invokeMethod<void>(NativePlayerMethods.pause);
  }

  @override
  Future<void> seek(int ptsUs, {int? requestId}) {
    final args = <String, dynamic>{NativePlayerKeys.ptsUs: ptsUs};
    if (requestId != null) {
      args[NativePlayerKeys.requestId] = requestId;
    }
    return _channel.invokeMethod<void>(NativePlayerMethods.seek, args);
  }

  @override
  Future<void> setSpeed(double speed) {
    return _channel.invokeMethod<void>(NativePlayerMethods.setSpeed, {
      NativePlayerKeys.speed: speed,
    });
  }

  @override
  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) {
    return _channel.invokeMethod<void>(NativePlayerMethods.setLoopRange, {
      NativePlayerKeys.enabled: enabled,
      NativePlayerKeys.startUs: startUs,
      NativePlayerKeys.endUs: endUs,
    });
  }

  @override
  Future<void> setAudibleTrack(int? fileId) {
    return _channel.invokeMethod<void>(NativePlayerMethods.setAudibleTrack, {
      NativePlayerKeys.fileId: fileId ?? -1,
    });
  }

  @override
  Future<void> resize({required int width, required int height}) {
    return _channel.invokeMethod<void>(NativePlayerMethods.resize, {
      NativePlayerKeys.width: width,
      NativePlayerKeys.height: height,
    });
  }

  @override
  Future<void> prewarmNativePresentationTargetSize({
    required int width,
    required int height,
  }) {
    return _invokeTimedVoid(
      NativePlayerMethods.prewarmNativePresentationTargetSize,
      {NativePlayerKeys.width: width, NativePlayerKeys.height: height},
    );
  }

  @override
  Future<void> setNativeCompositorViewportRect({
    required int left,
    required int top,
    required int width,
    required int height,
    required int surfaceWidth,
    required int surfaceHeight,
  }) {
    return _invokeTimedVoid(
      NativePlayerMethods.setNativeCompositorViewportRect,
      _withCompositorTrace({
        NativePlayerKeys.left: left,
        NativePlayerKeys.top: top,
        NativePlayerKeys.width: width,
        NativePlayerKeys.height: height,
        NativePlayerKeys.surfaceWidth: surfaceWidth,
        NativePlayerKeys.surfaceHeight: surfaceHeight,
      }),
    );
  }

  @override
  Future<void> requestNativeCompositorFlutterFrame({required String reason}) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.requestNativeCompositorFlutterFrame,
      {NativePlayerKeys.reason: reason},
    );
  }

  @override
  Future<void> boostNativeCompositorFlutterInteraction({
    required String reason,
  }) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.boostNativeCompositorFlutterInteraction,
      {NativePlayerKeys.reason: reason},
    );
  }

  @override
  Future<void> ackNativeCompositorFlutterState({
    required int serial,
    required bool transparentViewport,
  }) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.ackNativeCompositorFlutterState,
      {
        NativePlayerKeys.serial: serial,
        NativePlayerKeys.transparentViewport: transparentViewport,
      },
    );
  }

  @override
  Future<void> debugFailNativeCompositor({required String reason}) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.debugFailNativeCompositor,
      {NativePlayerKeys.reason: reason},
    );
  }

  @override
  Future<void> debugSimulateWindowsDeviceLoss({
    required String target,
    required String reason,
  }) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.debugSimulateWindowsDeviceLoss,
      {NativePlayerKeys.target: target, NativePlayerKeys.reason: reason},
    );
  }

  @override
  Future<void> resetNativePerfCounters() {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.resetNativePerfCounters,
    );
  }

  @override
  Future<void> beginNativeInteractionSample({required String label}) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.beginNativeInteractionSample,
      {NativePlayerKeys.label: label},
    );
  }

  @override
  Future<void> endNativeInteractionSample({required String label}) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.endNativeInteractionSample,
      {NativePlayerKeys.label: label},
    );
  }

  @override
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
    return _invokeTimedVoid(
      NativePlayerMethods.setNativeCompositorViewportTransform,
      _withCompositorTrace({
        NativePlayerKeys.enabled: enabled,
        NativePlayerKeys.scaleX: scaleX,
        NativePlayerKeys.scaleY: scaleY,
        NativePlayerKeys.translateX: translateX,
        NativePlayerKeys.translateY: translateY,
        NativePlayerKeys.mode: mode,
        NativePlayerKeys.splitPos: splitPos,
        NativePlayerKeys.activeTrackCount: activeTrackCount,
      }),
    );
  }

  @override
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
    return _invokeTimedVoid(
      NativePlayerMethods.prepareNativeCompositorSourceCache,
      _withCompositorTrace({
        NativePlayerKeys.sourceSlots: sourceSlots,
        NativePlayerKeys.sourceOrder: sourceOrder,
        NativePlayerKeys.mode: mode,
        NativePlayerKeys.splitPos: splitPos,
        NativePlayerKeys.activeTrackCount: activeTrackCount,
        NativePlayerKeys.displayOffsetX: displayOffsetX,
        NativePlayerKeys.displayOffsetY: displayOffsetY,
        NativePlayerKeys.invDisplaySizeX: invDisplaySizeX,
        NativePlayerKeys.invDisplaySizeY: invDisplaySizeY,
        NativePlayerKeys.viewOffsetUvX: viewOffsetUvX,
        NativePlayerKeys.viewOffsetUvY: viewOffsetUvY,
      }),
    );
  }

  @override
  Future<void> clearNativeCompositorSourceCache({required String reason}) {
    return _invokeTimedVoid(
      NativePlayerMethods.clearNativeCompositorSourceCache,
      _withCompositorTrace({NativePlayerKeys.reason: reason}),
    );
  }

  @override
  Future<void> setNativeAnalysisOverlay(Map<String, Object?> state) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.setNativeAnalysisOverlay,
      state,
    );
  }

  @override
  Future<void> setViewportBackgroundColor(int colorValue) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.setViewportBackgroundColor,
      {NativePlayerKeys.color: colorValue},
    );
  }

  @override
  Future<ViewportCapture> captureViewport({String? outputPath}) async {
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

  @override
  Future<ViewportCapture> captureViewportRegion({
    required int x,
    required int y,
    required int width,
    required int height,
    required int maxSize,
    String? outputPath,
  }) async {
    final args = <String, dynamic>{
      NativePlayerKeys.x: x,
      NativePlayerKeys.y: y,
      NativePlayerKeys.width: width,
      NativePlayerKeys.height: height,
      NativePlayerKeys.maxSize: maxSize,
    };
    if (outputPath != null) {
      args[NativePlayerKeys.outputPath] = outputPath;
    }
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.captureViewportRegion,
      args,
    );
    return ViewportCapture.fromMap(
      NativePlayerPayloads.requireMap(
        map,
        NativePlayerMethods.captureViewportRegion,
      ),
    );
  }

  @override
  Future<ViewportCapture> captureWindow({String? outputPath}) async {
    final args = <String, dynamic>{};
    if (outputPath != null) {
      args[NativePlayerKeys.outputPath] = outputPath;
    }
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.captureWindow,
      args,
    );
    return ViewportCapture.fromMap(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.captureWindow),
    );
  }

  @override
  Future<Map<String, dynamic>> debugFlutterSurfaceInfo() async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.debugFlutterSurfaceInfo,
    );
    return Map<String, dynamic>.from(
      NativePlayerPayloads.requireMap(
        map,
        NativePlayerMethods.debugFlutterSurfaceInfo,
      ),
    );
  }

  @override
  Future<Map<String, dynamic>> debugNativeCompositor() async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.debugNativeCompositor,
    );
    return Map<String, dynamic>.from(
      NativePlayerPayloads.requireMap(
        map,
        NativePlayerMethods.debugNativeCompositor,
      ),
    );
  }

  @override
  Future<void> stepForward() {
    return _channel.invokeMethod<void>(NativePlayerMethods.stepForward);
  }

  @override
  Future<void> stepBackward() {
    return _channel.invokeMethod<void>(NativePlayerMethods.stepBackward);
  }

  @override
  Future<int> currentPts() async {
    final value = await _channel.invokeMethod<Object?>(
      NativePlayerMethods.currentPts,
    );
    return NativePlayerPayloads.requireValue<int>(
      value,
      NativePlayerMethods.currentPts,
    );
  }

  @override
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.currentPresentedFrame,
      {NativePlayerKeys.fileId: fileId},
    );
    if (map == null) return null;
    final timing = PresentedFrameTiming.fromMap(
      map,
      context: NativePlayerMethods.currentPresentedFrame,
    );
    return timing.isValid ? timing : null;
  }

  @override
  Future<int> duration() async {
    final value = await _channel.invokeMethod<Object?>(
      NativePlayerMethods.duration,
    );
    return NativePlayerPayloads.requireValue<int>(
      value,
      NativePlayerMethods.duration,
    );
  }

  @override
  Future<bool> isPlaying() async {
    final value = await _channel.invokeMethod<Object?>(
      NativePlayerMethods.isPlaying,
    );
    return NativePlayerPayloads.requireValue<bool>(
      value,
      NativePlayerMethods.isPlaying,
    );
  }

  @override
  Future<PlaybackSnapshot> getPlaybackSnapshot({
    bool includePresentedFrames = false,
  }) async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.getPlaybackSnapshot,
      {NativePlayerKeys.includePresentedFrames: includePresentedFrames},
    );
    return PlaybackSnapshot.fromMap(
      NativePlayerPayloads.requireMap(
        map,
        NativePlayerMethods.getPlaybackSnapshot,
      ),
    );
  }

  @override
  Future<void> applyLayout(LayoutState state) {
    return _channel.invokeMethod<void>(
      NativePlayerMethods.applyLayout,
      state.toMap(),
    );
  }

  @override
  Future<LayoutState> getLayout() async {
    final map = await _channel.invokeMethod<Object?>(
      NativePlayerMethods.getLayout,
    );
    return LayoutState.fromMap(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.getLayout),
    );
  }

  @override
  Future<TrackInfo> addTrack(
    String videoPath, {
    required bool useHardwareDecode,
  }) async {
    final map = await _channel
        .invokeMethod<Map<dynamic, dynamic>>(NativePlayerMethods.addTrack, {
          NativePlayerKeys.path: videoPath,
          NativePlayerKeys.useHardwareDecode: useHardwareDecode,
        });
    return NativePlayerPayloads.trackInfoFromValue(
      NativePlayerPayloads.requireMap(map, NativePlayerMethods.addTrack),
      NativePlayerMethods.addTrack,
    );
  }

  @override
  Future<void> removeTrack(int fileId) {
    return _channel.invokeMethod<void>(NativePlayerMethods.removeTrack, {
      NativePlayerKeys.fileId: fileId,
    });
  }

  @override
  Future<void> setTrackOffset({required int fileId, required int offsetUs}) {
    return _channel.invokeMethod<void>(NativePlayerMethods.setTrackOffset, {
      NativePlayerKeys.fileId: fileId,
      NativePlayerKeys.offsetUs: offsetUs,
    });
  }

  @override
  Future<List<TrackInfo>> getTracks() async {
    final list = await _channel.invokeMethod<Object?>(
      NativePlayerMethods.getTracks,
    );
    return NativePlayerPayloads.requireList(list, NativePlayerMethods.getTracks)
        .map(
          (e) => NativePlayerPayloads.trackInfoFromValue(
            e,
            NativePlayerMethods.getTracks,
          ),
        )
        .toList();
  }

  @override
  Future<Map<String, dynamic>> getDiagnostics() async {
    final map = await _channel.invokeMethod<Object?>(
      NativePlayerMethods.getDiagnostics,
    );
    final payload = NativePlayerPayloads.requireMap(
      map,
      NativePlayerMethods.getDiagnostics,
    );
    try {
      return Map<String, dynamic>.from(payload);
    } catch (_) {
      throw NativeProtocolException(
        context: NativePlayerMethods.getDiagnostics,
        reason: 'expected diagnostics keys to be strings',
        payload: payload,
      );
    }
  }
}
