import 'dart:async';
import 'package:flutter/gestures.dart';
import 'package:desktop_drop/desktop_drop.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:path/path.dart' as p;
import '../app_log.dart';
import '../video_renderer_controller.dart';
import '../track_manager.dart';
import '../actions/action_registry.dart';
import '../actions/player_action.dart';
import '../actions/test_runner.dart';
import 'window_manager.dart';
import '../widgets/toolbar.dart';
import '../widgets/viewport_panel.dart';
import '../widgets/controls_bar.dart';
import '../widgets/loop_range_bar.dart';
import '../widgets/media_header.dart';
import '../widgets/timeline_area.dart';
import '../analysis/analysis_manager.dart';
import 'analysis_ipc.dart';
import 'native_file_picker.dart';

part 'main_window_actions.dart';

class MainWindow extends StatefulWidget {
  final String? testScriptPath;
  const MainWindow({super.key, this.testScriptPath});

  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> with TickerProviderStateMixin {
  static const double _trackDragHandleWidth = 28.0;
  static const double _trackDividerWidth = 1.0;

  final _controller = VideoRendererController();
  final _trackManager = TrackManager();
  final _analysisIpcServer = AnalysisIpcServer();
  final _analysisHashesByFileId = <int, String>{};
  final _timelineSliderKey = GlobalKey();
  int _testPointerId = 9000;
  int _analysisSnapshotSerial = 0;

  // Renderer state
  int? _textureId;
  int _viewportState = 1; // 0=loading, 1=empty, 2=active
  bool _isPlaying = false;
  double _playbackSpeed = 1.0;
  int _currentPtsUs = 0;
  int _durationUs = 0;
  LayoutState _layout = const LayoutState();
  int? _pendingSeekUs;
  DateTime? _pendingSeekAt;

  // Per-track sync offsets: slot -> offset in microseconds
  Map<int, int> _syncOffsets = {};

  // Shared timeline alignment + loop range state
  double _timelineControlsWidth = 320;
  bool _loopRangeEnabled = false;
  bool _nativeLoopRangeSynced = false;
  int _loopStartUs = 0;
  int _loopEndUs = 0;

  // Slider hover state for cross-track indicator
  int _hoverPtsUs = 0;
  bool _sliderHovering = false;

  // Polling
  Timer? _pollTimer;
  Timer? _loopBoundaryTimer;
  Ticker? _layoutTicker;
  bool _layoutDirty = false;

  // Viewport resize
  int _viewportWidth = 0;
  int _viewportHeight = 0;
  bool _resizeDirty = false;
  bool _layoutFlushInProgress = false;

  // Drag-drop
  bool _dragging = false;

  @override
  void initState() {
    super.initState();
    _trackManager.addListener(_onTrackManagerChanged);
    _bindActions();
    _startPolling();
    _startLayoutTicker();
    _maybeStartTestRunner();
  }

  void _maybeStartTestRunner() {
    final path = widget.testScriptPath;
    if (path == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(scriptPath: path, controller: _controller).run();
    });
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _loopBoundaryTimer?.cancel();
    _layoutTicker?.dispose();
    unawaited(_analysisIpcServer.dispose());
    _trackManager.dispose();
    _controller.dispose();
    super.dispose();
  }

  // -- TrackManager listener --

  void _onTrackManagerChanged() {
    setState(() {
      _layout = _layout.copyWith(order: _trackManager.order);
    });
    _markLayoutDirty();
    unawaited(_publishAnalysisTrackSnapshot());
  }

  void _togglePlayPause() async {
    if (_isPlaying) {
      await _pause();
    } else {
      await _play();
    }
  }

  Future<void> _play() async {
    if (_loopRangeEnabled && !_currentPtsInsideLoopRange) {
      _seekTo(_resolvedLoopStartUs);
    }
    await _controller.play();
    if (!mounted) return;
    setState(() => _isPlaying = true);
    _scheduleLoopBoundaryTimer();
  }

