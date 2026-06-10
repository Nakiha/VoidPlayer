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
  static const captureViewportRegion = 'captureViewportRegion';
  static const captureWindow = 'captureWindow';
  static const stepForward = 'stepForward';
  static const stepBackward = 'stepBackward';
  static const currentPts = 'currentPts';
  static const currentPresentedFrame = 'currentPresentedFrame';
  static const duration = 'duration';
  static const isPlaying = 'isPlaying';
  static const getPlaybackSnapshot = 'getPlaybackSnapshot';
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
  static const x = 'x';
  static const y = 'y';
  static const maxSize = 'maxSize';
  static const textureId = 'textureId';
  static const tracks = 'tracks';
  static const fileId = 'fileId';
  static const slot = 'slot';
  static const path = 'path';
  static const useHardwareDecode = 'useHardwareDecode';
  static const color = 'color';
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
  static const currentPtsUs = 'currentPtsUs';
  static const isPlaying = 'isPlaying';
  static const includePresentedFrames = 'includePresentedFrames';
  static const presentedFrames = 'presentedFrames';
  static const requestId = 'requestId';
  static const targetPtsUs = 'targetPtsUs';
  static const speed = 'speed';
  static const enabled = 'enabled';
  static const startUs = 'startUs';
  static const endUs = 'endUs';
  static const offsetUs = 'offsetUs';
}

class NativePlayerPayloads {
  const NativePlayerPayloads._();

  static Map<dynamic, dynamic> requireMap(
    Map<dynamic, dynamic>? map,
    String method,
  ) {
    if (map == null) {
      throw NativeProtocolException(
        context: method,
        reason: 'expected a map payload',
        payload: null,
      );
    }
    return map;
  }

  static TrackInfo trackInfoFromValue(Object? value, String context) {
    if (value is! Map) {
      throw NativeProtocolException(
        context: context,
        reason: 'expected a track map payload',
        payload: value,
      );
    }
    return TrackInfo.fromMap(Map<dynamic, dynamic>.from(value));
  }

  static T requireField<T>(
    Map<dynamic, dynamic> map,
    String key,
    String context,
  ) {
    final value = map[key];
    if (value is T) return value;
    throw NativeProtocolException(
      context: context,
      reason: 'expected "$key" to be $T',
      payload: map,
    );
  }

  static T requireValue<T>(Object? value, String context) {
    if (value is T) return value;
    throw NativeProtocolException(
      context: context,
      reason: 'expected value to be $T',
      payload: value,
    );
  }

  static T optionalField<T>(
    Map<dynamic, dynamic> map,
    String key,
    T defaultValue,
    String context,
  ) {
    final value = map[key];
    if (value == null) return defaultValue;
    if (value is T) return value;
    throw NativeProtocolException(
      context: context,
      reason: 'expected "$key" to be $T when present',
      payload: map,
    );
  }

  static int optionalInt(
    Map<dynamic, dynamic> map,
    String key,
    int defaultValue,
    String context,
  ) {
    final value = map[key];
    if (value == null) return defaultValue;
    if (value is num) return value.toInt();
    throw NativeProtocolException(
      context: context,
      reason: 'expected "$key" to be numeric when present',
      payload: map,
    );
  }

  static double optionalDouble(
    Map<dynamic, dynamic> map,
    String key,
    double defaultValue,
    String context,
  ) {
    final value = map[key];
    if (value == null) return defaultValue;
    if (value is num) return value.toDouble();
    throw NativeProtocolException(
      context: context,
      reason: 'expected "$key" to be numeric when present',
      payload: map,
    );
  }
}

class NativeProtocolException implements Exception {
  final String context;
  final String reason;
  final Object? payload;

  const NativeProtocolException({
    required this.context,
    required this.reason,
    this.payload,
  });

