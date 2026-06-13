import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:window_manager/window_manager.dart' as wm;

import '../app_log.dart';
import '../native_player/native_player_protocol.dart';
import '../widgets/analysis_overlay_controls.dart';
import '../widgets/media_header.dart';
import 'capture_metrics.dart';
import 'win32_native_input.dart';

/// Drives the main window through synthetic pointer events, native mouse
/// injection, and frame captures during UI automation. The main window only
/// supplies the widget handles and timeline getters passed to the constructor.
class MainWindowTestHarness {
  final GlobalKey viewportKey;
  final GlobalKey timelineSliderKey;
  final GlobalKey controlsBarKey;
  final GlobalKey analysisOverlayButtonKey;
  final GlobalKey fullFrameCaptureKey;
  final GlobalKey loopRangeBarKey;
  final double Function() splitPosition;
  final double Function() timelineStartWidth;
  final int Function() effectiveDurationUs;
  final int Function() resolvedLoopStartUs;
  final int Function() resolvedLoopEndUs;

  int _pointerId = 9000;

  MainWindowTestHarness({
    required this.viewportKey,
    required this.timelineSliderKey,
    required this.controlsBarKey,
    required this.analysisOverlayButtonKey,
    required this.fullFrameCaptureKey,
    required this.loopRangeBarKey,
    required this.splitPosition,
    required this.timelineStartWidth,
    required this.effectiveDurationUs,
    required this.resolvedLoopStartUs,
    required this.resolvedLoopEndUs,
  });

  Future<ViewportCapture> captureFlutterFrame({String? outputPath}) async {
    final image = await _captureFlutterFrameImage();
    try {
      final rawData = await image.toByteData(
        format: ui.ImageByteFormat.rawRgba,
      );
      if (rawData == null) {
        throw StateError('Failed to read Flutter frame pixels');
      }
      final rawRgba = rawData.buffer.asUint8List();
      final stats = computeRgbaStats(rawRgba);
      String? resolvedOutputPath;
      if (outputPath != null && outputPath.trim().isNotEmpty) {
        final pngData = await image.toByteData(format: ui.ImageByteFormat.png);
        if (pngData == null) {
          throw StateError('Failed to encode Flutter frame PNG');
        }
        final file = File(outputPath);
        await file.parent.create(recursive: true);
        await file.writeAsBytes(pngData.buffer.asUint8List(), flush: true);
        resolvedOutputPath = outputPath;
      }
      return ViewportCapture(
        hash: computeCaptureHash(rawRgba),
        width: image.width,
        height: image.height,
        avgLuma: stats.avgLuma,
        nonBlackRatio: stats.nonBlackRatio,
        outputPath: resolvedOutputPath,
      );
    } finally {
      image.dispose();
    }
  }

