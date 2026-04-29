import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:window_manager/window_manager.dart' as wm;

import '../app_log.dart';
import 'action_registry.dart';
import '../video_renderer_controller.dart';
import '../windows/window_manager.dart';
import 'player_action.dart';
import 'player_assert.dart';

/// A parsed instruction from a test script, with its scheduled time.
sealed class ScriptInstruction {
  final Duration time;
  const ScriptInstruction(this.time);
}

class ScriptAction extends ScriptInstruction {
  final PlayerAction action;
  const ScriptAction(super.time, this.action);
}

class ScriptAssert extends ScriptInstruction {
  final PlayerAssert assertion;
  const ScriptAssert(super.time, this.assertion);
}

class ScriptWait extends ScriptInstruction {
  final WaitState state;
  final Duration timeout;
  const ScriptWait(super.time, this.state, this.timeout);
}

enum WaitState { playing, paused }

class ScriptWaitAnalysisProcessCount extends ScriptInstruction {
  final int count;
  final Duration timeout;
  const ScriptWaitAnalysisProcessCount(super.time, this.count, this.timeout);
}

class ScriptSetAnalysisTestScript extends ScriptInstruction {
  final String path;
  const ScriptSetAnalysisTestScript(super.time, this.path);
}

class ScriptQuit extends ScriptInstruction {
  final int exitCode;
  const ScriptQuit(super.time, this.exitCode);
}

class _ViewCenterMetric {
  final double x;
  final double y;
  const _ViewCenterMetric(this.x, this.y);
}

class _ResourceUsageMetric {
  final int rssBytes;
  final int dedicatedGpuBytes;
  const _ResourceUsageMetric({
    required this.rssBytes,
    required this.dedicatedGpuBytes,
  });
}

/// Parses a test script file and runs instructions on a timeline.
class TestRunner {
  final String scriptPath;
  final VideoRendererController controller;
  final _captures = <String, ViewportCapture>{};
  final _viewCenterBaselines = <String, _ViewCenterMetric>{};
  final _resourceBaselines = <String, _ResourceUsageMetric>{};
  final _nativeSeekCountBaselines = <String, int>{};

  TestRunner({required this.scriptPath, required this.controller});

  /// Parse and execute the test script. Exits the process on QUIT or failure.
  Future<void> run() async {
    final instructions = _parseScript(scriptPath);
    if (instructions.isEmpty) {
      log.severe('Test script is empty: $scriptPath');
      exit(1);
    }

    log.info(
      'TestRunner: running ${instructions.length} instructions from $scriptPath',
    );

    final sw = Stopwatch()..start();

    for (final instr in instructions) {
      final waitMs = instr.time.inMilliseconds - sw.elapsedMilliseconds;
      if (waitMs > 0) {
        await Future.delayed(Duration(milliseconds: waitMs));
      }

      try {
        await _execute(instr);
      } catch (e) {
        log.severe('TestRunner FAIL at ${instr.time}: $e');
        exit(1);
      }
    }

    // If we reach here without a QUIT instruction, that's an error.
    log.severe('TestRunner: script ended without QUIT instruction');
    exit(1);
  }

  Future<void> _execute(ScriptInstruction instr) async {
    switch (instr) {
      case ScriptAction(:final action):
        await _executeAction(action);

      case ScriptAssert(:final assertion):
        log.info('TestRunner ${instr.time}: assert ${assertion.runtimeType}');
        await _executeAssert(assertion);

      case ScriptWait(:final state, :final timeout):
        log.info(
          'TestRunner ${instr.time}: WAIT_${state.name.toUpperCase()} ${timeout.inMilliseconds}ms',
        );
        await _executeWait(state, timeout);

      case ScriptWaitAnalysisProcessCount(:final count, :final timeout):
        log.info(
          'TestRunner ${instr.time}: WAIT_ANALYSIS_PROCESS_COUNT $count ${timeout.inMilliseconds}ms',
        );
        final ok = await WindowManager.waitForAnalysisProcessCount(
          count,
          timeout,
        );
        if (!ok) {
          throw AssertionError(
            'Expected $count analysis process(es), got '
            '${WindowManager.analysisProcessCount}; exits=${WindowManager.analysisExitCodes}',
          );
        }

      case ScriptSetAnalysisTestScript(:final path):
        log.info('TestRunner ${instr.time}: SET_ANALYSIS_TEST_SCRIPT $path');
        WindowManager.analysisTestScriptPath = path;

      case ScriptQuit(:final exitCode):
        log.info('TestRunner ${instr.time}: QUIT $exitCode');
        await WindowManager.closeAllAnalysisWindows();
        exit(exitCode);
    }
  }

