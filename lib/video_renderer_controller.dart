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

/// Result of createPlayer, containing texture ID and initial track info.
class CreatePlayerResult {
  final int textureId;
  final List<TrackInfo> tracks;

  const CreatePlayerResult({required this.textureId, required this.tracks});
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

class NativePlayerController {
  static const MethodChannel _channel = MethodChannel('video_renderer');

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

  Map<dynamic, dynamic> _requireMap(Map<dynamic, dynamic>? map, String method) {
    if (map == null) {
      throw StateError('$method returned invalid payload: null');
    }
    return map;
  }

  TrackInfo _trackInfoFromValue(Object? value, String context) {
    if (value is! Map) {
      throw StateError('$context returned invalid track payload: $value');
    }
    return TrackInfo.fromMap(Map<dynamic, dynamic>.from(value));
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
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      'createPlayer',
      {'videoPaths': videoPaths, 'width': width, 'height': height},
    );
    final payload = _requireMap(map, 'createPlayer');
    final textureId = payload['textureId'];
    if (textureId is! int) {
      throw StateError(
        'createPlayer returned invalid textureId: ${payload['textureId']}',
      );
    }
    final tracksValue = payload['tracks'];
    if (tracksValue != null && tracksValue is! List) {
      throw StateError(
        'createPlayer returned invalid tracks payload: $tracksValue',
      );
    }
    final tracksList = tracksValue as List<dynamic>? ?? [];
    _textureId = textureId;
    final result = CreatePlayerResult(
      textureId: textureId,
      tracks: tracksList
          .map((e) => _trackInfoFromValue(e, 'createPlayer'))
          .toList(),
    );
    if (_disposed) {
      _textureId = null;
      await _channel.invokeMethod<void>('destroyPlayer');
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
    _ensureAlive();
    return _channel.invokeMethod<void>('play');
  }

  Future<void> pause() {
    _ensureAlive();
    return _channel.invokeMethod<void>('pause');
  }

  Future<void> seek(int ptsUs) {
    _ensureAlive();
    return _channel.invokeMethod<void>('seek', {'ptsUs': ptsUs});
  }

  Future<void> setSpeed(double speed) {
    _ensureAlive();
    return _channel.invokeMethod<void>('setSpeed', {'speed': speed});
  }

  Future<void> setLoopRange({
    required bool enabled,
    required int startUs,
    required int endUs,
  }) {
    _ensureAlive();
    return _channel.invokeMethod<void>('setLoopRange', {
      'enabled': enabled,
      'startUs': startUs,
      'endUs': endUs,
    });
  }

  Future<void> setAudibleTrack(int? fileId) {
    _ensureAlive();
    return _channel.invokeMethod<void>('setAudibleTrack', {
      'fileId': fileId ?? -1,
    });
  }

  Future<void> resize(int width, int height) {
    _ensureAlive();
    return _channel.invokeMethod<void>('resize', {
      'width': width,
      'height': height,
    });
  }

  Future<void> setViewportBackgroundColor(int colorValue) {
    _ensureAlive();
    _viewportBackgroundColor = colorValue;
    return _channel.invokeMethod<void>('setViewportBackgroundColor', {
      'color': colorValue,
    });
  }

  Future<ViewportCapture> captureViewport({String? outputPath}) async {
    _ensureAlive();
    final args = <String, dynamic>{};
    if (outputPath != null) {
      args['outputPath'] = outputPath;
    }
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      'captureViewport',
      args,
    );
    return ViewportCapture.fromMap(_requireMap(map, 'captureViewport'));
  }

  Future<void> stepForward() {
    _ensureAlive();
    return _channel.invokeMethod<void>('stepForward');
  }

  Future<void> stepBackward() {
    _ensureAlive();
    return _channel.invokeMethod<void>('stepBackward');
  }

  Future<int> currentPts() async {
    _ensureAlive();
    return await _channel.invokeMethod<int>('currentPts') ?? 0;
  }

  Future<int> duration() async {
    _ensureAlive();
    return await _channel.invokeMethod<int>('duration') ?? 0;
  }

  Future<bool> isPlaying() async {
    _ensureAlive();
    return await _channel.invokeMethod<bool>('isPlaying') ?? false;
  }

  /// Atomically apply layout state and trigger redraw if paused.
  Future<void> applyLayout(LayoutState state) {
    _ensureAlive();
    return _channel.invokeMethod<void>('applyLayout', state.toMap());
  }

  /// Get a snapshot of the current layout state.
  Future<LayoutState> getLayout() async {
    _ensureAlive();
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>('getLayout');
    return LayoutState.fromMap(map ?? {});
  }

  /// Add a video track at the first empty slot.
  Future<TrackInfo> addTrack(String videoPath) async {
    _ensureAlive();
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>('addTrack', {
      'path': videoPath,
    });
    return _trackInfoFromValue(_requireMap(map, 'addTrack'), 'addTrack');
  }

  /// Remove a track by file_id.
  Future<void> removeTrack(int fileId) {
    _ensureAlive();
    return _channel.invokeMethod<void>('removeTrack', {'fileId': fileId});
  }

  /// Destroy the native player and texture while keeping this controller
  /// reusable for a future [createPlayer] call.
  Future<void> destroyPlayerOnly() {
    _ensureAlive();
    return _destroyPlayer(markDisposed: false);
  }

  /// Set per-track sync offset in microseconds.
  Future<void> setTrackOffset({required int fileId, required int offsetUs}) {
    _ensureAlive();
    return _channel.invokeMethod<void>('setTrackOffset', {
      'fileId': fileId,
      'offsetUs': offsetUs,
    });
  }

  /// Get current track info list.
  Future<List<TrackInfo>> getTracks() async {
    _ensureAlive();
    final list = await _channel.invokeMethod<List<dynamic>>('getTracks');
    return list?.map((e) => _trackInfoFromValue(e, 'getTracks')).toList() ?? [];
  }

  /// Get diagnostics data (placeholder, requires native counters).
  Future<Map<String, dynamic>> getDiagnostics() async {
    _ensureAlive();
    final map = await _channel.invokeMethod<Map<dynamic, dynamic>>(
      'getDiagnostics',
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
      await _channel.invokeMethod<void>('destroyPlayer');
    }
  }
}
