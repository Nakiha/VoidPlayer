import 'package:flutter/foundation.dart';

import '../preferences/playback_preferences.dart';

class NativePlayerChannel {
  static const name = 'video_renderer';
  static const eventsName = 'video_renderer/events';
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
  static const currentPresentedFrame = 'currentPresentedFrame';
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
  static const startTimeUs = 'startTimeUs';
  static const bitRate = 'bitRate';
  static const formatName = 'formatName';
  static const codecName = 'codecName';
  static const codecLongName = 'codecLongName';
  static const decoderName = 'decoderName';
  static const hash = 'hash';
  static const avgLuma = 'avgLuma';
  static const nonBlackRatio = 'nonBlackRatio';
  static const regionAvgLuma = 'regionAvgLuma';
  static const regionNonBlackRatio = 'regionNonBlackRatio';
  static const overlayLinePairedCenters = 'overlayLinePairedCenters';
  static const overlayLineWeakWhiteCenters = 'overlayLineWeakWhiteCenters';
  static const overlayLineBlackOnlyCenters = 'overlayLineBlackOnlyCenters';
  static const outputPath = 'outputPath';
  static const mode = 'mode';
  static const splitPos = 'splitPos';
  static const zoomRatio = 'zoomRatio';
  static const viewOffsetX = 'viewOffsetX';
  static const viewOffsetY = 'viewOffsetY';
  static const pixelSizeMode = 'pixelSizeMode';
  static const order = 'order';
  static const ptsUs = 'ptsUs';
  static const dtsUs = 'dtsUs';
  static const analysisFrameIndex = 'analysisFrameIndex';
  static const frameIdentityMode = 'frameIdentityMode';
  static const sourcePacketIndex = 'sourcePacketIndex';
  static const sourcePacketSize = 'sourcePacketSize';
  static const sourcePacketPos = 'sourcePacketPos';
  static const sourcePacketPtsUs = 'sourcePacketPtsUs';
  static const sourcePacketDtsUs = 'sourcePacketDtsUs';
  static const requestId = 'requestId';
  static const targetPtsUs = 'targetPtsUs';
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

class PresentedFrameTiming {
  static const noTimestampUs = -9223372036854775808;

  final int ptsUs;
  final int dtsUs;
  final int durationUs;
  final int analysisFrameIndex;
  final int frameIdentityMode;
  final int sourcePacketIndex;
  final int sourcePacketSize;
  final int sourcePacketPos;
  final int sourcePacketPtsUs;
  final int sourcePacketDtsUs;

  const PresentedFrameTiming({
    required this.ptsUs,
    required this.dtsUs,
    this.durationUs = 0,
    this.analysisFrameIndex = -1,
    this.frameIdentityMode = 0,
    this.sourcePacketIndex = -1,
    this.sourcePacketSize = 0,
    this.sourcePacketPos = -1,
    this.sourcePacketPtsUs = noTimestampUs,
    this.sourcePacketDtsUs = noTimestampUs,
  });

  factory PresentedFrameTiming.fromMap(Map<dynamic, dynamic> map) {
    return PresentedFrameTiming(
      ptsUs: map[NativePlayerKeys.ptsUs] as int? ?? -1,
      dtsUs: map[NativePlayerKeys.dtsUs] as int? ?? -1,
      durationUs: map[NativePlayerKeys.durationUs] as int? ?? 0,
      analysisFrameIndex:
          map[NativePlayerKeys.analysisFrameIndex] as int? ?? -1,
      frameIdentityMode: map[NativePlayerKeys.frameIdentityMode] as int? ?? 0,
      sourcePacketIndex: map[NativePlayerKeys.sourcePacketIndex] as int? ?? -1,
      sourcePacketSize: map[NativePlayerKeys.sourcePacketSize] as int? ?? 0,
      sourcePacketPos: map[NativePlayerKeys.sourcePacketPos] as int? ?? -1,
      sourcePacketPtsUs:
          map[NativePlayerKeys.sourcePacketPtsUs] as int? ?? noTimestampUs,
      sourcePacketDtsUs:
          map[NativePlayerKeys.sourcePacketDtsUs] as int? ?? noTimestampUs,
    );
  }

