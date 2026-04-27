part of 'main_window.dart';

extension _MainWindowActionBindings on _MainWindowState {
  void _bindActions() {
    // Playback
    actionRegistry.bind(const TogglePlayPause(), (_) => _togglePlayPause());
    actionRegistry.bind(const Play(), (_) => _play());
    actionRegistry.bind(const Pause(), (_) => _pause());
    actionRegistry.bind(const StepForward(), (_) => _controller.stepForward());
    actionRegistry.bind(
      const StepBackward(),
      (_) => _controller.stepBackward(),
    );
    actionRegistry.bind(const SeekTo(0), (action) {
      final a = action as SeekTo;
      _seekTo(a.ptsUs);
    });
    actionRegistry.bind(const ClickTimelineFraction(0), (action) {
      final a = action as ClickTimelineFraction;
      _clickTimelineFraction(a.fraction);
    });
    actionRegistry.bind(const SetSpeed(1.0), (action) {
      final a = action as SetSpeed;
      _setSpeed(a.speed);
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
    actionRegistry.bind(const AdjustTrackOffset(0, 0), (action) {
      final a = action as AdjustTrackOffset;
      _onOffsetChanged(a.slot, a.deltaMs);
    });
    actionRegistry.bind(const SetLoopEnabled(false), (action) {
      final a = action as SetLoopEnabled;
      _setLoopRangeEnabled(a.enabled);
    });
    actionRegistry.bind(const SetLoopRange(0, 0), (action) {
      final a = action as SetLoopRange;
      _setLoopRange(a.startUs, a.endUs, seekToStart: _loopRangeEnabled);
    });
    actionRegistry.bind(const DragLoopHandle('end', 0), (action) {
      final a = action as DragLoopHandle;
      _dragLoopHandle(a.handle, a.targetUs, steps: a.steps);
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
    actionRegistry.bind(
      const NewWindow(),
      (_) => WindowManager.showStatsWindow(),
    );
    actionRegistry.bind(
      const OpenSettings(),
      (_) => WindowManager.showSettingsWindow(),
    );
    actionRegistry.bind(
      const OpenStats(),
      (_) => WindowManager.showStatsWindow(),
    );
    actionRegistry.bind(
      const OpenMemory(),
      (_) => WindowManager.showMemoryWindow(),
    );
    actionRegistry.bind(const RunAnalysis(), (_) => _triggerAnalysis());
  }
}
