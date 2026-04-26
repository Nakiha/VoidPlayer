import 'package:flutter/services.dart';

/// Layout mode constants matching native defines.
class LayoutMode {
  static const int sideBySide = 0;
  static const int splitScreen = 1;
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
    fileId: map['fileId'] as int,
    slot: map['slot'] as int,
    path: map['path'] as String,
    width: map['width'] as int,
    height: map['height'] as int,
    durationUs: map['durationUs'] as int? ?? 0,
  );
}

/// Result of createRenderer, containing texture ID and initial track info.
class CreateRendererResult {
  final int textureId;
  final List<TrackInfo> tracks;

  const CreateRendererResult({required this.textureId, required this.tracks});
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
    hash: map['hash'] as String,
    width: map['width'] as int,
    height: map['height'] as int,
    avgLuma: (map['avgLuma'] as num?)?.toDouble() ?? 0.0,
    nonBlackRatio: (map['nonBlackRatio'] as num?)?.toDouble() ?? 0.0,
    outputPath: map['outputPath'] as String?,
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
  final List<int> order;

  const LayoutState({
    this.mode = LayoutMode.sideBySide,
    this.splitPos = 0.5,
    this.zoomRatio = 1.0,
    this.viewOffsetX = 0.0,
    this.viewOffsetY = 0.0,
    this.order = const [0, 1, 2, 3],
  });

  Map<String, dynamic> toMap() => {
    'mode': mode,
    'splitPos': splitPos,
    'zoomRatio': zoomRatio,
    'viewOffsetX': viewOffsetX,
    'viewOffsetY': viewOffsetY,
    'order': order,
  };

  factory LayoutState.fromMap(Map<dynamic, dynamic> map) => LayoutState(
    mode: map['mode'] as int? ?? LayoutMode.sideBySide,
    splitPos: (map['splitPos'] as double?) ?? 0.5,
    zoomRatio: (map['zoomRatio'] as double?) ?? 1.0,
    viewOffsetX: (map['viewOffsetX'] as double?) ?? 0.0,
    viewOffsetY: (map['viewOffsetY'] as double?) ?? 0.0,
    order:
        (map['order'] as List<dynamic>?)?.map((e) => e as int).toList() ??
        const [0, 1, 2, 3],
  );

  LayoutState copyWith({
    int? mode,
    double? splitPos,
    double? zoomRatio,
    double? viewOffsetX,
    double? viewOffsetY,
    List<int>? order,
  }) => LayoutState(
    mode: mode ?? this.mode,
    splitPos: splitPos ?? this.splitPos,
    zoomRatio: zoomRatio ?? this.zoomRatio,
    viewOffsetX: viewOffsetX ?? this.viewOffsetX,
    viewOffsetY: viewOffsetY ?? this.viewOffsetY,
    order: order ?? this.order,
  );
}

class VideoRendererController {
  static const MethodChannel _channel = MethodChannel('video_renderer');

  int? _textureId;
  int? get textureId => _textureId;

  Future<CreateRendererResult> createRenderer(
    List<String> videoPaths, {
    int width = 1920,
    int height = 1080,
  }) async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      'createRenderer',
      {'videoPaths': videoPaths, 'width': width, 'height': height},
    );
    _textureId = map?['textureId'] as int?;
    final tracksList = map?['tracks'] as List<dynamic>? ?? [];
    return CreateRendererResult(
      textureId: _textureId!,
      tracks: tracksList
          .map((e) => TrackInfo.fromMap(e as Map<dynamic, dynamic>))
          .toList(),
    );
  }

  Future<void> play() => _channel.invokeMethod<void>('play');

  Future<void> pause() => _channel.invokeMethod<void>('pause');

  Future<void> seek(int ptsUs) =>
      _channel.invokeMethod<void>('seek', {'ptsUs': ptsUs});

  Future<void> setSpeed(double speed) =>
      _channel.invokeMethod<void>('setSpeed', {'speed': speed});

  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) => _channel.invokeMethod<void>('setLoopRange', {
    'enabled': enabled,
    'startUs': startUs,
    'endUs': endUs,
  });

  Future<void> resize(int width, int height) =>
      _channel.invokeMethod<void>('resize', {'width': width, 'height': height});

  Future<ViewportCapture> captureViewport({String? outputPath}) async {
    final args = <String, dynamic>{};
    if (outputPath != null) {
      args['outputPath'] = outputPath;
    }
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      'captureViewport',
      args,
    );
    return ViewportCapture.fromMap(map!);
  }

  Future<void> stepForward() => _channel.invokeMethod<void>('stepForward');

  Future<void> stepBackward() => _channel.invokeMethod<void>('stepBackward');

  Future<int> currentPts() async {
    return await _channel.invokeMethod<int>('currentPts') ?? 0;
  }

  Future<int> duration() async {
    return await _channel.invokeMethod<int>('duration') ?? 0;
  }

  Future<bool> isPlaying() async {
    return await _channel.invokeMethod<bool>('isPlaying') ?? false;
  }

  /// Atomically apply layout state and trigger redraw if paused.
  Future<void> applyLayout(LayoutState state) =>
      _channel.invokeMethod<void>('applyLayout', state.toMap());

  /// Get a snapshot of the current layout state.
  Future<LayoutState> getLayout() async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>('getLayout');
    return LayoutState.fromMap(map ?? {});
  }

  /// Add a video track at the first empty slot.
  Future<TrackInfo> addTrack(String videoPath) async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>('addTrack', {
      'path': videoPath,
    });
    return TrackInfo.fromMap(map!);
  }

  /// Remove a track by file_id.
  Future<void> removeTrack(int fileId) =>
      _channel.invokeMethod<void>('removeTrack', {'fileId': fileId});

  /// Set per-track sync offset in microseconds.
  Future<void> setTrackOffset({required int fileId, required int offsetUs}) =>
      _channel.invokeMethod<void>('setTrackOffset', {
        'fileId': fileId,
        'offsetUs': offsetUs,
      });

  /// Get current track info list.
  Future<List<TrackInfo>> getTracks() async {
    final list = await _channel.invokeMethod<List<dynamic>>('getTracks');
    return list
            ?.map((e) => TrackInfo.fromMap(e as Map<dynamic, dynamic>))
            .toList() ??
        [];
  }

  /// Get diagnostics data (placeholder, requires native counters).
  Future<Map<String, dynamic>> getDiagnostics() async {
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      'getDiagnostics',
    );
    return Map<String, dynamic>.from(map ?? {});
  }

  Future<void> dispose() async {
    if (_textureId != null) {
      await _channel.invokeMethod<void>('destroyRenderer');
      _textureId = null;
    }
  }

  /// Open a native file picker dialog (IFileDialog) and return selected paths.
  /// Returns null if user cancels.
  Future<List<String>?> pickFiles({bool allowMultiple = true}) async {
    final result = await _channel.invokeMethod<List<dynamic>>('pickFiles', {
      'allowMultiple': allowMultiple,
    });
    if (result == null || result.isEmpty) return null;
    return result.cast<String>();
  }
}
