import 'package:flutter/foundation.dart';

import '../preferences/playback_preferences.dart';

class NativePlayerChannel {
  static const name = 'video_renderer';
}

class NativePlayerMethods {
  static const createPlayer = 'createPlayer';
  static const destroyPlayer = 'destroyPlayer';
  static const play = 'play';
  static const pause = 'pause';
  static const seek = 'seek';
  static const setSpeed = 'setSpeed';
  static const setLoopRange = 'setLoopRange';
  static const setAudibleTrack = 'setAudibleTrack';
  static const resize = 'resize';
  static const setViewportBackgroundColor = 'setViewportBackgroundColor';
  static const captureViewport = 'captureViewport';
  static const stepForward = 'stepForward';
  static const stepBackward = 'stepBackward';
  static const currentPts = 'currentPts';
  static const duration = 'duration';
  static const isPlaying = 'isPlaying';
  static const applyLayout = 'applyLayout';
  static const getLayout = 'getLayout';
  static const addTrack = 'addTrack';
  static const removeTrack = 'removeTrack';
  static const setTrackOffset = 'setTrackOffset';
  static const getTracks = 'getTracks';
  static const getDiagnostics = 'getDiagnostics';
}

class NativePlayerKeys {
  static const videoPaths = 'videoPaths';
  static const width = 'width';
  static const height = 'height';
  static const textureId = 'textureId';
  static const tracks = 'tracks';
  static const fileId = 'fileId';
  static const slot = 'slot';
  static const path = 'path';
  static const useHardwareDecode = 'useHardwareDecode';
  static const durationUs = 'durationUs';
  static const hash = 'hash';
  static const avgLuma = 'avgLuma';
  static const nonBlackRatio = 'nonBlackRatio';
  static const outputPath = 'outputPath';
  static const mode = 'mode';
  static const splitPos = 'splitPos';
  static const zoomRatio = 'zoomRatio';
  static const viewOffsetX = 'viewOffsetX';
  static const viewOffsetY = 'viewOffsetY';
  static const pixelSizeMode = 'pixelSizeMode';
  static const order = 'order';
  static const ptsUs = 'ptsUs';
  static const speed = 'speed';
  static const enabled = 'enabled';
  static const startUs = 'startUs';
  static const endUs = 'endUs';
  static const color = 'color';
  static const offsetUs = 'offsetUs';
}

class NativePlayerPayloads {
  const NativePlayerPayloads._();

  static Map<dynamic, dynamic> requireMap(
    Map<dynamic, dynamic>? map,
    String method,
  ) {
    if (map == null) {
      throw StateError('$method returned invalid payload: null');
    }
    return map;
  }

  static TrackInfo trackInfoFromValue(Object? value, String context) {
    if (value is! Map) {
      throw StateError('$context returned invalid track payload: $value');
    }
    return TrackInfo.fromMap(Map<dynamic, dynamic>.from(value));
  }
}

/// Layout mode constants matching native defines.
class LayoutMode {
  static const int sideBySide = 0;
  static const int splitScreen = 1;
}

class LayoutPixelSizeMode {
  static const int uniformVideoPixels = 0;
  static const int fillView = 1;
}

/// Track metadata returned from native layer.
class TrackInfo {
  final int fileId;
  final int slot;
  final String path;
  final int width;
  final int height;
  final int durationUs;

  const TrackInfo({
    required this.fileId,
    required this.slot,
    required this.path,
    required this.width,
    required this.height,
    this.durationUs = 0,
  });

  factory TrackInfo.fromMap(Map<dynamic, dynamic> map) => TrackInfo(
    fileId: map[NativePlayerKeys.fileId] as int,
    slot: map[NativePlayerKeys.slot] as int,
    path: map[NativePlayerKeys.path] as String,
    width: map[NativePlayerKeys.width] as int,
    height: map[NativePlayerKeys.height] as int,
    durationUs: map[NativePlayerKeys.durationUs] as int? ?? 0,
  );
}

/// Result of createPlayer, containing texture ID and initial track info.
class CreatePlayerResult {
  final int textureId;
  final List<TrackInfo> tracks;

  const CreatePlayerResult({required this.textureId, required this.tracks});

  factory CreatePlayerResult.fromMap(Map<dynamic, dynamic> payload) {
    final textureId = payload[NativePlayerKeys.textureId];
    if (textureId is! int) {
      throw StateError(
        'createPlayer returned invalid textureId: '
        '${payload[NativePlayerKeys.textureId]}',
      );
    }
    final tracksValue = payload[NativePlayerKeys.tracks];
    if (tracksValue != null && tracksValue is! List) {
      throw StateError(
        'createPlayer returned invalid tracks payload: $tracksValue',
      );
    }
    final tracksList = tracksValue as List<dynamic>? ?? [];
    return CreatePlayerResult(
      textureId: textureId,
      tracks: tracksList
          .map(
            (e) => NativePlayerPayloads.trackInfoFromValue(e, 'createPlayer'),
          )
          .toList(),
    );
  }
}

