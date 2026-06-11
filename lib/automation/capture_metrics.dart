import 'dart:math' as math;
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';

/// Pixel-level metrics computed over captured Flutter frames during UI
/// automation: luma statistics, capture hashes, and overlay edge scores.
class RgbaCaptureStats {
  final double avgLuma;
  final double nonBlackRatio;

  const RgbaCaptureStats({required this.avgLuma, required this.nonBlackRatio});
}

class ViewportOverlayDragSampleMetric {
  final double baselineScore;
  final double minScore;
  final double avgScore;
  final double minRatio;
  final int samples;
  final int dropSamples;
  final double minScoreRatio;
  final int maxDropSamples;

  const ViewportOverlayDragSampleMetric({
    required this.baselineScore,
    required this.minScore,
    required this.avgScore,
    required this.minRatio,
    required this.samples,
    required this.dropSamples,
    required this.minScoreRatio,
    required this.maxDropSamples,
  });

  factory ViewportOverlayDragSampleMetric.fromScores({
    required double baselineScore,
    required List<double> scores,
    required double minScoreRatio,
    required int maxDropSamples,
  }) {
    final samples = scores.length;
    final minScore = scores.isEmpty ? 0.0 : scores.reduce(math.min);
    final avgScore = scores.isEmpty
        ? 0.0
        : scores.reduce((a, b) => a + b) / samples;
    final minRatio = baselineScore <= 0 ? 0.0 : minScore / baselineScore;
    final dropSamples = scores
        .where(
          (score) => baselineScore > 0 && score / baselineScore < minScoreRatio,
        )
        .length;
    return ViewportOverlayDragSampleMetric(
      baselineScore: baselineScore,
      minScore: minScore,
      avgScore: avgScore,
      minRatio: minRatio,
      samples: samples,
      dropSamples: dropSamples,
      minScoreRatio: minScoreRatio,
      maxDropSamples: maxDropSamples,
    );
  }

  bool get stable => dropSamples <= maxDropSamples;

  String get failureMessage =>
      'Viewport overlay line score dropped during drag: '
      'baseline=${baselineScore.toStringAsFixed(6)} '
      'min=${minScore.toStringAsFixed(6)} '
      'avg=${avgScore.toStringAsFixed(6)} '
      'minRatio=${minRatio.toStringAsFixed(3)} '
      'dropSamples=$dropSamples/$samples '
      'thresholdRatio=$minScoreRatio maxDropSamples=$maxDropSamples';

  String summary() =>
      'DRAG_VIEWPORT_SAMPLE_OVERLAY summary: '
      'baseline=${baselineScore.toStringAsFixed(6)} '
      'min=${minScore.toStringAsFixed(6)} '
      'avg=${avgScore.toStringAsFixed(6)} '
      'minRatio=${minRatio.toStringAsFixed(3)} '
      'dropSamples=$dropSamples/$samples '
      'thresholdRatio=$minScoreRatio maxDropSamples=$maxDropSamples';
}

RgbaCaptureStats computeRgbaStats(Uint8List rgba) {
  final pixelCount = rgba.length ~/ 4;
  if (pixelCount == 0) {
    return const RgbaCaptureStats(avgLuma: 0, nonBlackRatio: 0);
  }

  var lumaSum = 0;
  var nonBlack = 0;
  for (var i = 0; i < pixelCount; i++) {
    final off = i * 4;
    final r = rgba[off];
    final g = rgba[off + 1];
    final b = rgba[off + 2];
    final luma = (77 * r + 150 * g + 29 * b) >> 8;
    lumaSum += luma;
    if (r > 8 || g > 8 || b > 8) nonBlack++;
  }

  return RgbaCaptureStats(
    avgLuma: lumaSum / pixelCount,
    nonBlackRatio: nonBlack / pixelCount,
  );
}

String computeCaptureHash(Uint8List bytes) =>
    sha256.convert(bytes).toString().substring(0, 16);

double computeViewportOverlayLineScore({
  required Uint8List rgba,
  required int imageWidth,
  required int imageHeight,
  required RenderBox viewportBox,
  required GlobalKey captureRootKey,
}) {
  final rootContext = captureRootKey.currentContext;
  final rootObject = rootContext?.findRenderObject();
  if (rootContext == null ||
      rootObject is! RenderRepaintBoundary ||
      !rootObject.hasSize) {
    throw StateError('Flutter frame capture root is not mounted');
  }
  if (imageWidth <= 2 || imageHeight <= 2) {
    return 0.0;
  }

  final rootTopLeft = rootObject.localToGlobal(Offset.zero);
  final viewportTopLeft = viewportBox.localToGlobal(Offset.zero);
  final pixelRatio = imageWidth / rootObject.size.width;
  final x0 = ((viewportTopLeft.dx - rootTopLeft.dx) * pixelRatio).floor().clamp(
    0,
    imageWidth - 2,
  );
  final y0 = ((viewportTopLeft.dy - rootTopLeft.dy) * pixelRatio).floor().clamp(
    0,
    imageHeight - 2,
  );
  final x1 =
      ((viewportTopLeft.dx - rootTopLeft.dx + viewportBox.size.width) *
              pixelRatio)
          .ceil()
          .clamp(x0 + 1, imageWidth - 1);
  final y1 =
      ((viewportTopLeft.dy - rootTopLeft.dy + viewportBox.size.height) *
              pixelRatio)
          .ceil()
          .clamp(y0 + 1, imageHeight - 1);

  const sampleStep = 2;
  const edgeThreshold = 42;
  var strongEdges = 0;
  var samples = 0;

  int lumaAt(int x, int y) {
    final off = (y * imageWidth + x) * 4;
    final r = rgba[off];
    final g = rgba[off + 1];
    final b = rgba[off + 2];
    return (77 * r + 150 * g + 29 * b) >> 8;
  }

  for (var y = y0; y < y1 - 1; y += sampleStep) {
    for (var x = x0; x < x1 - 1; x += sampleStep) {
      final luma = lumaAt(x, y);
      final right = lumaAt(x + 1, y);
      final down = lumaAt(x, y + 1);
      if (math.max((luma - right).abs(), (luma - down).abs()) >=
          edgeThreshold) {
        strongEdges++;
      }
      samples++;
    }
  }

  return samples == 0 ? 0.0 : strongEdges / samples;
}