  Future<void> _pause() async {
    await _controller.pause();
    if (!mounted) return;
    _cancelLoopBoundaryTimer();
    setState(() => _isPlaying = false);
  }

  void _setSpeed(double speed) {
    _playbackSpeed = speed > 0 ? speed : 1.0;
    _controller.setSpeed(speed);
    _scheduleLoopBoundaryTimer();
  }

  void _syncNativeLoopRange() {
    final enabled = _loopRangeEnabled;
    final startUs = _resolvedLoopStartUs;
    final endUs = _resolvedLoopEndUs;
    _nativeLoopRangeSynced = false;
    unawaited(
      _controller
          .setLoopRange(enabled: enabled, startUs: startUs, endUs: endUs)
          .then((_) {
            if (!mounted) return;
            _nativeLoopRangeSynced = enabled;
            if (_nativeLoopRangeSynced) {
              _cancelLoopBoundaryTimer();
            } else {
              _scheduleLoopBoundaryTimer();
            }
          })
          .catchError((_) {
            if (!mounted) return;
            _nativeLoopRangeSynced = false;
            _scheduleLoopBoundaryTimer();
          }),
    );
  }

  void _seekTo(int ptsUs) {
    setState(() {
      _currentPtsUs = ptsUs;
      _pendingSeekUs = ptsUs;
      _pendingSeekAt = DateTime.now();
    });
    _controller.seek(ptsUs);
    _scheduleLoopBoundaryTimer(fromPtsUs: ptsUs);
  }

  double get _timelineStartWidth =>
      _trackDragHandleWidth + _timelineControlsWidth + _trackDividerWidth;

  int get _resolvedLoopStartUs =>
      _loopStartUs.clamp(0, _effectiveDurationUs).toInt();

  int get _resolvedLoopEndUs {
    final effectiveDurationUs = _effectiveDurationUs;
    if (effectiveDurationUs <= 0) return 0;
    final defaultEndUs = _loopEndUs <= 0 ? effectiveDurationUs : _loopEndUs;
    return defaultEndUs
        .clamp(_resolvedLoopStartUs, effectiveDurationUs)
        .toInt();
  }

  List<int> get _loopMarkerPtsUs {
    if (!_loopRangeEnabled || _effectiveDurationUs <= 0) return const [];
    return [_resolvedLoopStartUs, _resolvedLoopEndUs];
  }

  bool get _currentPtsInsideLoopRange {
    if (!_loopRangeEnabled) return true;
    final startUs = _resolvedLoopStartUs;
    final endUs = _resolvedLoopEndUs;
    return _currentPtsUs >= startUs && _currentPtsUs < endUs;
  }

  void _setTimelineControlsWidth(double width) {
    if (_timelineControlsWidth == width) return;
    setState(() => _timelineControlsWidth = width);
  }