  Future<ui.Image> _captureFlutterFrameImage() async {
    final context = fullFrameCaptureKey.currentContext;
    if (context == null) {
      throw StateError('Flutter frame capture root is not mounted');
    }
    await WidgetsBinding.instance.endOfFrame;
    if (!context.mounted) {
      throw StateError('Flutter frame capture root was unmounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderRepaintBoundary || !renderObject.hasSize) {
      throw StateError('Flutter frame capture root has no repaint boundary');
    }

    final pixelRatio = View.of(context).devicePixelRatio;
    return renderObject.toImage(pixelRatio: pixelRatio);
  }

  void clickTimelineFraction(double fraction) {
    final context = timelineSliderKey.currentContext;
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
    final pointer = _pointerId++;

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

  void hoverControlsBarButtons({int steps = 24}) {
    final context = controlsBarKey.currentContext;
    if (context == null) {
      throw StateError('Controls bar is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Controls bar has no render box');
    }

    final y = renderObject.size.height / 2;
    final startWidth = timelineStartWidth()
        .clamp(0.0, renderObject.size.width)
        .toDouble();
    final buttonBandRight = startWidth > 0
        ? startWidth
        : renderObject.size.width;
    final fixedButtonCenters = <double>[
      100,
      132,
      164,
      196,
      228,
    ].where((x) => x > 0 && x < buttonBandRight).toList(growable: false);
    final count = steps <= 0 ? 1 : steps;
    final points = <Offset>[
      for (var i = 0; i <= count; i++)
        Offset(
          (buttonBandRight * (0.08 + 0.72 * i / count)).clamp(
            1.0,
            renderObject.size.width - 1,
          ),
          y,
        ),
      for (final x in fixedButtonCenters) Offset(x, y),
    ];

    var previousGlobal = renderObject.localToGlobal(points.first);
    log.info(
      'Test action: HOVER_CONTROLS_BAR_BUTTONS steps=$count '
      'points=${points.length} y=${y.toStringAsFixed(1)} '
      'buttonBandRight=${buttonBandRight.toStringAsFixed(1)}',
    );

    for (final local in points) {
      final global = renderObject.localToGlobal(local);
      GestureBinding.instance.handlePointerEvent(
        PointerHoverEvent(
          pointer: _pointerId,
          position: global,
          delta: global - previousGlobal,
          kind: PointerDeviceKind.mouse,
        ),
      );
      previousGlobal = global;
    }
    final exitGlobal = renderObject.localToGlobal(
      Offset(renderObject.size.width - 1, renderObject.size.height + 24),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerHoverEvent(
        pointer: _pointerId,
        position: exitGlobal,
        delta: exitGlobal - previousGlobal,
        kind: PointerDeviceKind.mouse,
      ),
    );
  }

  void clickAnalysisOverlayButton() {
    final context = analysisOverlayButtonKey.currentContext;
    if (context == null) {
      throw StateError('Analysis overlay button is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Analysis overlay button has no render box');
    }

    final local = Offset(
      renderObject.size.width / 2,
      renderObject.size.height / 2,
    );
    final global = renderObject.localToGlobal(local);
    final pointer = _pointerId++;
    log.info(
      'Test action: CLICK_MEDIA_HEADER_OVERLAY_BUTTON '
      'global=(${global.dx.toStringAsFixed(1)}, ${global.dy.toStringAsFixed(1)})',
    );
    GestureBinding.instance.handlePointerEvent(
      PointerAddedEvent(
        pointer: pointer,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: global,
        buttons: kPrimaryButton,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(
        pointer: pointer,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerRemovedEvent(
        pointer: pointer,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
  }

  void clickMediaHeaderRemoveButton(int fileId) {
    final context = _findContextByKey(mediaHeaderRemoveButtonKey(fileId));
    if (context == null) {
      throw StateError(
        'Media header remove button for fileId $fileId is not mounted',
      );
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError(
        'Media header remove button for fileId $fileId has no render box',
      );
    }

    final local = Offset(
      renderObject.size.width / 2,
      renderObject.size.height / 2,
    );
    final global = renderObject.localToGlobal(local);
    final pointer = _pointerId++;
    log.info(
      'Test action: CLICK_MEDIA_HEADER_REMOVE_BUTTON '
      'fileId=$fileId global=(${global.dx.toStringAsFixed(1)}, '
      '${global.dy.toStringAsFixed(1)})',
    );
    GestureBinding.instance.handlePointerEvent(
      PointerAddedEvent(
        pointer: pointer,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: global,
        buttons: kPrimaryButton,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(
        pointer: pointer,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerRemovedEvent(
        pointer: pointer,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
  }

  void hoverAnalysisOverlayButton() {
    final context = analysisOverlayButtonKey.currentContext;
    if (context == null) {
      throw StateError('Analysis overlay button is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Analysis overlay button has no render box');
    }

    final local = Offset(
      renderObject.size.width / 2,
      renderObject.size.height / 2,
    );
    final outside = renderObject.localToGlobal(
      Offset(renderObject.size.width + 24, renderObject.size.height + 24),
    );
    final global = renderObject.localToGlobal(local);
    log.info(
      'Test action: HOVER_MEDIA_HEADER_OVERLAY_BUTTON '
      'global=(${global.dx.toStringAsFixed(1)}, ${global.dy.toStringAsFixed(1)})',
    );
    GestureBinding.instance.handlePointerEvent(
      PointerHoverEvent(
        pointer: _pointerId,
        position: outside,
        kind: PointerDeviceKind.mouse,
      ),
    );
    GestureBinding.instance.handlePointerEvent(
      PointerHoverEvent(
        pointer: _pointerId,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
  }

  void hoverAnalysisOverlayPanelControls() {
    final context = _findContextByKey(analysisOverlayControlBarKey);
    if (context == null) {
      throw StateError('Analysis overlay control bar is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Analysis overlay control bar has no render box');
    }

    final global = renderObject.localToGlobal(
      Offset(
        (renderObject.size.width * 0.08).clamp(
          8.0,
          renderObject.size.width / 2,
        ),
        renderObject.size.height / 2,
      ),
    );
    log.info(
      'Test action: HOVER_MEDIA_HEADER_OVERLAY_PANEL_CONTROLS '
      'global=(${global.dx.toStringAsFixed(1)}, ${global.dy.toStringAsFixed(1)})',
    );
    GestureBinding.instance.handlePointerEvent(
      PointerHoverEvent(
        pointer: _pointerId,
        position: global,
        kind: PointerDeviceKind.mouse,
      ),
    );
  }

  void assertAnalysisOverlayPanelVisible(bool expected) {
    final context = _findContextByKey(analysisOverlayControlBarKey);
    var visible = false;
    if (context != null) {
      final renderObject = context.findRenderObject();
      visible =
          renderObject is RenderBox &&
          renderObject.hasSize &&
          renderObject.size.width > 0 &&
          renderObject.size.height > 0;
    }
    log.info(
      'Test assert: ASSERT_MEDIA_HEADER_OVERLAY_PANEL_VISIBLE '
      'expected=$expected actual=$visible',
    );
    if (visible != expected) {
      throw AssertionError(
        'Expected media-header overlay panel visible=$expected, got $visible',
      );
    }
  }

  BuildContext? _findContextByKey(Key key) {
    final root = WidgetsBinding.instance.rootElement;
    if (root == null) return null;
    Element? found;
    void visit(Element element) {
      if (found != null) return;
      if (element.widget.key == key) {
        found = element;
        return;
      }
      element.visitChildElements(visit);
    }

    visit(root);
    return found;
  }

  Future<void> hoverControlsBarButtonsNative({int steps = 24}) async {
    final context = controlsBarKey.currentContext;
    if (context == null) {
      throw StateError('Controls bar is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Controls bar has no render box');
    }

    final devicePixelRatio = View.of(context).devicePixelRatio;
    final windowPosition = await wm.windowManager.getPosition();
    final y = renderObject.size.height / 2;
    final startWidth = timelineStartWidth()
        .clamp(0.0, renderObject.size.width)
        .toDouble();
    final buttonBandRight = startWidth > 0
        ? startWidth
        : renderObject.size.width;
    final count = steps <= 0 ? 1 : steps;
    final points = <Offset>[
      for (var i = 0; i <= count; i++)
        Offset(
          (buttonBandRight * (0.08 + 0.72 * i / count)).clamp(
            1.0,
            renderObject.size.width - 1,
          ),
          y,
        ),
      for (final x in const <double>[100, 132, 164, 196, 228])
        if (x > 0 && x < buttonBandRight) Offset(x, y),
    ];

    await _moveNativeMouseAcrossPoints(
      renderObject: renderObject,
      points: points,
      windowPosition: windowPosition,
      scale: devicePixelRatio,
      label: 'scaled',
    );
    await _moveNativeMouseAcrossPoints(
      renderObject: renderObject,
      points: points,
      windowPosition: windowPosition,
      scale: 1,
      label: 'unscaled',
    );
  }

  Future<void> clickAnalysisOverlayButtonNative() async {
    final context = analysisOverlayButtonKey.currentContext;
    if (context == null) {
      throw StateError('Analysis overlay button is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Analysis overlay button has no render box');
    }

    final windowPosition = await wm.windowManager.getPosition();
    final local = Offset(
      renderObject.size.width / 2,
      renderObject.size.height / 2,
    );
    await _nativeClickAt(
      renderObject: renderObject,
      local: local,
      windowPosition: windowPosition,
      scale: 1,
      label: 'unscaled',
    );
    await Future<void>.delayed(const Duration(milliseconds: 120));
  }

  Future<void> _nativeClickAt({
    required RenderBox renderObject,
    required Offset local,
    required Offset windowPosition,
    required double scale,
    required String label,
  }) async {
    final appGlobal = renderObject.localToGlobal(local);
    final x = ((windowPosition.dx + appGlobal.dx) * scale).round();
    final y = ((windowPosition.dy + appGlobal.dy) * scale).round();
    log.info(
      'Test action: CLICK_MEDIA_HEADER_OVERLAY_BUTTON_NATIVE $label '
      'screen=($x, $y) window=(${windowPosition.dx.toStringAsFixed(1)}, '
      '${windowPosition.dy.toStringAsFixed(1)}) scale=${scale.toStringAsFixed(2)}',
    );
    nativeSetCursorPos(x, y);
    await Future<void>.delayed(const Duration(milliseconds: 40));
    nativeMouseLeftDown();
    await Future<void>.delayed(const Duration(milliseconds: 40));
    nativeMouseLeftUp();
    await Future<void>.delayed(const Duration(milliseconds: 80));
  }

  Future<void> _moveNativeMouseAcrossPoints({
    required RenderBox renderObject,
    required List<Offset> points,
    required Offset windowPosition,
    required double scale,
    required String label,
  }) async {
    log.info(
      'Test action: HOVER_CONTROLS_BAR_BUTTONS_NATIVE $label '
      'points=${points.length} window=(${windowPosition.dx.toStringAsFixed(1)}, '
      '${windowPosition.dy.toStringAsFixed(1)}) scale=${scale.toStringAsFixed(2)}',
    );
    for (final local in points) {
      final appGlobal = renderObject.localToGlobal(local);
      nativeSetCursorPos(
        ((windowPosition.dx + appGlobal.dx) * scale).round(),
        ((windowPosition.dy + appGlobal.dy) * scale).round(),
      );
      await Future<void>.delayed(const Duration(milliseconds: 18));
    }
    final exitGlobal = renderObject.localToGlobal(
      Offset(renderObject.size.width - 1, renderObject.size.height + 24),
    );
    nativeSetCursorPos(
      ((windowPosition.dx + exitGlobal.dx) * scale).round(),
      ((windowPosition.dy + exitGlobal.dy) * scale).round(),
    );
    await Future<void>.delayed(const Duration(milliseconds: 80));
  }

  void dragSplitHandle(double targetFraction, {int steps = 12}) {
    final context = viewportKey.currentContext;
    if (context == null) {
      throw StateError('Viewport is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Viewport has no render box');
    }

    final startFraction = splitPosition().clamp(0.0, 1.0).toDouble();
    final clampedTarget = targetFraction.clamp(0.0, 1.0).toDouble();
    final y = renderObject.size.height / 2;
    final start = renderObject.localToGlobal(
      Offset(renderObject.size.width * startFraction, y),
    );
    final end = renderObject.localToGlobal(
      Offset(renderObject.size.width * clampedTarget, y),
    );
    final count = steps <= 0 ? 1 : steps;
    final pointer = _pointerId++;
    var previous = start;

    log.info(
      'Test action: DRAG_SPLIT_HANDLE '
      '${startFraction.toStringAsFixed(4)}->${clampedTarget.toStringAsFixed(4)} '
      'steps=$count global=(${start.dx.toStringAsFixed(1)}, ${start.dy.toStringAsFixed(1)})'
      '->(${end.dx.toStringAsFixed(1)}, ${end.dy.toStringAsFixed(1)})',
    );

    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: start,
        buttons: kPrimaryButton,
      ),
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
          buttons: kPrimaryButton,
        ),
      );
      previous = next;
    }
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(pointer: pointer, position: end),
    );
  }

  Future<void> dragViewport(
    Offset delta, {
    int steps = 24,
    Duration stepDelay = const Duration(milliseconds: 16),
  }) async {
    final context = viewportKey.currentContext;
    if (context == null) {
      throw StateError('Viewport is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Viewport has no render box');
    }

    final count = steps <= 0 ? 1 : steps;
    final start = renderObject.localToGlobal(
      Offset(renderObject.size.width / 2, renderObject.size.height / 2),
    );
    final end = start + delta;
    final pointer = _pointerId++;
    var previous = start;

    log.info(
      'Test action: DRAG_VIEWPORT '
      'delta=(${delta.dx.toStringAsFixed(1)}, ${delta.dy.toStringAsFixed(1)}) '
      'steps=$count stepMs=${stepDelay.inMilliseconds} '
      'global=(${start.dx.toStringAsFixed(1)}, ${start.dy.toStringAsFixed(1)})'
      '->(${end.dx.toStringAsFixed(1)}, ${end.dy.toStringAsFixed(1)})',
    );

    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: start,
        buttons: kSecondaryButton,
        kind: PointerDeviceKind.mouse,
      ),
    );
    await Future<void>.delayed(stepDelay);
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
          buttons: kSecondaryButton,
          kind: PointerDeviceKind.mouse,
        ),
      );
      previous = next;
      await Future<void>.delayed(stepDelay);
    }
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(
        pointer: pointer,
        position: end,
        kind: PointerDeviceKind.mouse,
      ),
    );
    await Future<void>.delayed(stepDelay);
  }

  Future<ViewportOverlayDragSampleMetric> dragViewportAndSampleOverlay(
    Offset delta, {
    int steps = 24,
    Duration stepDelay = const Duration(milliseconds: 16),
    double minScoreRatio = 0.45,
    int maxDropSamples = 0,
  }) async {
    final context = viewportKey.currentContext;
    if (context == null) {
      throw StateError('Viewport is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Viewport has no render box');
    }

    final count = steps <= 0 ? 1 : steps;
    final start = renderObject.localToGlobal(
      Offset(renderObject.size.width / 2, renderObject.size.height / 2),
    );
    final end = start + delta;
    final pointer = _pointerId++;
    var previous = start;

    Future<double> sampleScore(String label) async {
      final image = await _captureFlutterFrameImage();
      try {
        final rawData = await image.toByteData(
          format: ui.ImageByteFormat.rawRgba,
        );
        if (rawData == null) {
          throw StateError('Failed to read Flutter frame pixels');
        }
        final score = computeViewportOverlayLineScore(
          rgba: rawData.buffer.asUint8List(),
          imageWidth: image.width,
          imageHeight: image.height,
          viewportBox: renderObject,
          captureRootKey: fullFrameCaptureKey,
        );
        log.info(
          'Test action: DRAG_VIEWPORT_SAMPLE_OVERLAY sample=$label '
          'score=${score.toStringAsFixed(6)}',
        );
        return score;
      } finally {
        image.dispose();
      }
    }

    final baselineScore = await sampleScore('baseline');
    if (baselineScore <= 0) {
      throw AssertionError(
        'Cannot sample overlay line score before drag; baseline=$baselineScore',
      );
    }

    final scores = <double>[];
    log.info(
      'Test action: DRAG_VIEWPORT_SAMPLE_OVERLAY '
      'delta=(${delta.dx.toStringAsFixed(1)}, ${delta.dy.toStringAsFixed(1)}) '
      'steps=$count stepMs=${stepDelay.inMilliseconds} '
      'minScoreRatio=$minScoreRatio maxDropSamples=$maxDropSamples '
      'global=(${start.dx.toStringAsFixed(1)}, ${start.dy.toStringAsFixed(1)})'
      '->(${end.dx.toStringAsFixed(1)}, ${end.dy.toStringAsFixed(1)})',
    );

    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: start,
        buttons: kSecondaryButton,
        kind: PointerDeviceKind.mouse,
      ),
    );
    await Future<void>.delayed(stepDelay);
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
          buttons: kSecondaryButton,
          kind: PointerDeviceKind.mouse,
        ),
      );
      previous = next;
      await Future<void>.delayed(stepDelay);
      scores.add(await sampleScore('$i/$count'));
    }
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(
        pointer: pointer,
        position: end,
        kind: PointerDeviceKind.mouse,
      ),
    );
    await Future<void>.delayed(stepDelay);
    return ViewportOverlayDragSampleMetric.fromScores(
      baselineScore: baselineScore,
      scores: scores,
      minScoreRatio: minScoreRatio,
      maxDropSamples: maxDropSamples,
    );
  }

  Future<int> dragViewportAndSample(
    Offset delta, {
    int steps = 24,
    Duration stepDelay = const Duration(milliseconds: 16),
    required Future<bool> Function(String label) sample,
  }) async {
    final context = viewportKey.currentContext;
    if (context == null) {
      throw StateError('Viewport is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Viewport has no render box');
    }

    final count = steps <= 0 ? 1 : steps;
    final start = renderObject.localToGlobal(
      Offset(renderObject.size.width / 2, renderObject.size.height / 2),
    );
    final end = start + delta;
    final pointer = _pointerId++;
    var previous = start;
    var matches = 0;

    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: start,
        buttons: kSecondaryButton,
        kind: PointerDeviceKind.mouse,
      ),
    );
    await Future<void>.delayed(stepDelay);
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
          buttons: kSecondaryButton,
          kind: PointerDeviceKind.mouse,
        ),
      );
      previous = next;
      await Future<void>.delayed(stepDelay);
      if (await sample('$i/$count')) {
        matches++;
      }
    }
    GestureBinding.instance.handlePointerEvent(
      PointerUpEvent(
        pointer: pointer,
        position: end,
        kind: PointerDeviceKind.mouse,
      ),
    );
    await Future<void>.delayed(stepDelay);
    return matches;
  }

  void dragLoopHandle(String handle, int targetUs, {int steps = 12}) {
    final context = loopRangeBarKey.currentContext;
    if (context == null) {
      throw StateError('Loop range bar is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Loop range bar has no render box');
    }
    final durationUs = effectiveDurationUs();
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
    final timelineLeft = timelineStartWidth();
    final drawableWidth = renderObject.size.width - timelineLeft - margin * 2;
    if (drawableWidth <= 0) {
      throw StateError('Loop range timeline has no drawable width');
    }

    final startUs = resolvedLoopStartUs();
    final endUs = resolvedLoopEndUs();
    final minRangeUs = durationUs > 10000 ? 10000 : 0;
    final currentUs = isEnd ? endUs : startUs;
    final clampedTargetUs =
        (isEnd
                ? targetUs.clamp(startUs + minRangeUs, durationUs)
                : targetUs.clamp(0, endUs - minRangeUs))
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
    final pointer = _pointerId++;
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
}