class ViewportCapture {
  final String hash;
  final int width;
  final int height;
  final double avgLuma;
  final double nonBlackRatio;
  final String? outputPath;

  const ViewportCapture({
    required this.hash,
    required this.width,
    required this.height,
    required this.avgLuma,
    required this.nonBlackRatio,
    this.outputPath,
  });

  factory ViewportCapture.fromMap(Map<dynamic, dynamic> map) => ViewportCapture(
    hash: map[NativePlayerKeys.hash] as String,
    width: map[NativePlayerKeys.width] as int,
    height: map[NativePlayerKeys.height] as int,
    avgLuma: (map[NativePlayerKeys.avgLuma] as num?)?.toDouble() ?? 0.0,
    nonBlackRatio:
        (map[NativePlayerKeys.nonBlackRatio] as num?)?.toDouble() ?? 0.0,
    outputPath: map[NativePlayerKeys.outputPath] as String?,
  );
}

/// Immutable snapshot of the layout state.
class LayoutState {
  static const double zoomMin = 1.0;
  static const double zoomMax = 50.0;

  final int mode;
  final double splitPos;
  final double zoomRatio;
  final double viewOffsetX;
  final double viewOffsetY;
  final int pixelSizeMode;
  final List<int> order;

  const LayoutState({
    this.mode = LayoutMode.sideBySide,
    this.splitPos = 0.5,
    this.zoomRatio = 1.0,
    this.viewOffsetX = 0.0,
    this.viewOffsetY = 0.0,
    this.pixelSizeMode = LayoutPixelSizeMode.uniformVideoPixels,
    this.order = const [0, 1, 2, 3],
  });

  factory LayoutState.fromPlaybackPreferences(
    PlaybackPreferences preferences,
  ) =>
      LayoutState(pixelSizeMode: preferences.viewportPixelSizeMode.layoutValue);

  Map<String, dynamic> toMap() => {
    NativePlayerKeys.mode: mode,
    NativePlayerKeys.splitPos: splitPos,
    NativePlayerKeys.zoomRatio: zoomRatio,
    NativePlayerKeys.viewOffsetX: viewOffsetX,
    NativePlayerKeys.viewOffsetY: viewOffsetY,
    NativePlayerKeys.pixelSizeMode: pixelSizeMode,
    NativePlayerKeys.order: order,
  };

  factory LayoutState.fromMap(Map<dynamic, dynamic> map) => LayoutState(
    mode:
        (map[NativePlayerKeys.mode] as num?)?.toInt() ?? LayoutMode.sideBySide,
    splitPos: (map[NativePlayerKeys.splitPos] as num?)?.toDouble() ?? 0.5,
    zoomRatio: (map[NativePlayerKeys.zoomRatio] as num?)?.toDouble() ?? 1.0,
    viewOffsetX: (map[NativePlayerKeys.viewOffsetX] as num?)?.toDouble() ?? 0.0,
    viewOffsetY: (map[NativePlayerKeys.viewOffsetY] as num?)?.toDouble() ?? 0.0,
    pixelSizeMode:
        (map[NativePlayerKeys.pixelSizeMode] as num?)?.toInt() ??
        LayoutPixelSizeMode.uniformVideoPixels,
    order:
        (map[NativePlayerKeys.order] as List<dynamic>?)
            ?.map((e) => (e as num).toInt())
            .toList() ??
        const [0, 1, 2, 3],
  );

  LayoutState copyWith({
    int? mode,
    double? splitPos,
    double? zoomRatio,
    double? viewOffsetX,
    double? viewOffsetY,
    int? pixelSizeMode,
    List<int>? order,
  }) => LayoutState(
    mode: mode ?? this.mode,
    splitPos: splitPos ?? this.splitPos,
    zoomRatio: zoomRatio ?? this.zoomRatio,
    viewOffsetX: viewOffsetX ?? this.viewOffsetX,
    viewOffsetY: viewOffsetY ?? this.viewOffsetY,
    pixelSizeMode: pixelSizeMode ?? this.pixelSizeMode,
    order: order ?? this.order,
  );

  @override
  bool operator ==(Object other) {
    return other is LayoutState &&
        mode == other.mode &&
        splitPos == other.splitPos &&
        zoomRatio == other.zoomRatio &&
        viewOffsetX == other.viewOffsetX &&
        viewOffsetY == other.viewOffsetY &&
        pixelSizeMode == other.pixelSizeMode &&
        listEquals(order, other.order);
  }

  @override
  int get hashCode => Object.hash(
    mode,
    splitPos,
    zoomRatio,
    viewOffsetX,
    viewOffsetY,
    pixelSizeMode,
    Object.hashAll(order),
  );
}