  bool get hasStableSourceIdentity =>
      analysisFrameIndex >= 0 || sourcePacketPos >= 0 || sourcePacketIndex >= 0;

  bool get isValid => ptsUs >= 0 || hasStableSourceIdentity;
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
  final int startTimeUs;
  final int bitRate;
  final String formatName;
  final String codecName;
  final String codecLongName;
  final String decoderName;

  const TrackInfo({
    required this.fileId,
    required this.slot,
    required this.path,
    required this.width,
    required this.height,
    this.durationUs = 0,
    this.startTimeUs = 0,
    this.bitRate = 0,
    this.formatName = '',
    this.codecName = '',
    this.codecLongName = '',
    this.decoderName = '',
  });

  factory TrackInfo.fromMap(Map<dynamic, dynamic> map) => TrackInfo(
    fileId: map[NativePlayerKeys.fileId] as int,
    slot: map[NativePlayerKeys.slot] as int,
    path: map[NativePlayerKeys.path] as String,
    width: map[NativePlayerKeys.width] as int,
    height: map[NativePlayerKeys.height] as int,
    durationUs: map[NativePlayerKeys.durationUs] as int? ?? 0,
    startTimeUs: map[NativePlayerKeys.startTimeUs] as int? ?? 0,
    bitRate: map[NativePlayerKeys.bitRate] as int? ?? 0,
    formatName: map[NativePlayerKeys.formatName] as String? ?? '',
    codecName: map[NativePlayerKeys.codecName] as String? ?? '',
    codecLongName: map[NativePlayerKeys.codecLongName] as String? ?? '',
    decoderName: map[NativePlayerKeys.decoderName] as String? ?? '',
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
  final Map<String, double> regionAvgLuma;
  final Map<String, double> regionNonBlackRatio;
  final bool overlayLineStyleMetricsAvailable;
  final int overlayLinePairedCenters;
  final int overlayLineWeakWhiteCenters;
  final int overlayLineBlackOnlyCenters;
  final String? outputPath;

  const ViewportCapture({
    required this.hash,
    required this.width,
    required this.height,
    required this.avgLuma,
    required this.nonBlackRatio,
    this.regionAvgLuma = const {},
    this.regionNonBlackRatio = const {},
    this.overlayLineStyleMetricsAvailable = false,
    this.overlayLinePairedCenters = 0,
    this.overlayLineWeakWhiteCenters = 0,
    this.overlayLineBlackOnlyCenters = 0,
    this.outputPath,
  });

  factory ViewportCapture.fromMap(Map<dynamic, dynamic> map) => ViewportCapture(
    hash: map[NativePlayerKeys.hash] as String,
    width: map[NativePlayerKeys.width] as int,
    height: map[NativePlayerKeys.height] as int,
    avgLuma: (map[NativePlayerKeys.avgLuma] as num?)?.toDouble() ?? 0.0,
    nonBlackRatio:
        (map[NativePlayerKeys.nonBlackRatio] as num?)?.toDouble() ?? 0.0,
    regionAvgLuma: _doubleMap(map[NativePlayerKeys.regionAvgLuma]),
    regionNonBlackRatio: _doubleMap(map[NativePlayerKeys.regionNonBlackRatio]),
    overlayLineStyleMetricsAvailable:
        map.containsKey(NativePlayerKeys.overlayLinePairedCenters) &&
        map.containsKey(NativePlayerKeys.overlayLineWeakWhiteCenters) &&
        map.containsKey(NativePlayerKeys.overlayLineBlackOnlyCenters),
    overlayLinePairedCenters:
        (map[NativePlayerKeys.overlayLinePairedCenters] as num?)?.toInt() ?? 0,
    overlayLineWeakWhiteCenters:
        (map[NativePlayerKeys.overlayLineWeakWhiteCenters] as num?)?.toInt() ??
        0,
    overlayLineBlackOnlyCenters:
        (map[NativePlayerKeys.overlayLineBlackOnlyCenters] as num?)?.toInt() ??
        0,
    outputPath: map[NativePlayerKeys.outputPath] as String?,
  );
}

Map<String, double> _doubleMap(Object? raw) {
  if (raw is! Map) {
    return const {};
  }
  return raw.map(
    (key, value) => MapEntry('$key', (value as num?)?.toDouble() ?? 0.0),
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
