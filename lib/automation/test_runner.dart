import 'dart:async';
import 'dart:io';

import 'package:window_manager/window_manager.dart' as wm;

import '../actions/automation_action.dart';
import '../actions/player_action.dart';
import '../app_log.dart';
import '../config/app_config.dart';
import 'automation_assert_executor.dart';
import 'automation_probe.dart';
import 'automation_run_state.dart';
import 'automation_script.dart';
import 'test_video_generator.dart';
import 'ui_automation_bridge.dart';
import '../video_renderer_controller.dart';

/// Parses a test script file and runs instructions on a timeline.
class TestRunner {
  final String scriptPath;
  final UiAutomationBridge automation;
  final AutomationRunState _state = AutomationRunState();

  TestRunner({required this.scriptPath, required this.automation});

  NativePlayerController get controller => automation.controller;

  AutomationProbe get _probe => AutomationProbe(controller);

  AutomationAssertExecutor get _assertExecutor => AutomationAssertExecutor(
    probe: _probe,
    state: _state,
    analysisProcesses: automation.analysisProcesses,
  );

  /// Parse and execute the test script. Exits the process on QUIT or failure.
  Future<void> run() async {
    final instructions = parseAutomationScript(scriptPath);
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
        final ok = await automation.waitForAnalysisProcessCount(count, timeout);
        if (!ok) {
          throw AssertionError(
            'Expected $count analysis process(es), got '
            '${automation.analysisProcessCount}; exits=${automation.analysisExitCodes}',
          );
        }

      case ScriptSetAnalysisTestScript(:final path):
        log.info('TestRunner ${instr.time}: SET_ANALYSIS_TEST_SCRIPT $path');
        automation.analysisTestScriptPath = path;

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
        await automation.closeAllAnalysisWindows();
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