  Future<void> _executeAction(PlayerAction action) async {
    switch (action) {
      case SetRenderSize(:final width, :final height):
        log.info('TestRunner: SET_RENDER_SIZE ${width}x$height');
        await controller.resize(width, height);
      case CaptureViewportAction(:final nameId, :final outputPath):
        final capture = await controller.captureViewport(
          outputPath: outputPath,
        );
        _captures[nameId] = capture;
        log.info(
          'TestRunner: CAPTURE_VIEWPORT $nameId hash=${capture.hash} ${capture.width}x${capture.height}'
          ' avgLuma=${capture.avgLuma.toStringAsFixed(2)}'
          ' nonBlack=${capture.nonBlackRatio.toStringAsFixed(4)}'
          '${capture.outputPath != null ? ' -> ${capture.outputPath}' : ''}',
        );
      case WindowMaximize():
        log.info('TestRunner: WINDOW_MAXIMIZE');
        await wm.windowManager.maximize();
      case WindowRestore():
        log.info('TestRunner: WINDOW_RESTORE');
        await wm.windowManager.restore();
      case StoreViewCenter(:final nameId):
        final metric = await _currentViewCenterMetric();
        _viewCenterBaselines[nameId] = metric;
        log.info(
          'TestRunner: STORE_VIEW_CENTER $nameId '
          'normalized=(${metric.x.toStringAsFixed(6)}, ${metric.y.toStringAsFixed(6)})',
        );
      case StoreResourceUsage(:final nameId):
        final metric = await _currentResourceUsageMetric();
        _resourceBaselines[nameId] = metric;
        log.info(
          'TestRunner: STORE_RESOURCE_USAGE $nameId '
          'rss=${_formatMb(metric.rssBytes)}MB '
          'dedicatedGpu=${_formatMb(metric.dedicatedGpuBytes)}MB',
        );
      case StoreNativeSeekCount(:final nameId):
        final count = _currentNativeSeekCount();
        _nativeSeekCountBaselines[nameId] = count;
        log.info('TestRunner: STORE_NATIVE_SEEK_COUNT $nameId count=$count');
      default:
        actionRegistry.execute(action.name, action);
    }
  }

