import 'package:flutter/services.dart';

import 'native_player_protocol.dart';

abstract interface class NativePlayerApi {
  Future<CreatePlayerResult> createPlayer({
    required List<String> videoPaths,
    required int width,
    required int height,
    required bool useHardwareDecode,
  });

  Future<void> destroyPlayer();
  Future<void> play();
  Future<void> pause();
  Future<void> seek(int ptsUs);
  Future<void> setSpeed(double speed);
  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  });
  Future<void> setAudibleTrack(int? fileId);
  Future<void> resize({required int width, required int height});
  Future<void> setViewportBackgroundColor(int colorValue);
  Future<ViewportCapture> captureViewport({String? outputPath});
  Future<void> stepForward();
  Future<void> stepBackward();
  Future<int> currentPts();
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId);
  Future<int> duration();
  Future<bool> isPlaying();
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

  const MethodChannelNativePlayerApi([
    this._channel = const MethodChannel(NativePlayerChannel.name),
  ]);

  @override
  Future<CreatePlayerResult> createPlayer({
    required List<String> videoPaths,
    required int width,
    required int height,
    required bool useHardwareDecode,
  }) async {
    final map = await _channel
        .invokeMethod<Map<dynamic, dynamic>>(NativePlayerMethods.createPlayer, {
          NativePlayerKeys.videoPaths: videoPaths,
          NativePlayerKeys.width: width,
          NativePlayerKeys.height: height,
          NativePlayerKeys.useHardwareDecode: useHardwareDecode,
        });
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
  Future<void> seek(int ptsUs) {
    return _channel.invokeMethod<void>(NativePlayerMethods.seek, {
      NativePlayerKeys.ptsUs: ptsUs,
    });
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
  Future<void> stepForward() {
    return _channel.invokeMethod<void>(NativePlayerMethods.stepForward);
  }

  @override
  Future<void> stepBackward() {
    return _channel.invokeMethod<void>(NativePlayerMethods.stepBackward);
  }

  @override
  Future<int> currentPts() async {
    return await _channel.invokeMethod<int>(NativePlayerMethods.currentPts) ??
        0;
  }

  @override
  Future<PresentedFrameTiming?> currentPresentedFrame(int fileId) async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.currentPresentedFrame,
      {NativePlayerKeys.fileId: fileId},
    );
    if (map == null) return null;
    final timing = PresentedFrameTiming.fromMap(map);
    return timing.isValid ? timing : null;
  }

  @override
  Future<int> duration() async {
    return await _channel.invokeMethod<int>(NativePlayerMethods.duration) ?? 0;
  }

  @override
  Future<bool> isPlaying() async {
    return await _channel.invokeMethod<bool>(NativePlayerMethods.isPlaying) ??
        false;
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
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.getLayout,
    );
    return LayoutState.fromMap(map ?? {});
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

  @override
  Future<Map<String, dynamic>> getDiagnostics() async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      NativePlayerMethods.getDiagnostics,
    );
    return Map<String, dynamic>.from(map ?? {});
  }
}