  void _clickTimelineFraction(double fraction) {
    final context = _timelineSliderKey.currentContext;
    if (context == null) {
      throw StateError('Timeline slider is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Timeline slider has no render box');
    }

    final clamped = fraction.clamp(0.0, 1.0).toDouble();
    final local = Offset(
      renderObject.size.width * clamped,
      renderObject.size.height / 2,
    );
    final global = renderObject.localToGlobal(local);
    final pointer = _testPointerId++;

    log.info(
      'Test action: CLICK_TIMELINE_FRACTION ${clamped.toStringAsFixed(4)} '
      'at global=(${global.dx.toStringAsFixed(1)}, ${global.dy.toStringAsFixed(1)})',
    );
    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(pointer: pointer, position: global),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(pointer: pointer, position: global),
    );
  }

  Future<void> _triggerAnalysis() async {
    if (_trackManager.isEmpty) return;
    final mgr = AnalysisManager.instance;
    final windows = <AnalysisWindowRequest>[];
    await _analysisIpcServer.start();
    WindowManager.analysisIpcPort = _analysisIpcServer.port;
    WindowManager.analysisIpcToken = _analysisIpcServer.token;
    for (final entry in _trackManager.entries) {
      final hash = await mgr.ensureAndLoad(entry.path);
      if (hash != null) {
        _analysisHashesByFileId[entry.fileId] = hash;
        windows.add((hash: hash, fileName: p.basename(entry.path)));
      }
    }
    await _publishAnalysisTrackSnapshot();
    await WindowManager.showAnalysisWindows(windows);
  }

  Future<void> _publishAnalysisTrackSnapshot() async {
    if (!_analysisIpcServer.isStarted) return;
    final serial = ++_analysisSnapshotSerial;
    final mgr = AnalysisManager.instance;
    final tracks = <AnalysisIpcTrack>[];
    final liveFileIds = _trackManager.entries.map((e) => e.fileId).toSet();
    _analysisHashesByFileId.removeWhere(
      (fileId, _) => !liveFileIds.contains(fileId),
    );

    for (final entry in _trackManager.entries) {
      var hash = _analysisHashesByFileId[entry.fileId];
      if (hash == null) {
        hash = await mgr.ensureAndLoad(entry.path);
        if (hash == null) continue;
        _analysisHashesByFileId[entry.fileId] = hash;
      }
      if (serial != _analysisSnapshotSerial) return;
      tracks.add(
        AnalysisIpcTrack(
          fileId: entry.fileId,
          slot: entry.slot,
          path: entry.path,
          fileName: entry.fileName,
          hash: hash,
          durationUs: entry.info.durationUs,
        ),
      );
    }

    if (serial != _analysisSnapshotSerial) return;
    _analysisIpcServer.publishTracks(tracks);
  }

  void _toggleLayoutMode() {
    _setLayoutMode(
      _layout.mode == LayoutMode.sideBySide
          ? LayoutMode.splitScreen
          : LayoutMode.sideBySide,
    );
  }

  void _setLayoutMode(int mode) {
    setState(() => _layout = _layout.copyWith(mode: mode));
    _markLayoutDirty();
  }

  void _setZoom(double ratio) {
    setState(
      () => _layout = _layout.copyWith(
        zoomRatio: ratio.clamp(LayoutState.zoomMin, LayoutState.zoomMax),
      ),
    );
    _markLayoutDirty();
  }

  void _setSplitPos(double pos) {
    setState(() => _layout = _layout.copyWith(splitPos: pos.clamp(0.0, 1.0)));
    _markLayoutDirty();
  }

  void _panByDelta(double dx, double dy) {
    setState(() {
      _layout = _layout.copyWith(
        viewOffsetX: _layout.viewOffsetX + dx,
        viewOffsetY: _layout.viewOffsetY + dy,
      );
    });
    _markLayoutDirty();
  }

  /// Load media files by paths (shared by file picker, drag-drop, and test scripts).
  void _loadMediaPaths(List<String> paths) async {
    if (paths.isEmpty) return;

    if (_textureId == null) {
      // First load: create renderer
      setState(() => _viewportState = 0);
      try {
        final initialWidth = _viewportWidth > 0 ? _viewportWidth : 1920;
        final initialHeight = _viewportHeight > 0 ? _viewportHeight : 1080;
        final res = await _controller.createRenderer(
          paths,
          width: initialWidth,
          height: initialHeight,
        );
        setState(() {
          _textureId = res.textureId;
        });
        _trackManager.setTracks(res.tracks);
        _layout = await _controller.getLayout();
        await WidgetsBinding.instance.endOfFrame;
        if (!mounted) return;
        if (_viewportWidth > 0 && _viewportHeight > 0) {
          await _controller.resize(_viewportWidth, _viewportHeight);
        }
        if (!mounted) return;
        setState(() => _viewportState = 2);
      } catch (e) {
        log.severe("createRenderer failed: $e");
        setState(() => _viewportState = 1);
      }
    } else {
      // Subsequent adds: add tracks
      for (final path in paths) {
        try {
          final track = await _controller.addTrack(path);
          _trackManager.addTrack(track);
        } catch (e) {
          log.severe("addTrack failed: $e");
        }
      }
    }
  }

  /// Add media by path (used by test scripts, bypasses file picker).
  void _addMediaByPath(String path) {
    if (path.isEmpty) return;
    _loadMediaPaths([path]);
  }

  // -- File opening --

  void _openFile() async {
    final paths = await WindowsNativeFilePicker.pickFiles(allowMultiple: true);
    if (paths == null || paths.isEmpty) return;
    _loadMediaPaths(paths);
  }

  // -- Viewport interaction --

  void _onPan(Offset delta) {
    setState(() {
      _layout = _layout.copyWith(
        viewOffsetX: _layout.viewOffsetX + delta.dx,
        viewOffsetY: _layout.viewOffsetY + delta.dy,
      );
    });
    _markLayoutDirty();
  }

  void _onSplit(double normalizedX) {
    setState(() {
      _layout = _layout.copyWith(splitPos: normalizedX.clamp(0.0, 1.0));
    });
    _markLayoutDirty();
  }

  void _onZoom(double scrollDelta, Offset localPos) {
    final factor = scrollDelta > 0 ? 0.9 : 1.1;
    final newZoom = (_layout.zoomRatio * factor).clamp(
      LayoutState.zoomMin,
      LayoutState.zoomMax,
    );

    // Zoomed out to floor (100%) — reset viewport offset to origin
    if (newZoom == LayoutState.zoomMin && factor < 1.0) {
      setState(() {
        _layout = _layout.copyWith(
          zoomRatio: newZoom,
          viewOffsetX: 0,
          viewOffsetY: 0,
        );
      });
      _markLayoutDirty();
      return;
    }

    final actualFactor = newZoom / _layout.zoomRatio;

    // Fallback if viewport size unknown
    if (_viewportWidth <= 0 || _viewportHeight <= 0) {
      setState(() {
        _layout = _layout.copyWith(zoomRatio: newZoom);
      });
      _markLayoutDirty();
      return;
    }

    // Compute cursor position in slot-normalized coords and slot pixel size.
    // Formula: offset_new = factor * offset_old + (1 - factor) * (cursor - 0.5) * slot_pixels
    double cursorX, cursorY, slotW, slotH;

    if (_layout.mode == LayoutMode.sideBySide) {
      final n = _trackManager.count > 0 ? _trackManager.count : 1;
      final nx = localPos.dx / _viewportWidth;
      final ny = localPos.dy / _viewportHeight;
      final slotIndex = (nx * n).floor().clamp(0, n - 1);
      cursorX = nx * n - slotIndex;
      cursorY = ny;
      slotW = _viewportWidth / n;
      slotH = _viewportHeight.toDouble();
    } else {
      // Split screen: cursor in full canvas UV
      cursorX = localPos.dx / _viewportWidth;
      cursorY = localPos.dy / _viewportHeight;
      slotW = _viewportWidth.toDouble();
      slotH = _viewportHeight.toDouble();
    }

    setState(() {
      _layout = _layout.copyWith(
        zoomRatio: newZoom,
        viewOffsetX:
            actualFactor * _layout.viewOffsetX +
            (1 - actualFactor) * (cursorX - 0.5) * slotW,
        viewOffsetY:
            actualFactor * _layout.viewOffsetY +
            (1 - actualFactor) * (cursorY - 0.5) * slotH,
      );
    });
    _markLayoutDirty();
  }

  void _onPointerButton(bool panning, bool splitting) {
    // No-op for now; could show cursor changes etc.
  }

  void _onViewportResize(int width, int height) {
    if (width == _viewportWidth && height == _viewportHeight) return;
    _viewportWidth = width;
    _viewportHeight = height;
    _resizeDirty = true;
    _layoutTicker?.start();
  }

  void _onZoomComboChanged(double value) {
    setState(() {
      _layout = _layout.copyWith(zoomRatio: value);
    });
    _markLayoutDirty();
  }

  // -- Layout sync --

  void _markLayoutDirty() {
    _layoutDirty = true;
    _layoutTicker?.start();
  }

  void _startLayoutTicker() {
    _layoutTicker = createTicker((_) {
      unawaited(_flushPendingLayout());
    });
    // Don't start here — will start on first dirty mark.
  }

  Future<void> _flushPendingLayout() async {
    if (_layoutFlushInProgress) return;
    if (_textureId == null) {
      _resizeDirty = false;
      _layoutDirty = false;
      _layoutTicker?.stop();
      return;
    }

    _layoutFlushInProgress = true;
    try {
      while (mounted && (_resizeDirty || _layoutDirty)) {
        if (_layoutDirty) {
          final layout = _layout;
          _layoutDirty = false;
          await _controller.applyLayout(layout);
          if (!mounted) return;
        }

        if (_resizeDirty && _viewportWidth > 0 && _viewportHeight > 0) {
          final width = _viewportWidth;
          final height = _viewportHeight;
          _resizeDirty = false;
          await _controller.resize(width, height);
          if (!mounted) return;
          if (!_layoutDirty) {
            final layout = await _controller.getLayout();
            if (!mounted) return;
            setState(() => _layout = layout);
          }
        } else if (_resizeDirty) {
          _resizeDirty = false;
        }
      }
    } finally {
      _layoutFlushInProgress = false;
      if (mounted) {
        if (_resizeDirty || _layoutDirty) {
          _layoutTicker?.start();
        } else {
          _layoutTicker?.stop();
        }
      }
    }
  }

  // -- Polling --

  void _startPolling() {
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 200),
      (_) => _pollState(),
    );
  }

  void _cancelLoopBoundaryTimer() {
    _loopBoundaryTimer?.cancel();
    _loopBoundaryTimer = null;
  }

  void _scheduleLoopBoundaryTimer({int? fromPtsUs}) {
    _cancelLoopBoundaryTimer();
    if (!_loopRangeEnabled ||
        _nativeLoopRangeSynced ||
        !_isPlaying ||
        _playbackSpeed <= 0 ||
        _resolvedLoopEndUs <= _resolvedLoopStartUs) {
      return;
    }

    final startUs = _resolvedLoopStartUs;
    final endUs = _resolvedLoopEndUs;
    final baseUs = (fromPtsUs ?? _pendingSeekUs ?? _currentPtsUs)
        .clamp(startUs, endUs)
        .toInt();
    final remainingUs = endUs - baseUs;
    final delayUs = (remainingUs / _playbackSpeed).round();
    final delay = Duration(microseconds: delayUs.clamp(0, 1 << 31).toInt());
    _loopBoundaryTimer = Timer(delay, _onLoopBoundaryTimer);
  }

  void _onLoopBoundaryTimer() async {
    _loopBoundaryTimer = null;
    if (!_loopRangeEnabled ||
        !_isPlaying ||
        _resolvedLoopEndUs <= _resolvedLoopStartUs) {
      return;
    }

    final startUs = _resolvedLoopStartUs;
    final endUs = _resolvedLoopEndUs;
    var pts = _pendingSeekUs ?? _currentPtsUs;
    try {
      pts = await _controller.currentPts();
    } catch (_) {
      // Renderer may be disposed; fall back to the UI state below.
    }
    if (!mounted || !_loopRangeEnabled || !_isPlaying) return;

    if (pts < endUs - 12000) {
      _scheduleLoopBoundaryTimer(fromPtsUs: pts);
      return;
    }
    _seekTo(startUs);
  }

  void _pollState() async {
    if (_textureId == null) return;
    try {
      final results = await Future.wait([
        _controller.currentPts(),
        _controller.duration(),
        _controller.isPlaying(),
      ]);
      if (!mounted) return;
      var pts = results[0] as int;
      final dur = results[1] as int;
      final playing = results[2] as bool;
      final pendingSeekUs = _pendingSeekUs;
      if (pendingSeekUs != null) {
        final seekAge = _pendingSeekAt == null
            ? Duration.zero
            : DateTime.now().difference(_pendingSeekAt!);
        final settled = (pts - pendingSeekUs).abs() <= 50000;
        if (settled) {
          _pendingSeekUs = null;
          _pendingSeekAt = null;
        } else if (seekAge < const Duration(milliseconds: 1500)) {
          pts = pendingSeekUs;
        } else {
          _pendingSeekUs = null;
          _pendingSeekAt = null;
        }
      }
      if (_loopRangeEnabled &&
          playing &&
          _pendingSeekUs == null &&
          _resolvedLoopEndUs > _resolvedLoopStartUs &&
          pts >= _resolvedLoopEndUs) {
        _seekTo(_resolvedLoopStartUs);
        return;
      }
      if (pts == _currentPtsUs && dur == _durationUs && playing == _isPlaying) {
        return;
      }
      setState(() {
        _currentPtsUs = pts;
        _durationUs = dur;
        _isPlaying = playing;
      });
      if (playing) {
        _scheduleLoopBoundaryTimer(fromPtsUs: pts);
      } else {
        _cancelLoopBoundaryTimer();
      }
    } catch (_) {
      // Renderer may be disposed
    }
  }

  // -- Track operations --

  void _onRemoveTrack(int fileId) async {
    // Capture slot before removal for offset cleanup
    final entry = _trackManager.entries.firstWhere(
      (e) => e.fileId == fileId,
      orElse: () => throw StateError('No track with fileId $fileId'),
    );
    final slot = entry.info.slot;

    await _controller.removeTrack(fileId);
    final tracks = await _controller.getTracks();
    if (tracks.isEmpty) {
      // Last track removed: destroy renderer, show empty viewport
      await _controller.dispose();
      _cancelLoopBoundaryTimer();
      setState(() {
        _trackManager.clear();
        _textureId = null;
        _viewportState = 1; // empty
        _isPlaying = false;
        _currentPtsUs = 0;
        _durationUs = 0;
        _layout = const LayoutState();
        _syncOffsets = {};
        _loopRangeEnabled = false;
        _nativeLoopRangeSynced = false;
        _loopStartUs = 0;
        _loopEndUs = 0;
      });
    } else {
      _trackManager.setTracks(tracks);
      setState(() {
        _syncOffsets = Map.from(_syncOffsets)..remove(slot);
      });
    }
  }

  void _onMediaSwapped(int slotIndex, int targetTrackIndex) {
    _trackManager.moveTrack(slotIndex, targetTrackIndex);
  }

  void _onOffsetChanged(int slot, int deltaMs) async {
    final currentOffsetUs = _syncOffsets[slot] ?? 0;
    final newOffsetUs = currentOffsetUs + deltaMs * 1000; // ms -> us

    // Find the fileId for this slot
    final entry = _trackManager.entries.firstWhere(
      (e) => e.info.slot == slot,
      orElse: () => throw StateError('No track at slot $slot'),
    );

    await _controller.setTrackOffset(
      fileId: entry.fileId,
      offsetUs: newOffsetUs,
    );
    if (!mounted) return;

    setState(() {
      _syncOffsets = Map.from(_syncOffsets)..[slot] = newOffsetUs;
    });
    await _refreshTracksAtCurrentPosition();
  }

  Future<void> _refreshTracksAtCurrentPosition() async {
    var targetUs = _pendingSeekUs ?? _currentPtsUs;
    if (_pendingSeekUs == null) {
      try {
        targetUs = await _controller.currentPts();
      } catch (_) {
        targetUs = _currentPtsUs;
      }
    }
    if (!mounted) return;

    final clampedTargetUs = targetUs.clamp(0, _effectiveDurationUs).toInt();
    _seekTo(clampedTargetUs);
  }

  void _setLoopRangeEnabled(bool enabled) async {
    if (enabled) {
      _ensureLoopRangeInitialized();
      setState(() => _loopRangeEnabled = true);
      _syncNativeLoopRange();
      await _controller.pause();
      if (!mounted) return;
      _cancelLoopBoundaryTimer();
      setState(() => _isPlaying = false);
      _seekTo(_resolvedLoopStartUs);
    } else {
      _cancelLoopBoundaryTimer();
      setState(() => _loopRangeEnabled = false);
      _syncNativeLoopRange();
    }
  }

  void _setLoopRange(int startUs, int endUs, {bool seekToStart = false}) async {
    final effectiveDurationUs = _effectiveDurationUs;
    final minRangeUs = effectiveDurationUs > 10000 ? 10000 : 0;
    final clampedStartUs = startUs
        .clamp(
          0,
          (effectiveDurationUs - minRangeUs).clamp(0, effectiveDurationUs),
        )
        .toInt();
    final clampedEndUs = endUs
        .clamp(clampedStartUs + minRangeUs, effectiveDurationUs)
        .toInt();

    setState(() {
      _loopStartUs = clampedStartUs;
      _loopEndUs = clampedEndUs;
    });
    if (_loopRangeEnabled) {
      _syncNativeLoopRange();
    }
    _scheduleLoopBoundaryTimer();

    if (seekToStart && _loopRangeEnabled) {
      await _controller.pause();
      if (!mounted) return;
      _cancelLoopBoundaryTimer();
      setState(() => _isPlaying = false);
      _seekTo(_resolvedLoopStartUs);
    }
  }

  void _ensureLoopRangeInitialized() {
    final effectiveDurationUs = _effectiveDurationUs;
    if (effectiveDurationUs <= 0) return;
    if (_loopEndUs <= _loopStartUs || _loopEndUs > effectiveDurationUs) {
      _loopStartUs = _loopStartUs.clamp(0, effectiveDurationUs).toInt();
      _loopEndUs = effectiveDurationUs;
    }
  }

  void _onSliderHover(int hoverUs, bool hovering) {
    if (_hoverPtsUs == hoverUs && _sliderHovering == hovering) return;
    setState(() {
      _hoverPtsUs = hoverUs;
      _sliderHovering = hovering;
    });
  }

  /// Effective max duration accounting for per-track offsets.
  int get _effectiveDurationUs {
    int maxEffective = _durationUs;
    for (final entry in _trackManager.entries) {
      final offsetUs = _syncOffsets[entry.info.slot] ?? 0;
      final effective = entry.info.durationUs + offsetUs;
      if (effective > maxEffective) maxEffective = effective;
    }
    return maxEffective;
  }

  // -- Build --

  @override
  Widget build(BuildContext context) {
    return DropTarget(
      onDragEntered: (_) {
        if (!_dragging) setState(() => _dragging = true);
      },
      onDragExited: (_) {
        if (_dragging) setState(() => _dragging = false);
      },
      onDragDone: (details) {
        setState(() => _dragging = false);
        final paths = details.files
            .map((f) => f.path)
            .where((p) => p.isNotEmpty)
            .toList();
        if (paths.isNotEmpty) _loadMediaPaths(paths);
      },
      child: Scaffold(
        body: Stack(
          children: [
            Column(
              children: [
                // Toolbar (40px)
                AppToolBar(
                  viewMode: _layout.mode,
                  onViewModeChanged: (mode) {
                    setState(() => _layout = _layout.copyWith(mode: mode));
                    _markLayoutDirty();
                  },
                  onAddMedia: _openFile,
                  onAnalysis: _triggerAnalysis,
                  onProfiler: () => WindowManager.showStatsWindow(),
                  onSettings: () => WindowManager.showSettingsWindow(),
                  viewModeEnabled: _textureId != null,
                  analysisEnabled: _trackManager.count > 0,
                ),
                // Viewport (expanded)
                Expanded(
                  child: ViewportPanel(
                    textureId: _textureId,
                    viewportState: _viewportState,
                    layout: _layout,
                    onPan: _onPan,
                    onSplit: _onSplit,
                    onZoom: _onZoom,
                    onPointerButton: _onPointerButton,
                    onResize: _onViewportResize,
                  ),
                ),
                // Media header bar (per-track source combo + actions)
                if (_trackManager.count > 0)
                  MediaHeaderBar(
                    entries: _trackManager.entries,
                    onMediaSwapped: _onMediaSwapped,
                    onRemoveClicked: (slotIndex) {
                      if (slotIndex < _trackManager.count) {
                        _onRemoveTrack(_trackManager.entries[slotIndex].fileId);
                      }
                    },
                  ),
                // Controls bar (40px)
                if (_trackManager.count > 0)
                  ControlsBar(
                    timelineKey: _timelineSliderKey,
                    timelineStartWidth: _timelineStartWidth,
                    zoomRatio: _layout.zoomRatio,
                    onZoomChanged: _onZoomComboChanged,
                    isPlaying: _isPlaying,
                    onTogglePlay: _togglePlayPause,
                    onStepForward: () => _controller.stepForward(),
                    onStepBackward: () => _controller.stepBackward(),
                    currentPtsUs: _currentPtsUs,
                    durationUs: _effectiveDurationUs,
                    onSeek: _seekTo,
                    onHoverChanged: _onSliderHover,
                    markerUs: _loopMarkerPtsUs,
                    seekMinUs: _loopRangeEnabled ? _resolvedLoopStartUs : null,
                    seekMaxUs: _loopRangeEnabled ? _resolvedLoopEndUs : null,
                  ),
                if (_trackManager.count > 0)
                  LoopRangeBar(
                    timelineStartWidth: _timelineStartWidth,
                    enabled: _loopRangeEnabled,
                    startUs: _resolvedLoopStartUs,
                    endUs: _resolvedLoopEndUs,
                    durationUs: _effectiveDurationUs,
                    onEnabledChanged: _setLoopRangeEnabled,
                    onRangeChanged: (startUs, endUs) =>
                        _setLoopRange(startUs, endUs),
                    onRangeChangeEnd: _loopRangeEnabled
                        ? () => _setLoopRange(
                            _resolvedLoopStartUs,
                            _resolvedLoopEndUs,
                            seekToStart: true,
                          )
                        : null,
                  ),
                // Timeline area (variable, max 40%)
                if (_trackManager.count > 0)
                  Expanded(
                    flex: 0,
                    child: TimelineArea(
                      trackManager: _trackManager,
                      currentPtsUs: _currentPtsUs,
                      onRemoveTrack: _onRemoveTrack,
                      onReorder: _trackManager.moveTrack,
                      onOffsetChanged: _onOffsetChanged,
                      syncOffsets: _syncOffsets,
                      maxEffectiveDurationUs: _effectiveDurationUs,
                      hoverPtsUs: _hoverPtsUs,
                      sliderHovering: _sliderHovering,
                      controlsWidth: _timelineControlsWidth,
                      onControlsWidthChanged: _setTimelineControlsWidth,
                      markerPtsUs: _loopMarkerPtsUs,
                      loopRangeEnabled: _loopRangeEnabled,
                      loopStartUs: _resolvedLoopStartUs,
                      loopEndUs: _resolvedLoopEndUs,
                    ),
                  ),
              ],
            ),
            // Drag overlay
            if (_dragging)
              Positioned.fill(
                child: IgnorePointer(
                  child: Container(
                    decoration: BoxDecoration(
                      border: Border.all(
                        color: Theme.of(
                          context,
                        ).colorScheme.primary.withValues(alpha: 0.5),
                        width: 3,
                      ),
                      color: Theme.of(
                        context,
                      ).colorScheme.primary.withValues(alpha: 0.08),
                    ),
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