  Future<void> _executeAssert(PlayerAssert assertion) async {
    switch (assertion) {
      case AssertPlaying():
        if (!await controller.isPlaying()) {
          throw AssertionError('Expected PLAYING, but isPlaying=false');
        }
      case AssertPaused():
        if (await controller.isPlaying()) {
          throw AssertionError('Expected PAUSED, but isPlaying=true');
        }
      case AssertPosition(:final ptsUs, :final toleranceMs):
        final actual = await controller.currentPts();
        final diff = (actual - ptsUs).abs();
        if (diff > toleranceMs * 1000) {
          throw AssertionError(
            'Expected position $ptsUs μs (±${toleranceMs}ms), got $actual μs (diff ${diff ~/ 1000}ms)',
          );
        }
      case AssertPositionRange(:final minUs, :final maxUs):
        final actual = await controller.currentPts();
        if (actual < minUs || actual > maxUs) {
          throw AssertionError(
            'Expected position in [$minUs, $maxUs] μs, got $actual μs',
          );
        }
      case AssertTrackCount(:final count):
        final tracks = await controller.getTracks();
        if (tracks.length != count) {
          throw AssertionError(
            'Expected track count $count, got ${tracks.length}',
          );
        }
      case AssertDuration(:final ptsUs, :final toleranceMs):
        final actual = await controller.duration();
        final diff = (actual - ptsUs).abs();
        if (diff > toleranceMs * 1000) {
          throw AssertionError(
            'Expected duration $ptsUs μs (±${toleranceMs}ms), got $actual μs',
          );
        }
      case AssertLayoutMode(:final mode):
        final layout = await controller.getLayout();
        if (layout.mode != mode) {
          throw AssertionError(
            'Expected layout mode $mode, got ${layout.mode}',
          );
        }
      case AssertZoom(:final ratio, :final tolerance):
        final layout = await controller.getLayout();
        if ((layout.zoomRatio - ratio).abs() > tolerance) {
          throw AssertionError(
            'Expected zoom $ratio (±$tolerance), got ${layout.zoomRatio}',
          );
        }
      case AssertSplitPos(:final position, :final tolerance):
        final layout = await controller.getLayout();
        if ((layout.splitPos - position).abs() > tolerance) {
          throw AssertionError(
            'Expected split position $position (±$tolerance), got ${layout.splitPos}',
          );
        }
      case AssertViewOffset(:final x, :final y, :final tolerance):
        final layout = await controller.getLayout();
        final dx = (layout.viewOffsetX - x).abs();
        final dy = (layout.viewOffsetY - y).abs();
        if (dx > tolerance || dy > tolerance) {
          throw AssertionError(
            'Expected view offset ($x, $y) (±$tolerance), '
            'got (${layout.viewOffsetX}, ${layout.viewOffsetY})',
          );
        }
      case AssertViewCenterStable(:final baseline, :final tolerance):
        final expected = _viewCenterBaselines[baseline];
        if (expected == null) {
          throw AssertionError(
            'Missing baseline for ASSERT_VIEW_CENTER_STABLE: $baseline',
          );
        }
        final actual = await _currentViewCenterMetric();
        final dx = (actual.x - expected.x).abs();
        final dy = (actual.y - expected.y).abs();
        if (dx > tolerance || dy > tolerance) {
          throw AssertionError(
            'Expected normalized view center to match $baseline '
            '(±$tolerance), got '
            '(${actual.x.toStringAsFixed(6)}, ${actual.y.toStringAsFixed(6)}) '
            'vs (${expected.x.toStringAsFixed(6)}, ${expected.y.toStringAsFixed(6)})',
          );
        }
      case AssertCaptureEquals(:final expectedCapture, :final actualCapture):
        final expected = _captures[expectedCapture];
        final actual = _captures[actualCapture];
        if (expected == null || actual == null) {
          throw AssertionError(
            'Missing capture(s) for ASSERT_CAPTURE_EQUALS: $expectedCapture / $actualCapture',
          );
        }
        if (expected.hash != actual.hash) {
          throw AssertionError(
            'Expected capture $actualCapture to equal $expectedCapture, '
            'got ${actual.hash} != ${expected.hash}',
          );
        }
      case AssertCaptureChanged(:final beforeCapture, :final afterCapture):
        final before = _captures[beforeCapture];
        final after = _captures[afterCapture];
        if (before == null || after == null) {
          throw AssertionError(
            'Missing capture(s) for ASSERT_CAPTURE_CHANGED: $beforeCapture / $afterCapture',
          );
        }
        if (before.hash == after.hash) {
          throw AssertionError(
            'Expected capture $afterCapture to differ from $beforeCapture, '
            'but both hashes are ${before.hash}',
          );
        }
      case AssertCaptureHash(:final capture, :final hash):
        final actual = _captures[capture];
        if (actual == null) {
          throw AssertionError(
            'Missing capture for ASSERT_CAPTURE_HASH: $capture',
          );
        }
        if (actual.hash != hash) {
          throw AssertionError(
            'Expected capture $capture hash=$hash, got ${actual.hash}',
          );
        }
      case AssertCaptureNotBlack(
        :final capture,
        :final minNonBlackRatio,
        :final minAvgLuma,
      ):
        final actual = _captures[capture];
        if (actual == null) {
          throw AssertionError(
            'Missing capture for ASSERT_CAPTURE_NOT_BLACK: $capture',
          );
        }
        if (actual.nonBlackRatio < minNonBlackRatio ||
            actual.avgLuma < minAvgLuma) {
          throw AssertionError(
            'Expected capture $capture to be non-black '
            '(nonBlack>=${minNonBlackRatio.toStringAsFixed(4)}, avgLuma>=${minAvgLuma.toStringAsFixed(2)}), '
            'got nonBlack=${actual.nonBlackRatio.toStringAsFixed(4)}, '
            'avgLuma=${actual.avgLuma.toStringAsFixed(2)}, hash=${actual.hash}',
          );
        }
      case AssertAnalysisProcessCount(:final count):
        final actual = WindowManager.analysisProcessCount;
        if (actual != count) {
          throw AssertionError(
            'Expected analysis process count $count, got $actual; '
            'exits=${WindowManager.analysisExitCodes}',
          );
        }
      case AssertTrackBufferCountBelow(:final maxCount):
        final diagnostics = await controller.getDiagnostics();
        final tracks = diagnostics['tracks'] as List<dynamic>? ?? const [];
        for (final rawTrack in tracks) {
          final track = rawTrack as Map<dynamic, dynamic>;
          final bufferCount = track['bufferCount'] as int? ?? 0;
          final slot = track['slot'] as int? ?? -1;
          if (bufferCount > maxCount) {
            throw AssertionError(
              'Expected track[$slot] bufferCount <= $maxCount, got $bufferCount',
            );
          }
        }
      case AssertResourceUsageBelow(:final maxRssMb, :final maxDedicatedGpuMb):
        final actual = await _currentResourceUsageMetric();
        _assertResourceMetricAvailable(actual, maxDedicatedGpuMb);
        final rssMb = _bytesToMb(actual.rssBytes);
        final gpuMb = _bytesToMb(actual.dedicatedGpuBytes);
        if (rssMb > maxRssMb || gpuMb > maxDedicatedGpuMb) {
          throw AssertionError(
            'Expected resource usage <= rss=${maxRssMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${maxDedicatedGpuMb.toStringAsFixed(1)}MB; '
            'got rss=${rssMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${gpuMb.toStringAsFixed(1)}MB',
          );
        }
      case AssertResourceUsageDeltaBelow(
        :final baseline,
        :final maxRssDeltaMb,
        :final maxDedicatedGpuDeltaMb,
      ):
        final expected = _resourceBaselines[baseline];
        if (expected == null) {
          throw AssertionError(
            'Missing baseline for ASSERT_RESOURCE_USAGE_DELTA_BELOW: $baseline',
          );
        }
        final actual = await _currentResourceUsageMetric();
        _assertResourceMetricAvailable(actual, maxDedicatedGpuDeltaMb);
        final rssDeltaMb = _bytesToMb(actual.rssBytes - expected.rssBytes);
        final gpuDeltaMb = _bytesToMb(
          actual.dedicatedGpuBytes - expected.dedicatedGpuBytes,
        );
        if (rssDeltaMb > maxRssDeltaMb || gpuDeltaMb > maxDedicatedGpuDeltaMb) {
          throw AssertionError(
            'Expected resource delta from $baseline <= '
            'rss=${maxRssDeltaMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${maxDedicatedGpuDeltaMb.toStringAsFixed(1)}MB; '
            'got rss=${rssDeltaMb.toStringAsFixed(1)}MB, '
            'dedicatedGpu=${gpuDeltaMb.toStringAsFixed(1)}MB',
          );
        }
      case AssertNativeSeekCountDelta(:final baseline, :final expectedDelta):
        final expected = _nativeSeekCountBaselines[baseline];
        if (expected == null) {
          throw AssertionError(
            'Missing baseline for ASSERT_NATIVE_SEEK_COUNT_DELTA: $baseline',
          );
        }
        final actual = _currentNativeSeekCount();
        final delta = actual - expected;
        if (delta != expectedDelta) {
          throw AssertionError(
            'Expected native seek count delta from $baseline to be '
            '$expectedDelta, got $delta (baseline=$expected, actual=$actual)',
          );
        }
    }
  }

