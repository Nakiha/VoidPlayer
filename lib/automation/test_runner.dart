import 'dart:async';
import 'dart:io';

import 'package:window_manager/window_manager.dart' as wm;

import '../actions/automation_action.dart';
import '../actions/player_action.dart';
import '../actions/player_assert.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import '../preferences/playback_preferences.dart';
import 'automation_assert_executor.dart';
import 'automation_probe.dart';
import 'automation_run_state.dart';
import 'test_video_generator.dart';
import 'ui_automation_bridge.dart';
import '../video_renderer_controller.dart';
import '../windows/window_manager.dart';

/// A parsed instruction from a test script, with its scheduled time.
sealed class ScriptInstruction {
  final Duration time;
  const ScriptInstruction(this.time);
}

class ScriptAction extends ScriptInstruction {
  final PlayerAction action;
  const ScriptAction(super.time, this.action);
}

class ScriptAutomationAction extends ScriptInstruction {
  final AutomationAction action;
  const ScriptAutomationAction(super.time, this.action);
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

class ScriptGenerateTestVideo extends ScriptInstruction {
  final String path;
  final int frames;
  final int fps;
  final int width;
  final int height;

  const ScriptGenerateTestVideo(
    super.time, {
    required this.path,
    required this.frames,
    required this.fps,
    required this.width,
    required this.height,
  });
}

class ScriptSetSeekAfterJumpBehavior extends ScriptInstruction {
  final SeekAfterJumpBehavior behavior;
  const ScriptSetSeekAfterJumpBehavior(super.time, this.behavior);
}

class ScriptQuit extends ScriptInstruction {
  final int exitCode;
  const ScriptQuit(super.time, this.exitCode);
}

/// Parses a test script file and runs instructions on a timeline.
class TestRunner {
  final String scriptPath;
  final UiAutomationBridge automation;
  final AutomationRunState _state = AutomationRunState();

  TestRunner({required this.scriptPath, required this.automation});

  NativePlayerController get controller => automation.controller;

  AutomationProbe get _probe => AutomationProbe(controller);

  AutomationAssertExecutor get _assertExecutor =>
      AutomationAssertExecutor(probe: _probe, state: _state);

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

      case ScriptAutomationAction(:final action):
        await _executeAutomationAction(action);

      case ScriptAssert(:final assertion):
        log.info('TestRunner ${instr.time}: assert ${assertion.runtimeType}');
        await _assertExecutor.execute(assertion);

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

      case ScriptGenerateTestVideo(
        :final path,
        :final frames,
        :final fps,
        :final width,
        :final height,
      ):
        log.info(
          'TestRunner ${instr.time}: GENERATE_TEST_VIDEO '
          '$path frames=$frames fps=$fps size=${width}x$height',
        );
        await generateTestVideo(
          path: path,
          frames: frames,
          fps: fps,
          width: width,
          height: height,
        );

      case ScriptSetSeekAfterJumpBehavior(:final behavior):
        log.info(
          'TestRunner ${instr.time}: SET_SEEK_AFTER_JUMP_BEHAVIOR ${behavior.storageValue}',
        );
        AppConfig.instance.seekAfterJumpBehavior = behavior;
        await AppConfig.instance.save();

