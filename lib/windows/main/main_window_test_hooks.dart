import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../../app_log.dart';

class MainWindowTestHarness {
  final GlobalKey viewportKey;
  final GlobalKey timelineSliderKey;
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
    required this.loopRangeBarKey,
    required this.splitPosition,
    required this.timelineStartWidth,
    required this.effectiveDurationUs,
    required this.resolvedLoopStartUs,
    required this.resolvedLoopEndUs,
  });

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
