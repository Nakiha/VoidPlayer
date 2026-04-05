import 'package:flutter/services.dart';

/// Layout mode constants matching native defines.
class LayoutMode {
  static const int sideBySide = 0;
  static const int splitScreen = 1;
}

/// Immutable snapshot of the layout state.
class LayoutState {
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
        order: (map['order'] as List<dynamic>?)
                ?.map((e) => e as int)
                .toList() ??
            const [0, 1, 2, 3],
      );

  LayoutState copyWith({
    int? mode,
    double? splitPos,
    double? zoomRatio,
    double? viewOffsetX,
    double? viewOffsetY,
    List<int>? order,
  }) =>
      LayoutState(
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

  Future<int> createRenderer(
    List<String> videoPaths, {
    int width = 1920,
    int height = 1080,
  }) async {
    _textureId = await _channel.invokeMethod<int>('createRenderer', {
      'videoPaths': videoPaths,
      'width': width,
      'height': height,
    });
    return _textureId!;
  }

  Future<void> play() => _channel.invokeMethod<void>('play');

  Future<void> pause() => _channel.invokeMethod<void>('pause');

  Future<void> seek(int ptsUs) => _channel.invokeMethod<void>('seek', {
        'ptsUs': ptsUs,
      });

  Future<void> setSpeed(double speed) =>
      _channel.invokeMethod<void>('setSpeed', {
        'speed': speed,
      });

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

  Future<void> dispose() async {
    if (_textureId != null) {
      await _channel.invokeMethod<void>('destroyRenderer');
      _textureId = null;
    }
  }
}