  Future<void> _executeWait(WaitState state, Duration timeout) async {
    final sw = Stopwatch()..start();
    while (sw.elapsed < timeout) {
      final satisfied = switch (state) {
        WaitState.playing => await controller.isPlaying(),
        WaitState.paused => !await controller.isPlaying(),
      };
      if (satisfied) return;
      await Future.delayed(const Duration(milliseconds: 50));
    }
    throw AssertionError(
      'WAIT_${state.name.toUpperCase()} timed out after ${timeout.inMilliseconds}ms',
    );
  }

  Future<_ViewCenterMetric> _currentViewCenterMetric() async {
    final layout = await controller.getLayout();
    final tracks = await controller.getTracks();
    final capture = await controller.captureViewport();
    final display = _displayPixelSizeForLayout(
      width: capture.width,
      height: capture.height,
      layout: layout,
      tracks: tracks,
    );
    final x = display.width.abs() > 1e-4
        ? layout.viewOffsetX / display.width
        : 0.0;
    final y = display.height.abs() > 1e-4
        ? layout.viewOffsetY / display.height
        : 0.0;
    return _ViewCenterMetric(x, y);
  }

  Future<_ResourceUsageMetric> _currentResourceUsageMetric() async {
    final diagnostics = await controller.getDiagnostics();
    final rssBytes =
        diagnostics['processRssBytes'] as int? ?? ProcessInfo.currentRss;
    final dedicatedGpuBytes =
        diagnostics['dedicatedGpuUsageBytes'] as int? ?? 0;
    return _ResourceUsageMetric(
      rssBytes: rssBytes,
      dedicatedGpuBytes: dedicatedGpuBytes,
    );
  }

