part of 'main_window.dart';

extension _MainWindowPlayback on _MainWindowState {
  void _togglePlayPause() async {
    if (_isPlaying) {
      await _pause();
    } else {
      await _play();
    }
  }

  Future<void> _play() async {
    if (_loopRangeEnabled && !_currentPtsInsideLoopRange) {
      await _seekToAsync(_resolvedLoopStartUs);
      if (!mounted) return;
    }
    await _controller.play();
    if (!mounted) return;
    _setPlaying(true);
    _scheduleLoopBoundaryTimer();
  }

  Future<void> _pause() async {
    await _controller.pause();
    if (!mounted) return;
    _cancelLoopBoundaryTimer();
    _setPlaying(false);
  }

  void _setSpeed(double speed) {
    _playbackSpeed = speed > 0 ? speed : 1.0;
    unawaited(_controller.setSpeed(speed));
    _scheduleLoopBoundaryTimer();
  }

  void _seekTo(int ptsUs) {
    unawaited(_seekToAsync(ptsUs).catchError((_) {}));
  }

  Future<void> _seekToAsync(int ptsUs) async {
    _setSeekPreview(ptsUs);
    await _controller.seek(ptsUs);
    if (!mounted) return;
    _scheduleLoopBoundaryTimer(fromPtsUs: ptsUs);
  }

  double get _timelineStartWidth =>
      _MainWindowState._trackDragHandleWidth +
      _timelineControlsWidth +
      _MainWindowState._trackDividerWidth;

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
      _setPolledPlaybackState(pts, dur, playing);
      if (playing) {
        _scheduleLoopBoundaryTimer(fromPtsUs: pts);
      } else {
        _cancelLoopBoundaryTimer();
      }
    } catch (_) {
      // Renderer may be disposed.
    }
  }

  void _setLoopRangeEnabled(bool enabled) async {
    if (enabled) {
      _ensureLoopRangeInitialized();
      _setLoopRangeEnabledState(true);
      _syncNativeLoopRange();
      await _controller.pause();
      if (!mounted) return;
      _cancelLoopBoundaryTimer();
      _setPlaying(false);
      _seekTo(_resolvedLoopStartUs);
    } else {
      _cancelLoopBoundaryTimer();
      _setLoopRangeEnabledState(false);
      _syncNativeLoopRange();
    }
  }

  void _applyStartupLoopRangeIfReady() {
    if (_startupLoopRangeApplied || _trackManager.isEmpty) return;

    final range = widget.startupOptions.loopRange;
    if (range == null) return;

    final effectiveDurationUs = _effectiveDurationUs;
    if (effectiveDurationUs <= 0) return;

    _startupLoopRangeApplied = true;
    log.info('Applying startup loop range: ${range.startUs}:${range.endUs} us');
    _setLoopRange(range.startUs, range.endUs);
    _setLoopRangeEnabled(true);
  }

  void _setLoopRange(
    int startUs,
    int endUs, {
    bool seekToStart = false,
    bool seekOnlyIfStartChanged = false,
  }) async {
    final effectiveDurationUs = _effectiveDurationUs;
    final previousStartUs = _resolvedLoopStartUs;
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

    _setLoopRangeState(clampedStartUs, clampedEndUs);
    if (_loopRangeEnabled) {
      _syncNativeLoopRange();
    }
    _scheduleLoopBoundaryTimer();

    if (seekToStart &&
        _loopRangeEnabled &&
        (!seekOnlyIfStartChanged || clampedStartUs != previousStartUs)) {
      await _controller.pause();
      if (!mounted) return;
      _cancelLoopBoundaryTimer();
      _setPlaying(false);
      _seekTo(_resolvedLoopStartUs);
    }
  }

  void _syncNativeLoopRange() {
    final enabled = _loopRangeEnabled;
    final startUs = _resolvedLoopStartUs;
    final endUs = _resolvedLoopEndUs;
    final serial = ++_loopRangeSyncSerial;
    _nativeLoopRangeSynced = false;
    unawaited(
      _controller
          .setLoopRange(enabled: enabled, startUs: startUs, endUs: endUs)
          .then((_) {
            if (!mounted || serial != _loopRangeSyncSerial) return;
            _nativeLoopRangeSynced = enabled;
            if (_nativeLoopRangeSynced) {
              _cancelLoopBoundaryTimer();
            } else {
              _scheduleLoopBoundaryTimer();
            }
          })
          .catchError((_) {
            if (!mounted || serial != _loopRangeSyncSerial) return;
            _nativeLoopRangeSynced = false;
            _scheduleLoopBoundaryTimer();
          }),
    );
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
    _setSliderHoverState(hoverUs, hovering);
  }
}
