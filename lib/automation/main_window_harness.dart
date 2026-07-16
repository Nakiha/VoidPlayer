import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:window_manager/window_manager.dart' as wm;

import '../app_log.dart';
import '../l10n/app_localizations.dart';
import '../native_player/native_player_protocol.dart';
import '../widgets/analysis_overlay_controls.dart';
import '../widgets/media_header.dart';
import 'capture_metrics.dart';
import 'win32_native_input.dart';
import 'windows_axtree_probe.dart';

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
    WidgetsBinding.instance.scheduleFrame();
    await WidgetsBinding.instance.endOfFrame.timeout(
      const Duration(seconds: 2),
      onTimeout: () => throw TimeoutException(
        'Timed out waiting for a Flutter frame capture boundary',
      ),
    );
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

  void clickFlutterPoint(Offset point) {
    final context = fullFrameCaptureKey.currentContext;
    if (context == null) {
      throw StateError('Flutter frame capture root is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Flutter frame capture root has no render box');
    }

    final clamped = Offset(
      point.dx.clamp(0.0, renderObject.size.width - 1).toDouble(),
      point.dy.clamp(0.0, renderObject.size.height - 1).toDouble(),
    );
    final global = renderObject.localToGlobal(clamped);
    final pointer = _pointerId++;
    log.info(
      'Test action: CLICK_FLUTTER_POINT '
      'local=(${clamped.dx.toStringAsFixed(1)}, ${clamped.dy.toStringAsFixed(1)}) '
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

  void clickControlsPlayButton() {
    final context = controlsBarKey.currentContext;
    if (context == null) {
      throw StateError('Controls bar is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Controls bar has no render box');
    }

    final startWidth = timelineStartWidth()
        .clamp(0.0, renderObject.size.width)
        .toDouble();
    final playXMax = (startWidth - 1)
        .clamp(1.0, renderObject.size.width)
        .toDouble();
    final local = Offset(
      164.0.clamp(1.0, playXMax).toDouble(),
      renderObject.size.height / 2,
    );
    final global = renderObject.localToGlobal(local);
    final pointer = _pointerId++;
    log.info(
      'Test action: CLICK_CONTROLS_PLAY_BUTTON '
      'local=(${local.dx.toStringAsFixed(1)}, ${local.dy.toStringAsFixed(1)}) '
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

  Future<void> hoverTimeline({int steps = 48, int stepMs = 8}) async {
    final context = timelineSliderKey.currentContext;
    if (context == null) {
      throw StateError('Timeline slider is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Timeline slider has no render box');
    }

    final count = steps <= 0 ? 1 : steps;
    final y = renderObject.size.height / 2;
    final left = math.min(6.0, renderObject.size.width / 2);
    final right = math.max(left, renderObject.size.width - 6.0);
    var previousGlobal = renderObject.localToGlobal(Offset(left, y));
    log.info(
      'Test action: HOVER_TIMELINE steps=$count stepMs=$stepMs '
      'width=${renderObject.size.width.toStringAsFixed(1)} '
      'y=${y.toStringAsFixed(1)}',
    );

    for (var i = 0; i <= count; i++) {
      final t = i / count;
      final local = Offset(left + (right - left) * t, y);
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
      if (stepMs > 0) {
        await Future<void>.delayed(Duration(milliseconds: stepMs));
      }
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

  Future<void> invokeWindowsAxAction(String actionName) =>
      invokeWindowsAxTreeAction(actionName.trim());

  Future<void> pressKeyNative(String key) async {
    final normalized = key.trim().toLowerCase();
    final virtualKey = switch (normalized) {
      'space' => 0x20,
      'left' || 'arrowleft' => 0x25,
      'right' || 'arrowright' => 0x27,
      'escape' || 'esc' => 0x1b,
      'f11' => 0x7a,
      'm' => 0x4d,
      'o' => 0x4f,
      _ => throw ArgumentError.value(key, 'key', 'unsupported native key'),
    };
    log.info(
      'Test action: PRESS_KEY_NATIVE key=$normalized '
      'vk=0x${virtualKey.toRadixString(16)}',
    );
    await wm.windowManager.focus();
    await Future<void>.delayed(const Duration(milliseconds: 80));
    nativeKeyDown(virtualKey);
    await Future<void>.delayed(const Duration(milliseconds: 40));
    nativeKeyUp(virtualKey);
    await Future<void>.delayed(const Duration(milliseconds: 120));
  }

  Future<void> clickToolbarMediaInfoNative() async {
    final root = WidgetsBinding.instance.rootElement;
    if (root == null) {
      throw StateError('Flutter root element is not mounted');
    }
    final localizationContext = controlsBarKey.currentContext ?? root;
    final l = AppLocalizations.of(localizationContext)!;
    final context = _findTooltipContext(l.mediaInfo);
    if (context == null) {
      throw StateError('Toolbar media info button is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Toolbar media info button has no render box');
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
      label: 'toolbar-media-info',
    );
    await Future<void>.delayed(const Duration(milliseconds: 120));
  }

  BuildContext? _findTooltipContext(String message) {
    final root = WidgetsBinding.instance.rootElement;
    if (root == null) return null;
    Element? found;
    void visit(Element element) {
      if (found != null) return;
      final widget = element.widget;
      if (widget is Tooltip && widget.message == message) {
        found = element;
        return;
      }
      element.visitChildElements(visit);
    }

    visit(root);
    return found;
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
      'Test action: NATIVE_CLICK $label '
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

  Offset _nativeScreenPoint({
    required RenderBox renderObject,
    required Offset local,
    required Offset windowPosition,
    required double scale,
  }) {
    final appGlobal = renderObject.localToGlobal(local);
    return Offset(
      ((windowPosition.dx + appGlobal.dx) * scale).roundToDouble(),
      ((windowPosition.dy + appGlobal.dy) * scale).roundToDouble(),
    );
  }

  Future<void> _nativeDragAt({
    required RenderBox renderObject,
    required Offset startLocal,
    required Offset endLocal,
    required Offset windowPosition,
    required double scale,
    required String label,
    required int steps,
    required Duration stepDelay,
    required String button,
  }) async {
    final count = steps <= 0 ? 1 : steps;
    final normalizedButton = button.toLowerCase();
    final useSecondary =
        normalizedButton == 'secondary' ||
        normalizedButton == 'right' ||
        normalizedButton == '2';
    final startScreen = _nativeScreenPoint(
      renderObject: renderObject,
      local: startLocal,
      windowPosition: windowPosition,
      scale: scale,
    );
    final endScreen = _nativeScreenPoint(
      renderObject: renderObject,
      local: endLocal,
      windowPosition: windowPosition,
      scale: scale,
    );
    log.info(
      'Test action: NATIVE_DRAG $label '
      'button=${useSecondary ? "secondary" : "primary"} '
      'steps=$count stepMs=${stepDelay.inMilliseconds} '
      'local=(${startLocal.dx.toStringAsFixed(1)}, '
      '${startLocal.dy.toStringAsFixed(1)})'
      '->(${endLocal.dx.toStringAsFixed(1)}, '
      '${endLocal.dy.toStringAsFixed(1)}) '
      'screen=(${startScreen.dx.toStringAsFixed(0)}, '
      '${startScreen.dy.toStringAsFixed(0)})'
      '->(${endScreen.dx.toStringAsFixed(0)}, '
      '${endScreen.dy.toStringAsFixed(0)}) '
      'window=(${windowPosition.dx.toStringAsFixed(1)}, '
      '${windowPosition.dy.toStringAsFixed(1)}) scale=${scale.toStringAsFixed(2)}',
    );

    nativeSetCursorPos(startScreen.dx.round(), startScreen.dy.round());
    await Future<void>.delayed(const Duration(milliseconds: 80));
    if (useSecondary) {
      nativeMouseRightDown();
    } else {
      nativeMouseLeftDown();
    }
    await Future<void>.delayed(stepDelay);
    for (var i = 1; i <= count; i++) {
      final t = i / count;
      final next = Offset(
        startScreen.dx + (endScreen.dx - startScreen.dx) * t,
        startScreen.dy + (endScreen.dy - startScreen.dy) * t,
      );
      nativeSetCursorPos(next.dx.round(), next.dy.round());
      await Future<void>.delayed(stepDelay);
    }
    if (useSecondary) {
      nativeMouseRightUp();
    } else {
      nativeMouseLeftUp();
    }
    await Future<void>.delayed(const Duration(milliseconds: 120));
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

  Future<void> dragSplitHandle(
    double targetFraction, {
    int steps = 12,
    int stepMs = 16,
  }) async {
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
    final stepDelay = Duration(milliseconds: stepMs);
    final pointer = _pointerId++;
    var previous = start;

    log.info(
      'Test action: DRAG_SPLIT_HANDLE '
      '${startFraction.toStringAsFixed(4)}->${clampedTarget.toStringAsFixed(4)} '
      'steps=$count stepMs=${stepDelay.inMilliseconds} '
      'global=(${start.dx.toStringAsFixed(1)}, ${start.dy.toStringAsFixed(1)})'
      '->(${end.dx.toStringAsFixed(1)}, ${end.dy.toStringAsFixed(1)})',
    );

    GestureBinding.instance.handlePointerEvent(
      PointerDownEvent(
        pointer: pointer,
        position: start,
        buttons: kPrimaryButton,
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
          buttons: kPrimaryButton,
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

  Future<void> dragSplitHandleNative(
    double targetFraction, {
    int steps = 12,
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

    final startFraction = splitPosition().clamp(0.0, 1.0).toDouble();
    final clampedTarget = targetFraction.clamp(0.0, 1.0).toDouble();
    final y = renderObject.size.height / 2;
    final start = Offset(renderObject.size.width * startFraction, y);
    final end = Offset(renderObject.size.width * clampedTarget, y);
    final devicePixelRatio = View.of(context).devicePixelRatio;
    final windowPosition = await wm.windowManager.getPosition();
    await _nativeDragAt(
      renderObject: renderObject,
      startLocal: start,
      endLocal: end,
      windowPosition: windowPosition,
      scale: devicePixelRatio,
      label: 'split-handle',
      steps: steps,
      stepDelay: stepDelay,
      button: 'primary',
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

  Future<void> panZoomViewport({
    required Offset panDelta,
    required double scale,
    int steps = 24,
    Duration stepDelay = const Duration(milliseconds: 8),
    double xFraction = 0.5,
    double yFraction = 0.5,
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
    final local = Offset(
      renderObject.size.width * xFraction.clamp(0.0, 1.0),
      renderObject.size.height * yFraction.clamp(0.0, 1.0),
    );
    final position = renderObject.localToGlobal(local);
    final pointer = _pointerId++;
    final safeScale = scale > 0 && scale.isFinite ? scale : 1.0;

    log.info(
      'Test action: PAN_ZOOM_VIEWPORT '
      'pan=(${panDelta.dx.toStringAsFixed(1)}, ${panDelta.dy.toStringAsFixed(1)}) '
      'scale=${safeScale.toStringAsFixed(4)} steps=$count '
      'stepMs=${stepDelay.inMilliseconds} '
      'global=(${position.dx.toStringAsFixed(1)}, ${position.dy.toStringAsFixed(1)})',
    );

    GestureBinding.instance.handlePointerEvent(
      PointerPanZoomStartEvent(pointer: pointer, position: position),
    );
    await Future<void>.delayed(stepDelay);
    for (var i = 1; i <= count; i++) {
      final t = i / count;
      final stepPan = Offset(panDelta.dx / count, panDelta.dy / count);
      final cumulativePan = Offset(panDelta.dx * t, panDelta.dy * t);
      final cumulativeScale = safeScale == 1.0
          ? 1.0
          : math.exp(math.log(safeScale) * t);
      GestureBinding.instance.handlePointerEvent(
        PointerPanZoomUpdateEvent(
          pointer: pointer,
          position: position,
          pan: cumulativePan,
          panDelta: stepPan,
          scale: cumulativeScale,
        ),
      );
      await Future<void>.delayed(stepDelay);
    }
    GestureBinding.instance.handlePointerEvent(
      PointerPanZoomEndEvent(pointer: pointer, position: position),
    );
  }

  Future<void> dragViewportNative(
    Offset delta, {
    int steps = 24,
    Duration stepDelay = const Duration(milliseconds: 16),
    String button = 'secondary',
  }) async {
    final context = viewportKey.currentContext;
    if (context == null) {
      throw StateError('Viewport is not mounted');
    }
    final renderObject = context.findRenderObject();
    if (renderObject is! RenderBox || !renderObject.hasSize) {
      throw StateError('Viewport has no render box');
    }

    final start = Offset(
      renderObject.size.width / 2,
      renderObject.size.height / 2,
    );
    final end = start + delta;
    final devicePixelRatio = View.of(context).devicePixelRatio;
    final windowPosition = await wm.windowManager.getPosition();
    await _nativeDragAt(
      renderObject: renderObject,
      startLocal: start,
      endLocal: end,
      windowPosition: windowPosition,
      scale: devicePixelRatio,
      label: 'viewport',
      steps: steps,
      stepDelay: stepDelay,
      button: button,
    );
  }

  Future<void> wheelViewportNative({
    required int delta,
    int steps = 1,
    Duration stepDelay = const Duration(milliseconds: 16),
    double xFraction = 0.5,
    double yFraction = 0.5,
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
    final clampedX = xFraction.clamp(0.0, 1.0).toDouble();
    final clampedY = yFraction.clamp(0.0, 1.0).toDouble();
    final local = Offset(
      renderObject.size.width * clampedX,
      renderObject.size.height * clampedY,
    );
    final devicePixelRatio = View.of(context).devicePixelRatio;
    final windowPosition = await wm.windowManager.getPosition();
    final screen = _nativeScreenPoint(
      renderObject: renderObject,
      local: local,
      windowPosition: windowPosition,
      scale: devicePixelRatio,
    );
    log.info(
      'Test action: WHEEL_VIEWPORT_NATIVE delta=$delta steps=$count '
      'stepMs=${stepDelay.inMilliseconds} '
      'local=(${local.dx.toStringAsFixed(1)}, '
      '${local.dy.toStringAsFixed(1)}) '
      'screen=(${screen.dx.toStringAsFixed(0)}, '
      '${screen.dy.toStringAsFixed(0)}) '
      'window=(${windowPosition.dx.toStringAsFixed(1)}, '
      '${windowPosition.dy.toStringAsFixed(1)}) '
      'scale=${devicePixelRatio.toStringAsFixed(2)}',
    );
    nativeSetCursorPos(screen.dx.round(), screen.dy.round());
    await Future<void>.delayed(const Duration(milliseconds: 80));
    for (var i = 0; i < count; i++) {
      nativeMouseWheel(delta);
      await Future<void>.delayed(stepDelay);
    }
    await Future<void>.delayed(const Duration(milliseconds: 120));
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
