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

  void _dragLoopHandle(String handle, int targetUs, {int steps = 12}) {
    final context = _loopRangeBarKey.currentContext;
    if (context == null) {
      throw StateError('Loop range bar is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Loop range bar has no render box');
    }
    final durationUs = _effectiveDurationUs;
    if (durationUs <= 0) {
      throw StateError(
        'Cannot drag loop handle before media duration is known',
      );
    }

    final normalizedHandle = handle.toLowerCase();
    final isEnd = normalizedHandle == 'end' || normalizedHandle == 'tail';
    final isStart = normalizedHandle == 'start' || normalizedHandle == 'head';
    if (!isStart && !isEnd) {
      throw ArgumentError('Unknown loop handle "$handle"; expected start/end');
    }

    const margin = 8.0;
    final timelineLeft = _timelineStartWidth;
    final drawableWidth = renderObject.size.width - timelineLeft - margin * 2;
    if (drawableWidth <= 0) {
      throw StateError('Loop range timeline has no drawable width');
    }

    final minRangeUs = durationUs > 10000 ? 10000 : 0;
    final currentUs = isEnd ? _resolvedLoopEndUs : _resolvedLoopStartUs;
    final clampedTargetUs =
        (isEnd
                ? targetUs.clamp(_resolvedLoopStartUs + minRangeUs, durationUs)
                : targetUs.clamp(0, _resolvedLoopEndUs - minRangeUs))
            .toInt();

    Offset pointForUs(int us) {
      final ratio = (us / durationUs).clamp(0.0, 1.0);
      return renderObject.localToGlobal(
        Offset(timelineLeft + margin + drawableWidth * ratio, 20),
      );
    }

    final start = pointForUs(currentUs);
    final target = pointForUs(clampedTargetUs);
    final dragDirection = (target.dx - start.dx).sign;
    const dragSlopCompensation = 24.0;
    final dragEndX = dragDirection == 0
        ? target.dx
        : (target.dx + dragDirection * dragSlopCompensation).clamp(
            renderObject.localToGlobal(Offset(timelineLeft + margin, 20)).dx,
            renderObject
                .localToGlobal(
                  Offset(timelineLeft + margin + drawableWidth, 20),
                )
                .dx,
          );
    final end = Offset(dragEndX.toDouble(), target.dy);
    final count = steps <= 0 ? 1 : steps;
    final pointer = _testPointerId++;
    var previous = start;

    log.info(
      'Test action: DRAG_LOOP_HANDLE $normalizedHandle '
      '$currentUs->$clampedTargetUs us steps=$count '
      'global=(${start.dx.toStringAsFixed(1)}, ${start.dy.toStringAsFixed(1)})'
      '->target(${target.dx.toStringAsFixed(1)}, ${target.dy.toStringAsFixed(1)})'
      ' dragEnd=(${end.dx.toStringAsFixed(1)}, ${end.dy.toStringAsFixed(1)})',
    );

    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(pointer: pointer, position: start),
    );
    for (var i = 1; i <= count; i++) {
      final t = i / count;
      final next = Offset(
        start.dx + (end.dx - start.dx) * t,
        start.dy + (end.dy - start.dy) * t,
      );
      GestureBinding.instance.handlePointerEvent(
        PointerMoveEvent(
          pointer: pointer,
          position: next,
          delta: next - previous,
        ),
      );
      previous = next;
    }
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(pointer: pointer, position: end),
    );
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
