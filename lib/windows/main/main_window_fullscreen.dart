import 'dart:async';

import 'package:flutter/material.dart';

import '../../platform/main_window_platform.dart';
import '../../utils/async_guard.dart';
import 'main_window_layout.dart';
import 'main_window_state.dart';

class MainWindowFullScreenCoordinator {
  final MainWindowPlatform platformWindow;
  final MainWindowLayoutCoordinator layoutCoordinator;
  final MainWindowStateStore stateStore;
  final GlobalKey viewportKey;
  final bool Function() mounted;

  Timer? _controlsTimer;
  int _serial = 0;
  bool? _pendingFullScreen;
  bool _uiResizePending = false;
  int? _windowedViewportWidth;
  int? _windowedViewportHeight;

  MainWindowFullScreenCoordinator({
    required this.platformWindow,
    required this.layoutCoordinator,
    required this.stateStore,
    required this.viewportKey,
    required this.mounted,
  });

  bool get uiResizePending => _uiResizePending;

  MainWindowStateModel get _state => stateStore.value;
  bool get _fullScreen => _state.fullScreen;

  void dispose() {
    _serial++;
    _controlsTimer?.cancel();
    _controlsTimer = null;
  }

  void toggle() {
    final currentTarget = _pendingFullScreen ?? _fullScreen;
    request(!currentTarget, reason: 'toggle full screen');
  }

  void exit() {
    final currentTarget = _pendingFullScreen ?? _fullScreen;
    if (!currentTarget) return;
    request(false, reason: 'exit full screen');
  }

  void request(bool fullScreen, {required String reason}) {
    _serial++;
    _pendingFullScreen = fullScreen;
    fireAndLog(reason, _setFullScreen(fullScreen, _serial));
  }

  Future<void> _setFullScreen(bool fullScreen, int serial) async {
    _controlsTimer?.cancel();
    try {
      if (fullScreen) {
        _rememberWindowedViewportSize();
      }
      // Switch the native window first so the Flutter fullscreen chrome never
      // renders inside the old, non-fullscreen bounds.
      await platformWindow.setFullScreen(fullScreen);
      if (!mounted() || serial != _serial) return;
      if (fullScreen) {
        await _preemptFullScreenViewportResize();
      } else {
        await _preemptWindowedViewportResize();
      }
      if (!mounted() || serial != _serial) return;
      _uiResizePending = true;
      stateStore.setFullScreen(fullScreen);
      await WidgetsBinding.instance.endOfFrame;
      if (!mounted() || serial != _serial) return;
      await _preemptMeasuredViewportResize();
      if (!mounted() || serial != _serial) return;
      if (fullScreen) {
        _scheduleControlsHide();
      }
    } finally {
      if (serial == _serial) {
        _uiResizePending = false;
        _pendingFullScreen = null;
      }
    }
  }

  void _rememberWindowedViewportSize() {
    if (layoutCoordinator.viewportWidth <= 0 ||
        layoutCoordinator.viewportHeight <= 0) {
      return;
    }
    _windowedViewportWidth = layoutCoordinator.viewportWidth;
    _windowedViewportHeight = layoutCoordinator.viewportHeight;
  }

  Future<void> _preemptFullScreenViewportResize() async {
    final dpr = layoutCoordinator.viewportDevicePixelRatio;
    if (dpr <= 0) return;
    final bounds = await platformWindow.getBounds();
    await layoutCoordinator.preemptViewportResize(
      width: (bounds.width * dpr).round(),
      height: (bounds.height * dpr).round(),
    );
  }

  Future<void> _preemptWindowedViewportResize() async {
    final width = _windowedViewportWidth;
    final height = _windowedViewportHeight;
    if (width == null || height == null) return;
    await layoutCoordinator.preemptViewportResize(width: width, height: height);
  }

  Future<void> _preemptMeasuredViewportResize() async {
    final context = viewportKey.currentContext;
    if (context == null) return;
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) return;
    final size = renderObject.size;
    if (size.width <= 0 || size.height <= 0) return;
    final dpr = View.of(context).devicePixelRatio;
    if (dpr <= 0) return;
    await layoutCoordinator.preemptViewportResize(
      width: (size.width * dpr).round(),
      height: (size.height * dpr).round(),
    );
  }

  void showControlsTemporarily() {
    if (!_fullScreen) return;
    stateStore.setFullScreenControlsVisible(true);
    _scheduleControlsHide();
  }

  void setControlsHovering(bool hovering) {
    if (!_fullScreen) return;
    if (hovering) {
      _controlsTimer?.cancel();
      stateStore.setFullScreenControlsVisible(true);
    } else {
      _scheduleControlsHide();
    }
  }

  void _scheduleControlsHide() {
    _controlsTimer?.cancel();
    _controlsTimer = Timer(const Duration(seconds: 1), () {
      if (!_fullScreen || !mounted()) return;
      stateStore.setFullScreenControlsVisible(false);
    });
  }
}