  @override
  String toString() {
    return 'NativeProtocolException($context): $reason; payload=$payload';
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

class PlaybackSnapshot {
  final int currentPtsUs;
  final int durationUs;
  final bool isPlaying;
  final Map<int, PresentedFrameTiming> presentedFrames;

  const PlaybackSnapshot({
    required this.currentPtsUs,
    required this.durationUs,
    required this.isPlaying,
    this.presentedFrames = const {},
  });

  static const empty = PlaybackSnapshot(
    currentPtsUs: 0,
    durationUs: 0,
    isPlaying: false,
  );

  factory PlaybackSnapshot.fromMap(Map<dynamic, dynamic> map) {
    const context = NativePlayerMethods.getPlaybackSnapshot;
    return PlaybackSnapshot(
      currentPtsUs: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.currentPtsUs,
        context,
      ),
      durationUs: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.durationUs,
        context,
      ),
      isPlaying: NativePlayerPayloads.requireField<bool>(
        map,
        NativePlayerKeys.isPlaying,
        context,
      ),
      presentedFrames: _presentedFramesFromValue(
        map[NativePlayerKeys.presentedFrames],
      ),
    );
  }

  static Map<int, PresentedFrameTiming> _presentedFramesFromValue(
    Object? value,
  ) {
    if (value == null) return const {};
    if (value is! List) {
      throw NativeProtocolException(
        context: NativePlayerMethods.getPlaybackSnapshot,
        reason: 'expected "presentedFrames" to be a list when present',
        payload: value,
      );
    }
    final frames = <int, PresentedFrameTiming>{};
    for (final item in value) {
      if (item is! Map) {
        throw NativeProtocolException(
          context: NativePlayerMethods.getPlaybackSnapshot,
          reason: 'expected presented frame entry to be a map',
          payload: item,
        );
      }
      final map = Map<dynamic, dynamic>.from(item);
      final fileId = NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.fileId,
        NativePlayerMethods.getPlaybackSnapshot,
      );
      final timing = PresentedFrameTiming.fromMap(map);
      if (timing.isValid) {
        frames[fileId] = timing;
      }
    }
    return Map.unmodifiable(frames);
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

  factory TrackInfo.fromMap(Map<dynamic, dynamic> map) {
    const context = 'TrackInfo';
    return TrackInfo(
      fileId: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.fileId,
        context,
      ),
      slot: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.slot,
        context,
      ),
      path: NativePlayerPayloads.requireField<String>(
        map,
        NativePlayerKeys.path,
        context,
      ),
      width: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.width,
        context,
      ),
      height: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.height,
        context,
      ),
      durationUs: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.durationUs,
        0,
        context,
      ),
      startTimeUs: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.startTimeUs,
        0,
        context,
      ),
      bitRate: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.bitRate,
        0,
        context,
      ),
      formatName: NativePlayerPayloads.optionalField<String>(
        map,
        NativePlayerKeys.formatName,
        '',
        context,
      ),
      codecName: NativePlayerPayloads.optionalField<String>(
        map,
        NativePlayerKeys.codecName,
        '',
        context,
      ),
      codecLongName: NativePlayerPayloads.optionalField<String>(
        map,
        NativePlayerKeys.codecLongName,
        '',
        context,
      ),
      decoderName: NativePlayerPayloads.optionalField<String>(
        map,
        NativePlayerKeys.decoderName,
        '',
        context,
      ),
    );
  }
}

/// Result of createPlayer, containing texture ID and initial track info.
class CreatePlayerResult {
  final int textureId;
  final List<TrackInfo> tracks;

  const CreatePlayerResult({required this.textureId, required this.tracks});

