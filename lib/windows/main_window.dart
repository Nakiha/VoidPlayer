import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
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
import '../widgets/timeline_area.dart';

class MainWindow extends StatefulWidget {
  final String? testScriptPath;
  const MainWindow({super.key, this.testScriptPath});

  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> with TickerProviderStateMixin {
  final _controller = VideoRendererController();
  final _trackManager = TrackManager();

  // Renderer state
  int? _textureId;
  int _viewportState = 1; // 0=loading, 1=empty, 2=active
  bool _isPlaying = false;
  int _currentPtsUs = 0;
  int _durationUs = 0;
  LayoutState _layout = const LayoutState();

  // Polling
  Timer? _pollTimer;
  Ticker? _layoutTicker;
  bool _layoutDirty = false;

  // Viewport resize
  int _viewportWidth = 0;
  int _viewportHeight = 0;
  bool _resizeDirty = false;

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
    _layoutTicker?.dispose();
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
  }

  // -- Action bindings --

  void _bindActions() {
    // Playback
    actionRegistry.bind(const TogglePlayPause(), (_) => _togglePlayPause());
    actionRegistry.bind(const Play(), (_) => _controller.play());
    actionRegistry.bind(const Pause(), (_) => _controller.pause());
    actionRegistry.bind(const StepForward(), (_) => _controller.stepForward());
    actionRegistry.bind(const StepBackward(), (_) => _controller.stepBackward());
    actionRegistry.bind(const SeekTo(0), (action) {
      final a = action as SeekTo;
      _controller.seek(a.ptsUs);
    });
    actionRegistry.bind(const SetSpeed(1.0), (action) {
      final a = action as SetSpeed;
      _controller.setSpeed(a.speed);
    });

    // Media
    actionRegistry.bind(const OpenFile(), (_) => _openFile());
    actionRegistry.bind(const AddMedia(''), (action) {
      final a = action as AddMedia;
      _addMediaByPath(a.path);
    });
    actionRegistry.bind(const RemoveTrackAction(0), (action) {
      final a = action as RemoveTrackAction;
      _onRemoveTrack(a.fileId);
    });

    // Layout
    actionRegistry.bind(const ToggleLayoutMode(), (_) => _toggleLayoutMode());
    actionRegistry.bind(const SetLayoutMode(0), (action) {
      final a = action as SetLayoutMode;
      _setLayoutMode(a.mode);
    });
    actionRegistry.bind(const SetZoom(1.0), (action) {
      final a = action as SetZoom;
      _setZoom(a.ratio);
    });
    actionRegistry.bind(const SetSplitPos(0.5), (action) {
      final a = action as SetSplitPos;
      _setSplitPos(a.position);
    });
    actionRegistry.bind(const Pan(0, 0), (action) {
      final a = action as Pan;
      _panByDelta(a.dx, a.dy);
    });

    // Window management
    actionRegistry.bind(const NewWindow(), (_) => WindowManager.showStatsWindow());
    actionRegistry.bind(const OpenSettings(), (_) => WindowManager.showSettingsWindow());
    actionRegistry.bind(const OpenStats(), (_) => WindowManager.showStatsWindow());
    actionRegistry.bind(const OpenMemory(), (_) => WindowManager.showMemoryWindow());
  }