  int _currentNativeSeekCount() {
    final file = File(
      '${logConfig.logsDir}${Platform.pathSeparator}${logConfig.nativeLogFileName}',
    );
    if (!file.existsSync()) {
      throw StateError('Native log file not found: ${file.path}');
    }
    final text = file.readAsStringSync();
    return RegExp(
      RegExp.escape('[VideoRendererPlugin] seek:'),
    ).allMatches(text).length;
  }

  void _assertResourceMetricAvailable(
    _ResourceUsageMetric metric,
    double gpuThresholdMb,
  ) {
    if (gpuThresholdMb >= 0 && metric.dedicatedGpuBytes <= 0) {
      throw AssertionError('Dedicated GPU memory metric is unavailable');
    }
  }

  static double _bytesToMb(int bytes) => bytes / 1024.0 / 1024.0;

  static String _formatMb(int bytes) => _bytesToMb(bytes).toStringAsFixed(1);

  ({double width, double height}) _displayPixelSizeForLayout({
    required int width,
    required int height,
    required LayoutState layout,
    required List<TrackInfo> tracks,
  }) {
    if (width <= 0 || height <= 0 || tracks.isEmpty) {
      return (width: width.toDouble(), height: height.toDouble());
    }

    TrackInfo? track;
    for (final fileId in layout.order) {
      for (final candidate in tracks) {
        if (candidate.fileId == fileId) {
          track = candidate;
          break;
        }
      }
      if (track != null) break;
    }
    track ??= tracks.first;

    var slotW = width.toDouble();
    final slotH = height.toDouble();
    if (layout.mode != LayoutMode.splitScreen && tracks.length > 1) {
      slotW /= tracks.length;
    }
    final slotAspect = slotH > 0 ? slotW / slotH : 1.0;

    var refTrack = tracks.first;
    var maxPixels = 0;
    for (final candidate in tracks) {
      final pixels = candidate.width * candidate.height;
      if (pixels > maxPixels) {
        maxPixels = pixels;
        refTrack = candidate;
      }
    }

    final refW = refTrack.width.toDouble();
    final refH = refTrack.height.toDouble();
    final refDensity = refW > 0 && refH > 0
        ? math.min(slotW / refW, slotH / refH)
        : 1.0;

    final trackW = track.width.toDouble();
    final trackH = track.height.toDouble();
    final trackDensity = trackW > 0 && trackH > 0
        ? math.min(slotW / trackW, slotH / trackH)
        : 1.0;
    final trackScale = trackDensity > 0 ? refDensity / trackDensity : 1.0;

    var videoAspect = trackH > 0 ? trackW / trackH : slotAspect;
    if (videoAspect <= 0) videoAspect = slotAspect;

    var fitScale = videoAspect > slotAspect ? slotAspect / videoAspect : 1.0;
    fitScale *= trackScale;
    final displayScale = fitScale * layout.zoomRatio;
    final dsX = slotAspect > 0
        ? videoAspect * displayScale / slotAspect
        : displayScale;
    final dsY = displayScale;

    return (width: dsX * slotW, height: dsY * slotH);
  }
}

// ---------------------------------------------------------------------------
// Script parser
// ---------------------------------------------------------------------------

/// Parse a CSV test script file into scheduled instructions.
List<ScriptInstruction> _parseScript(String path) {
  final file = File(path);
  if (!file.existsSync()) {
    log.severe('Test script not found: $path');
    return [];
  }

  final instructions = <ScriptInstruction>[];
  final lines = file.readAsLinesSync();

  for (var i = 0; i < lines.length; i++) {
    final line = lines[i].trim();
    if (line.isEmpty || line.startsWith('#') || line.startsWith('@')) continue;

    final parts = line.split(',').map((s) => s.trim()).toList();
    if (parts.length < 2) {
      log.warning('Test script line ${i + 1}: invalid format: $line');
      continue;
    }

    final time = Duration(
      milliseconds: (double.parse(parts[0]) * 1000).round(),
    );
    final cmd = parts[1].toUpperCase();

    final instr = _parseInstruction(time, cmd, parts.sublist(2), line);
    if (instr != null) instructions.add(instr);
  }

  // Sort by time
  instructions.sort((a, b) => a.time.compareTo(b.time));
  return instructions;
}