  factory CreatePlayerResult.fromMap(Map<dynamic, dynamic> payload) {
    final textureId = NativePlayerPayloads.requireField<int>(
      payload,
      NativePlayerKeys.textureId,
      NativePlayerMethods.createPlayer,
    );
    final tracksValue = payload[NativePlayerKeys.tracks];
    if (tracksValue != null && tracksValue is! List) {
      throw NativeProtocolException(
        context: NativePlayerMethods.createPlayer,
        reason: 'expected "tracks" to be a list when present',
        payload: payload,
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

  factory ViewportCapture.fromMap(Map<dynamic, dynamic> map) {
    const context = 'ViewportCapture';
    return ViewportCapture(
      hash: NativePlayerPayloads.requireField<String>(
        map,
        NativePlayerKeys.hash,
        context,
      ),
      width: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.width,
        context,
      ),
      height: NativePlayerPayloads.requireField<int>(
        map,
        NativePlayerKeys.height,
        context,
      ),
      avgLuma: NativePlayerPayloads.optionalDouble(
        map,
        NativePlayerKeys.avgLuma,
        0.0,
        context,
      ),
      nonBlackRatio: NativePlayerPayloads.optionalDouble(
        map,
        NativePlayerKeys.nonBlackRatio,
        0.0,
        context,
      ),
      regionAvgLuma: _doubleMap(map[NativePlayerKeys.regionAvgLuma]),
      regionNonBlackRatio: _doubleMap(
        map[NativePlayerKeys.regionNonBlackRatio],
      ),
      overlayLineStyleMetricsAvailable:
          map.containsKey(NativePlayerKeys.overlayLinePairedCenters) &&
          map.containsKey(NativePlayerKeys.overlayLineWeakWhiteCenters) &&
          map.containsKey(NativePlayerKeys.overlayLineBlackOnlyCenters),
      overlayLinePairedCenters: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.overlayLinePairedCenters,
        0,
        context,
      ),
      overlayLineWeakWhiteCenters: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.overlayLineWeakWhiteCenters,
        0,
        context,
      ),
      overlayLineBlackOnlyCenters: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.overlayLineBlackOnlyCenters,
        0,
        context,
      ),
      outputPath: NativePlayerPayloads.optionalField<String?>(
        map,
        NativePlayerKeys.outputPath,
        null,
        context,
      ),
    );
  }
}

Map<String, double> _doubleMap(Object? raw) {
  if (raw is! Map) {
    return const {};
  }
  return raw.map((key, value) {
    if (value is! num) {
      throw NativeProtocolException(
        context: 'ViewportCapture',
        reason: 'expected region metric "$key" to be numeric',
        payload: raw,
      );
    }
    return MapEntry('$key', value.toDouble());
  });
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

  factory LayoutState.fromMap(Map<dynamic, dynamic> map) {
    const context = 'LayoutState';
    final orderValue = map[NativePlayerKeys.order];
    if (orderValue != null && orderValue is! List) {
      throw NativeProtocolException(
        context: context,
        reason: 'expected "order" to be a list when present',
        payload: map,
      );
    }
    final orderList = orderValue as List<dynamic>?;
    final List<int> order = orderList == null
        ? const [0, 1, 2, 3]
        : orderList.map<int>((Object? entry) {
            if (entry is! num) {
              throw NativeProtocolException(
                context: context,
                reason: 'expected "order" entries to be numeric',
                payload: map,
              );
            }
            return entry.toInt();
          }).toList();
    return LayoutState(
      mode: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.mode,
        LayoutMode.sideBySide,
        context,
      ),
      splitPos: NativePlayerPayloads.optionalDouble(
        map,
        NativePlayerKeys.splitPos,
        0.5,
        context,
      ),
      zoomRatio: NativePlayerPayloads.optionalDouble(
        map,
        NativePlayerKeys.zoomRatio,
        1.0,
        context,
      ),
      viewOffsetX: NativePlayerPayloads.optionalDouble(
        map,
        NativePlayerKeys.viewOffsetX,
        0.0,
        context,
      ),
      viewOffsetY: NativePlayerPayloads.optionalDouble(
        map,
        NativePlayerKeys.viewOffsetY,
        0.0,
        context,
      ),
      pixelSizeMode: NativePlayerPayloads.optionalInt(
        map,
        NativePlayerKeys.pixelSizeMode,
        LayoutPixelSizeMode.uniformVideoPixels,
        context,
      ),
      order: order,
    );
  }

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
