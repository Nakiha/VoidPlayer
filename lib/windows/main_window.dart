import 'dart:async';
import 'package:desktop_drop/desktop_drop.dart';
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
import '../widgets/media_header.dart';
import '../widgets/timeline_area.dart';
import '../widgets/analysis_panel.dart';
import '../analysis/analysis_manager.dart';

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

  // Per-track sync offsets: slot -> offset in microseconds
  Map<int, int> _syncOffsets = {};

  // Slider hover state for cross-track indicator
  int _hoverPtsUs = 0;
  bool _sliderHovering = false;

  // Polling
  Timer? _pollTimer;
  Ticker? _layoutTicker;
  bool _layoutDirty = false;

  // Viewport resize
  int _viewportWidth = 0;
  int _viewportHeight = 0;
  bool _resizeDirty = false;

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
    actionRegistry.bind(const OpenAnalysis(), (_) => _triggerAnalysis());
  }

  void _togglePlayPause() async {
    if (_isPlaying) {
      await _controller.pause();
    } else {
      await _controller.play();
    }
    setState(() => _isPlaying = !_isPlaying);
  }

  Future<void> _triggerAnalysis() async {
    final mgr = AnalysisManager.instance;
    for (final entry in _trackManager.entries) {
      final hash = await mgr.ensureAndLoad(entry.path);
      if (hash != null) {
        WindowManager.showAnalysisWindow(hash);
      }
    }
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

  /// Load media files by paths (shared by file picker, drag-drop, and test scripts).
  void _loadMediaPaths(List<String> paths) async {
    if (paths.isEmpty) return;

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

  /// Add media by path (used by test scripts, bypasses file picker).
  void _addMediaByPath(String path) {
    if (path.isEmpty) return;
    _loadMediaPaths([path]);
  }

  // -- File opening --

  void _openFile() async {
    final paths = await _controller.pickFiles(allowMultiple: true);
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
      _layout = _layout.copyWith(splitPos: normalizedX.clamp(0.1, 0.9));
    });
    _markLayoutDirty();
  }

  void _onZoom(double scrollDelta, Offset localPos) {
    final factor = scrollDelta > 0 ? 0.9 : 1.1;
    final newZoom = (_layout.zoomRatio * factor).clamp(LayoutState.zoomMin, LayoutState.zoomMax);

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
      if (_resizeDirty && _textureId != null &&
          _viewportWidth > 0 && _viewportHeight > 0) {
        _resizeDirty = false;
        _controller.resize(_viewportWidth, _viewportHeight);
      }
      if (_layoutDirty && _textureId != null) {
        _layoutDirty = false;
        _controller.applyLayout(_layout);
      }
      if (!_resizeDirty && !_layoutDirty) {
        _layoutTicker?.stop();
      }
    });
    // Don't start here — will start on first dirty mark.
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
      final pts = results[0] as int;
      final dur = results[1] as int;
      final playing = results[2] as bool;
      if (pts == _currentPtsUs && dur == _durationUs && playing == _isPlaying) return;
      setState(() {
        _currentPtsUs = pts;
        _durationUs = dur;
        _isPlaying = playing;
      });
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
      setState(() {
        _trackManager.clear();
        _textureId = null;
        _viewportState = 1; // empty
        _isPlaying = false;
        _currentPtsUs = 0;
        _durationUs = 0;
        _layout = const LayoutState();
        _syncOffsets = {};
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

    setState(() {
      _syncOffsets = Map.from(_syncOffsets)..[slot] = newOffsetUs;
    });
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
      onDragEntered: (_) { if (!_dragging) setState(() => _dragging = true); },
      onDragExited: (_) { if (_dragging) setState(() => _dragging = false); },
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
                    zoomRatio: _layout.zoomRatio,
                    onZoomChanged: _onZoomComboChanged,
                    isPlaying: _isPlaying,
                    onTogglePlay: _togglePlayPause,
                    onStepForward: () => _controller.stepForward(),
                    onStepBackward: () => _controller.stepBackward(),
                    currentPtsUs: _currentPtsUs,
                    durationUs: _effectiveDurationUs,
                    onSeek: (pts) => _controller.seek(pts),
                    onHoverChanged: _onSliderHover,
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
                        color: Theme.of(context).colorScheme.primary.withValues(alpha: 0.5),
                        width: 3,
                      ),
                      color: Theme.of(context).colorScheme.primary.withValues(alpha: 0.08),
                    ),
                  ),
                ),
              ),
            // Analysis floating button (only when video is loaded)
            if (_viewportState == 2)
              AnalysisPanel(
                onTriggerAnalysis: _triggerAnalysis,
              ),
          ],
        ),
      ),
    );
  }
}