ScriptInstruction? _parseInstruction(
  Duration time,
  String cmd,
  List<String> args,
  String rawLine,
) {
  switch (cmd) {
    // Actions — playback
    case 'PLAY':
      return ScriptAction(time, const Play());
    case 'PAUSE':
      return ScriptAction(time, const Pause());
    case 'TOGGLE_PLAY_PAUSE':
      return ScriptAction(time, const TogglePlayPause());
    case 'SEEK_TO':
      if (args.isEmpty) {
        log.warning('SEEK_TO missing ptsUs argument: $rawLine');
        return null;
      }
      return ScriptAction(time, SeekTo(int.parse(args[0])));
    case 'CLICK_TIMELINE_FRACTION':
      if (args.isEmpty) {
        log.warning(
          'CLICK_TIMELINE_FRACTION missing fraction argument: $rawLine',
        );
        return null;
      }
      return ScriptAction(time, ClickTimelineFraction(double.parse(args[0])));
    case 'SET_SPEED':
      if (args.isEmpty) {
        log.warning('SET_SPEED missing speed argument: $rawLine');
        return null;
      }
      return ScriptAction(time, SetSpeed(double.parse(args[0])));
    case 'STEP_FORWARD':
      return ScriptAction(time, const StepForward());
    case 'STEP_BACKWARD':
      return ScriptAction(time, const StepBackward());

    // Actions — media
    case 'OPEN_FILE':
      return ScriptAction(time, const OpenFile());
    case 'ADD_MEDIA':
      if (args.isEmpty) {
        log.warning('ADD_MEDIA missing path argument: $rawLine');
        return null;
      }
      return ScriptAction(time, AddMedia(args[0]));
    case 'REMOVE_TRACK':
      if (args.isEmpty) {
        log.warning('REMOVE_TRACK missing slot argument: $rawLine');
        return null;
      }
      return ScriptAction(time, RemoveTrackAction(int.parse(args[0])));
    case 'ADJUST_TRACK_OFFSET':
      if (args.length < 2) {
        log.warning(
          'ADJUST_TRACK_OFFSET needs slot and deltaMs arguments: $rawLine',
        );
        return null;
      }
      return ScriptAction(
        time,
        AdjustTrackOffset(int.parse(args[0]), int.parse(args[1])),
      );
    case 'SET_LOOP_ENABLED':
      if (args.isEmpty) {
        log.warning('SET_LOOP_ENABLED missing enabled argument: $rawLine');
        return null;
      }
      return ScriptAction(
        time,
        SetLoopEnabled(args[0] == '1' || args[0].toLowerCase() == 'true'),
      );
    case 'SET_LOOP_RANGE':
      if (args.length < 2) {
        log.warning('SET_LOOP_RANGE needs startUs and endUs: $rawLine');
        return null;
      }
      return ScriptAction(
        time,
        SetLoopRange(int.parse(args[0]), int.parse(args[1])),
      );
    case 'DRAG_LOOP_HANDLE':
      if (args.length < 2) {
        log.warning(
          'DRAG_LOOP_HANDLE needs handle and targetUs arguments: $rawLine',
        );
        return null;
      }
      return ScriptAction(
        time,
        DragLoopHandle(
          args[0],
          int.parse(args[1]),
          steps: args.length >= 3 ? int.parse(args[2]) : 12,
        ),
      );

    // Actions — layout
    case 'SET_ZOOM':
      if (args.isEmpty) {
        log.warning('SET_ZOOM missing ratio argument: $rawLine');
        return null;
      }
      return ScriptAction(time, SetZoom(double.parse(args[0])));
    case 'SET_LAYOUT_MODE':
      if (args.isEmpty) {
        log.warning('SET_LAYOUT_MODE missing mode argument: $rawLine');
        return null;
      }
      return ScriptAction(time, SetLayoutMode(int.parse(args[0])));
    case 'SET_SPLIT_POS':
      if (args.isEmpty) {
        log.warning('SET_SPLIT_POS missing position argument: $rawLine');
        return null;
      }
      return ScriptAction(time, SetSplitPos(double.parse(args[0])));
    case 'TOGGLE_LAYOUT_MODE':
      return ScriptAction(time, const ToggleLayoutMode());
    case 'PAN':
      if (args.length < 2) {
        log.warning('PAN needs dx and dy arguments: $rawLine');
        return null;
      }
      return ScriptAction(
        time,
        Pan(double.parse(args[0]), double.parse(args[1])),
      );
    case 'SET_RENDER_SIZE':
      if (args.length < 2) {
        log.warning(
          'SET_RENDER_SIZE needs width and height arguments: $rawLine',
        );
        return null;
      }
      return ScriptAction(
        time,
        SetRenderSize(int.parse(args[0]), int.parse(args[1])),
      );
    case 'NEW_WINDOW':
      return ScriptAction(time, const NewWindow());
    case 'OPEN_SETTINGS':
      return ScriptAction(time, const OpenSettings());
    case 'OPEN_STATS':
      return ScriptAction(time, const OpenStats());
    case 'OPEN_MEMORY':
      return ScriptAction(time, const OpenMemory());
    case 'CAPTURE_VIEWPORT':
      if (args.isEmpty) {
        log.warning('CAPTURE_VIEWPORT needs a capture name: $rawLine');
        return null;
      }
      return ScriptAction(
        time,
        CaptureViewportAction(
          args[0],
          outputPath: args.length >= 2 ? args[1] : null,
        ),
      );
    case 'WINDOW_MAXIMIZE':
      return ScriptAction(time, const WindowMaximize());
    case 'WINDOW_RESTORE':
      return ScriptAction(time, const WindowRestore());
    case 'STORE_VIEW_CENTER':
      if (args.isEmpty) {
        log.warning('STORE_VIEW_CENTER needs a baseline name: $rawLine');
        return null;
      }
      return ScriptAction(time, StoreViewCenter(args[0]));
    case 'STORE_RESOURCE_USAGE':
      if (args.isEmpty) {
        log.warning('STORE_RESOURCE_USAGE needs a baseline name: $rawLine');
        return null;
      }
      return ScriptAction(time, StoreResourceUsage(args[0]));
    case 'STORE_NATIVE_SEEK_COUNT':
      if (args.isEmpty) {
        log.warning('STORE_NATIVE_SEEK_COUNT needs a baseline name: $rawLine');
        return null;
      }
      return ScriptAction(time, StoreNativeSeekCount(args[0]));
    case 'RUN_ANALYSIS':
    case 'TRIGGER_ANALYSIS':
      return ScriptAction(time, const RunAnalysis());

    // Waits
    case 'WAIT_PLAYING':
      final timeoutMs = args.isNotEmpty ? int.parse(args[0]) : 3000;
      return ScriptWait(
        time,
        WaitState.playing,
        Duration(milliseconds: timeoutMs),
      );
    case 'WAIT_PAUSED':
      final timeoutMs = args.isNotEmpty ? int.parse(args[0]) : 3000;
      return ScriptWait(
        time,
        WaitState.paused,
        Duration(milliseconds: timeoutMs),
      );
    case 'WAIT_ANALYSIS_PROCESS_COUNT':
      if (args.isEmpty) {
        log.warning(
          'WAIT_ANALYSIS_PROCESS_COUNT missing count argument: $rawLine',
        );
        return null;
      }
      final timeoutMs = args.length >= 2 ? int.parse(args[1]) : 10000;
      return ScriptWaitAnalysisProcessCount(
        time,
        int.parse(args[0]),
        Duration(milliseconds: timeoutMs),
      );
    case 'SET_ANALYSIS_TEST_SCRIPT':
      if (args.isEmpty) {
        log.warning('SET_ANALYSIS_TEST_SCRIPT missing path argument: $rawLine');
        return null;
      }
      return ScriptSetAnalysisTestScript(time, args[0]);

    // Asserts — playback
    case 'ASSERT_PLAYING':
      return ScriptAssert(time, const AssertPlaying());
    case 'ASSERT_PAUSED':
      return ScriptAssert(time, const AssertPaused());
    case 'ASSERT_POSITION':
      if (args.length < 2) {
        log.warning('ASSERT_POSITION needs ptsUs and toleranceMs: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertPosition(int.parse(args[0]), int.parse(args[1])),
      );
    case 'ASSERT_POSITION_RANGE':
      if (args.length < 2) {
        log.warning('ASSERT_POSITION_RANGE needs minUs and maxUs: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertPositionRange(int.parse(args[0]), int.parse(args[1])),
      );
    case 'ASSERT_TRACK_COUNT':
      if (args.isEmpty) {
        log.warning('ASSERT_TRACK_COUNT missing count argument: $rawLine');
        return null;
      }
      return ScriptAssert(time, AssertTrackCount(int.parse(args[0])));
    case 'ASSERT_DURATION':
      if (args.length < 2) {
        log.warning('ASSERT_DURATION needs ptsUs and toleranceMs: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertDuration(int.parse(args[0]), int.parse(args[1])),
      );

    // Asserts — layout
    case 'ASSERT_LAYOUT_MODE':
      if (args.isEmpty) {
        log.warning('ASSERT_LAYOUT_MODE missing mode argument: $rawLine');
        return null;
      }
      return ScriptAssert(time, AssertLayoutMode(int.parse(args[0])));
    case 'ASSERT_ZOOM':
      if (args.length < 2) {
        log.warning('ASSERT_ZOOM needs ratio and tolerance: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertZoom(double.parse(args[0]), double.parse(args[1])),
      );
    case 'ASSERT_SPLIT_POS':
      if (args.length < 2) {
        log.warning('ASSERT_SPLIT_POS needs position and tolerance: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertSplitPos(double.parse(args[0]), double.parse(args[1])),
      );
    case 'ASSERT_VIEW_OFFSET':
      if (args.length < 3) {
        log.warning('ASSERT_VIEW_OFFSET needs x, y and tolerance: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertViewOffset(
          double.parse(args[0]),
          double.parse(args[1]),
          double.parse(args[2]),
        ),
      );
    case 'ASSERT_VIEW_CENTER_STABLE':
      if (args.length < 2) {
        log.warning(
          'ASSERT_VIEW_CENTER_STABLE needs baseline and tolerance: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertViewCenterStable(args[0], double.parse(args[1])),
      );
    case 'ASSERT_CAPTURE_EQUALS':
      if (args.length < 2) {
        log.warning(
          'ASSERT_CAPTURE_EQUALS needs expected and actual capture names: $rawLine',
        );
        return null;
      }
      return ScriptAssert(time, AssertCaptureEquals(args[0], args[1]));
    case 'ASSERT_CAPTURE_CHANGED':
      if (args.length < 2) {
        log.warning(
          'ASSERT_CAPTURE_CHANGED needs before and after capture names: $rawLine',
        );
        return null;
      }
      return ScriptAssert(time, AssertCaptureChanged(args[0], args[1]));
    case 'ASSERT_CAPTURE_HASH':
      if (args.length < 2) {
        log.warning(
          'ASSERT_CAPTURE_HASH needs capture name and hash: $rawLine',
        );
        return null;
      }
      return ScriptAssert(time, AssertCaptureHash(args[0], args[1]));
    case 'ASSERT_CAPTURE_NOT_BLACK':
      if (args.isEmpty) {
        log.warning('ASSERT_CAPTURE_NOT_BLACK needs capture name: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertCaptureNotBlack(
          args[0],
          minNonBlackRatio: args.length >= 2 ? double.parse(args[1]) : 0.01,
          minAvgLuma: args.length >= 3 ? double.parse(args[2]) : 4.0,
        ),
      );
    case 'ASSERT_ANALYSIS_PROCESS_COUNT':
      if (args.isEmpty) {
        log.warning(
          'ASSERT_ANALYSIS_PROCESS_COUNT missing count argument: $rawLine',
        );
        return null;
      }
      return ScriptAssert(time, AssertAnalysisProcessCount(int.parse(args[0])));
    case 'ASSERT_TRACK_BUFFER_COUNT_BELOW':
      if (args.isEmpty) {
        log.warning(
          'ASSERT_TRACK_BUFFER_COUNT_BELOW missing maxCount argument: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertTrackBufferCountBelow(int.parse(args[0])),
      );
    case 'ASSERT_RESOURCE_USAGE_BELOW':
      if (args.length < 2) {
        log.warning(
          'ASSERT_RESOURCE_USAGE_BELOW needs maxRssMb and maxDedicatedGpuMb: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertResourceUsageBelow(double.parse(args[0]), double.parse(args[1])),
      );
    case 'ASSERT_RESOURCE_USAGE_DELTA_BELOW':
      if (args.length < 3) {
        log.warning(
          'ASSERT_RESOURCE_USAGE_DELTA_BELOW needs baseline, maxRssDeltaMb and maxDedicatedGpuDeltaMb: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertResourceUsageDeltaBelow(
          args[0],
          double.parse(args[1]),
          double.parse(args[2]),
        ),
      );
    case 'ASSERT_NATIVE_SEEK_COUNT_DELTA':
      if (args.length < 2) {
        log.warning(
          'ASSERT_NATIVE_SEEK_COUNT_DELTA needs baseline and expectedDelta: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertNativeSeekCountDelta(args[0], int.parse(args[1])),
      );

    // Control
    case 'QUIT':
      final exitCode = args.isNotEmpty ? int.parse(args[0]) : 0;
      return ScriptQuit(time, exitCode);

    default:
      log.warning('Unknown test script command: $cmd');
      return null;
  }
}