      case ScriptQuit(:final exitCode):
        log.info('TestRunner ${instr.time}: QUIT $exitCode');
        await WindowManager.closeAllAnalysisWindows();
        exit(exitCode);
    }
  }

  Future<void> _executeAction(PlayerAction action) async {
    automation.executePlayerAction(action);
  }

  Future<void> _executeAutomationAction(AutomationAction action) async {
    switch (action) {
      case SetRenderSize(:final width, :final height):
        log.info('TestRunner: SET_RENDER_SIZE ${width}x$height');
        await controller.resize(width, height);
      case CaptureViewportAction(:final nameId, :final outputPath):
        final capture = await controller.captureViewport(
          outputPath: outputPath,
        );
        _state.captures[nameId] = capture;
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
        final metric = await _probe.currentViewCenterMetric();
        _state.viewCenterBaselines[nameId] = metric;
        log.info(
          'TestRunner: STORE_VIEW_CENTER $nameId '
          'normalized=(${metric.x.toStringAsFixed(6)}, ${metric.y.toStringAsFixed(6)})',
        );
      case StoreResourceUsage(:final nameId):
        final metric = await _probe.currentResourceUsageMetric();
        _state.resourceBaselines[nameId] = metric;
        log.info(
          'TestRunner: STORE_RESOURCE_USAGE $nameId '
          'rss=${AutomationProbe.formatMb(metric.rssBytes)}MB '
          'dedicatedGpu=${AutomationProbe.formatMb(metric.dedicatedGpuBytes)}MB',
        );
      case StoreNativeSeekCount(:final nameId):
        final count = _probe.currentNativeSeekCount();
        _state.nativeSeekCountBaselines[nameId] = count;
        log.info('TestRunner: STORE_NATIVE_SEEK_COUNT $nameId count=$count');
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
    case 'DRAG_SPLIT_HANDLE':
      if (args.isEmpty) {
        log.warning('DRAG_SPLIT_HANDLE needs target fraction: $rawLine');
        return null;
      }
      return ScriptAction(
        time,
        DragSplitHandle(
          double.parse(args[0]),
          steps: args.length >= 2 ? int.parse(args[1]) : 12,
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
    case 'TOGGLE_FULL_SCREEN':
      return ScriptAction(time, const ToggleFullScreen());
    case 'EXIT_FULL_SCREEN':
      return ScriptAction(time, const ExitFullScreen());
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
      return ScriptAutomationAction(
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
      return ScriptAutomationAction(
        time,
        CaptureViewportAction(
          args[0],
          outputPath: args.length >= 2 ? args[1] : null,
        ),
      );
    case 'WINDOW_MAXIMIZE':
      return ScriptAutomationAction(time, const WindowMaximize());
    case 'WINDOW_RESTORE':
      return ScriptAutomationAction(time, const WindowRestore());
    case 'STORE_VIEW_CENTER':
      if (args.isEmpty) {
        log.warning('STORE_VIEW_CENTER needs a baseline name: $rawLine');
        return null;
      }
      return ScriptAutomationAction(time, StoreViewCenter(args[0]));
    case 'STORE_RESOURCE_USAGE':
      if (args.isEmpty) {
        log.warning('STORE_RESOURCE_USAGE needs a baseline name: $rawLine');
        return null;
      }
      return ScriptAutomationAction(time, StoreResourceUsage(args[0]));
    case 'STORE_NATIVE_SEEK_COUNT':
      if (args.isEmpty) {
        log.warning('STORE_NATIVE_SEEK_COUNT needs a baseline name: $rawLine');
        return null;
      }
      return ScriptAutomationAction(time, StoreNativeSeekCount(args[0]));
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
    case 'GENERATE_TEST_VIDEO':
      if (args.isEmpty) {
        log.warning('GENERATE_TEST_VIDEO missing path argument: $rawLine');
        return null;
      }
      return ScriptGenerateTestVideo(
        time,
        path: args[0],
        frames: args.length >= 2 ? int.parse(args[1]) : 9000,
        fps: args.length >= 3 ? int.parse(args[2]) : 120,
        width: args.length >= 4 ? int.parse(args[3]) : 64,
        height: args.length >= 5 ? int.parse(args[4]) : 64,
      );
    case 'SET_SEEK_AFTER_JUMP_BEHAVIOR':
      if (args.isEmpty) {
        log.warning(
          'SET_SEEK_AFTER_JUMP_BEHAVIOR missing behavior argument: $rawLine',
        );
        return null;
      }
      final behavior = SeekAfterJumpBehavior.fromStorage(args[0]);
      return ScriptSetSeekAfterJumpBehavior(time, behavior);

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
    case 'ASSERT_MAIN_WINDOW_BORDERLESS':
      return ScriptAssert(time, const AssertMainWindowBorderless());
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