  void _togglePlayPause() async {
    if (_isPlaying) {
      await _controller.pause();
    } else {
      await _controller.play();
    }
    setState(() => _isPlaying = !_isPlaying);
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
    setState(() => _layout = _layout.copyWith(zoomRatio: ratio.clamp(LayoutState.zoomMin, LayoutState.zoomMax)));
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

  /// Add media by path (used by test scripts, bypasses file picker).
  void _addMediaByPath(String path) async {
    if (path.isEmpty) return;

    if (_textureId == null) {
      setState(() => _viewportState = 0);
      try {
        final res = await _controller.createRenderer([path]);
        setState(() {
          _textureId = res.textureId;
          _viewportState = 2;
        });
        _trackManager.setTracks(res.tracks);
        _layout = await _controller.getLayout();
      } catch (e) {
        log.severe("createRenderer failed: $e");
        setState(() => _viewportState = 1);
      }
    } else {
      try {
        final track = await _controller.addTrack(path);
        _trackManager.addTrack(track);
      } catch (e) {
        log.severe("addTrack failed: $e");
      }
    }
  }

  // -- File opening --

  void _openFile() async {
    final paths = await _controller.pickFiles(allowMultiple: true);
    if (paths == null || paths.isEmpty) return;

    if (_textureId == null) {
      // First load: create renderer
      setState(() => _viewportState = 0);
      try {
        final res = await _controller.createRenderer(paths);
        setState(() {
          _textureId = res.textureId;
          _viewportState = 2;
        });
        _trackManager.setTracks(res.tracks);
        _layout = await _controller.getLayout();
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
      _layout = _layout.copyWith(splitPos: normalizedX.clamp(0.1, 0.9));
    });
    _markLayoutDirty();
  }

  void _onZoom(double scrollDelta, Offset localPos) {
    final factor = scrollDelta > 0 ? 0.9 : 1.1;
    final newZoom = (_layout.zoomRatio * factor).clamp(LayoutState.zoomMin, LayoutState.zoomMax);
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
        viewOffsetX: actualFactor * _layout.viewOffsetX +
            (1 - actualFactor) * (cursorX - 0.5) * slotW,
        viewOffsetY: actualFactor * _layout.viewOffsetY +
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
  }

  void _onZoomComboChanged(double value) {
    setState(() {
      _layout = _layout.copyWith(zoomRatio: value);
    });
    _markLayoutDirty();
  }

  // -- Layout sync --

  void _markLayoutDirty() => _layoutDirty = true;

  void _startLayoutTicker() {
    _layoutTicker = createTicker((_) {
      if (_resizeDirty && _textureId != null &&
          _viewportWidth > 0 && _viewportHeight > 0) {
        _resizeDirty = false;
        _controller.resize(_viewportWidth, _viewportHeight);
      }
      if (_layoutDirty && _textureId != null) {
        _layoutDirty = false;
        _controller.applyLayout(_layout);
      }
    });
    _layoutTicker!.start();
  }

  // -- Polling --

  void _startPolling() {
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 200),
      (_) => _pollState(),
    );
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
      setState(() {
        _currentPtsUs = results[0] as int;
        _durationUs = results[1] as int;
        _isPlaying = results[2] as bool;
      });
    } catch (_) {
      // Renderer may be disposed
    }
  }

  // -- Track operations --

  void _onRemoveTrack(int fileId) async {
    await _controller.removeTrack(fileId);
    final tracks = await _controller.getTracks();
    if (tracks.isEmpty) {
      // Last track removed: destroy renderer, show empty viewport
      await _controller.dispose();
      setState(() {
        _trackManager.clear();
        _textureId = null;
        _viewportState = 1; // empty
        _isPlaying = false;
        _currentPtsUs = 0;
        _durationUs = 0;
        _layout = const LayoutState();
      });
    } else {
      _trackManager.setTracks(tracks);
    }
  }

  void _onOffsetChanged(int slot, int offsetMs) {
    // TODO: wire to native when sync offset is supported
  }

  // -- Build --

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Column(
        children: [
          // Toolbar (40px)
          AppToolBar(
            viewMode: _layout.mode,
            onViewModeChanged: (mode) {
              setState(() => _layout = _layout.copyWith(mode: mode));
              _markLayoutDirty();
            },
            onAddMedia: _openFile,
            onNewWindow: () => WindowManager.showStatsWindow(),
            onSettings: () => WindowManager.showSettingsWindow(),
            onDebugMemory: () => WindowManager.showStatsWindow(),
            viewModeEnabled: _textureId != null,
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
          // Controls bar (40px)
          ControlsBar(
            zoomRatio: _layout.zoomRatio,
            onZoomChanged: _onZoomComboChanged,
            isPlaying: _isPlaying,
            onTogglePlay: _togglePlayPause,
            onStepForward: () => _controller.stepForward(),
            onStepBackward: () => _controller.stepBackward(),
            currentPtsUs: _currentPtsUs,
            durationUs: _durationUs,
            onSeek: (pts) => _controller.seek(pts),
          ),
          // Timeline area (variable, max 40%)
          if (_trackManager.count > 0)
            Expanded(
              flex: 0,
              child: TimelineArea(
                trackManager: _trackManager,
                playheadPosition: _durationUs > 0
                    ? _currentPtsUs / _durationUs
                    : 0.0,
                onRemoveTrack: _onRemoveTrack,
                onReorder: _trackManager.moveTrack,
                onOffsetChanged: _onOffsetChanged,
              ),
            ),
        ],
      ),
    );
  }
}
