import 'dart:async';

import '../actions/automation_action.dart';
import '../actions/player_action.dart';
import '../analysis/analysis_cache.dart';
import '../app_log.dart';
import '../video_renderer_controller.dart';
import '../windows/main/main_window_test_hooks.dart';
import 'automation_assert_executor.dart';
import 'automation_probe.dart';
import 'automation_run_state.dart';
import 'automation_script.dart';
import 'ui_automation_bridge.dart';
import 'ui_automation_runtime.dart';

/// Parses a test script file and runs instructions on a timeline.
class TestRunner {
  final String scriptPath;
  final UiAutomationBridge automation;
  final UiAutomationRuntime runtime;
  final AutomationRunState _state = AutomationRunState();
  bool _terminalInstructionSeen = false;

  TestRunner({
    required this.scriptPath,
    required this.automation,
    this.runtime = const DefaultUiAutomationRuntime(),
  });

  NativePlayerController get controller => automation.controller;

  AutomationProbe get _probe => AutomationProbe(controller);

  AutomationAssertExecutor get _assertExecutor => AutomationAssertExecutor(
    probe: _probe,
    state: _state,
    analysisProcesses: automation.analysisProcesses,
    effectiveDurationUs: automation.effectiveDurationUs,
  );

  /// Parse and execute the test script. Exits the process on QUIT or failure.
  Future<void> run() async {
    final instructions = parseAutomationScript(scriptPath);
    if (instructions.isEmpty) {
      log.severe('Test script is empty: $scriptPath');
      runtime.quit(1);
      return;
    }

    log.info(
      'TestRunner: running ${instructions.length} instructions from $scriptPath',
    );

    final sw = Stopwatch()..start();

    for (final instr in instructions) {
      final waitMs = instr.time.inMilliseconds - sw.elapsedMilliseconds;
      if (waitMs > 0) {
        await Future<void>.delayed(Duration(milliseconds: waitMs));
      }

      try {
        await _execute(instr);
        if (_terminalInstructionSeen) return;
      } catch (e) {
        log.severe('TestRunner FAIL at ${instr.time}: $e');
        runtime.quit(1);
        return;
      }
    }

    // If we reach here without a QUIT instruction, that's an error.
    log.severe('TestRunner: script ended without QUIT instruction');
    runtime.quit(1);
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
        :final ptsOffsetUs,
        :final withAudio,
      ):
        log.info(
          'TestRunner ${instr.time}: GENERATE_TEST_VIDEO '
          '$path frames=$frames fps=$fps size=${width}x$height '
          'ptsOffsetUs=$ptsOffsetUs withAudio=$withAudio',
        );
        await runtime.generateVideo(
          path: path,
          frames: frames,
          fps: fps,
          width: width,
          height: height,
          ptsOffsetUs: ptsOffsetUs,
          withAudio: withAudio,
        );

      case ScriptSetSeekAfterJumpBehavior(:final behavior):
        log.info(
          'TestRunner ${instr.time}: SET_SEEK_AFTER_JUMP_BEHAVIOR ${behavior.storageValue}',
        );
        await runtime.setSeekAfterJumpBehavior(behavior);

      case ScriptSetDecodeMode(:final mode):
        log.info(
          'TestRunner ${instr.time}: SET_DECODE_MODE ${mode.storageValue}',
        );
        await runtime.setDecodeMode(mode);

      case ScriptSetAudibleTrack(:final fileId):
        log.info('TestRunner ${instr.time}: SET_AUDIBLE_TRACK $fileId');
        await controller.setAudibleTrack(fileId);

      case ScriptSetViewportPixelSizeMode(:final mode):
        log.info(
          'TestRunner ${instr.time}: SET_VIEWPORT_PIXEL_SIZE_MODE ${mode.storageValue}',
        );
        await runtime.setViewportPixelSizeMode(mode);
        if (controller.hasPlayer) {
          final layout = await controller.getLayout();
          await controller.applyLayout(
            layout.copyWith(pixelSizeMode: mode.layoutValue),
          );
        }

      case ScriptQuit(:final exitCode):
        log.info('TestRunner ${instr.time}: QUIT $exitCode');
        await automation.closeAllAnalysisWindows();
        if (controller.hasPlayer) {
          await controller.destroyPlayerOnly();
        }
        _terminalInstructionSeen = true;
        runtime.quit(exitCode);

      case ScriptCloseMainWindow():
        log.info('TestRunner ${instr.time}: CLOSE_MAIN_WINDOW');
        _terminalInstructionSeen = true;
        await runtime.closeMainWindow();
        await Future<void>.delayed(const Duration(seconds: 5));
        log.severe('TestRunner: CLOSE_MAIN_WINDOW did not terminate the app');
        runtime.quit(1);
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
      case CaptureFlutterAction(:final nameId, :final outputPath):
        final capture = await testHarness.captureFlutterFrame(
          outputPath: outputPath,
        );
        _state.captures[nameId] = capture;
        log.info(
          'TestRunner: CAPTURE_FLUTTER $nameId hash=${capture.hash} ${capture.width}x${capture.height}'
          ' avgLuma=${capture.avgLuma.toStringAsFixed(2)}'
          ' nonBlack=${capture.nonBlackRatio.toStringAsFixed(4)}'
          '${capture.outputPath != null ? ' -> ${capture.outputPath}' : ''}',
        );
      case WindowMaximize():
        log.info('TestRunner: WINDOW_MAXIMIZE');
        await runtime.maximizeWindow();
      case WindowRestore():
        log.info('TestRunner: WINDOW_RESTORE');
        await runtime.restoreWindow();
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
          'private=${AutomationProbe.formatMb(metric.privateBytes)}MB '
          'heapAlloc=${AutomationProbe.formatMb(metric.heapAllocatedBytes)}MB '
          'heapCommit=${AutomationProbe.formatMb(metric.heapCommittedBytes)}MB '
          'heapReserve=${AutomationProbe.formatMb(metric.heapReservedBytes)}MB '
          'heaps=${metric.heapCount} '
          'dedicatedGpu=${AutomationProbe.formatMb(metric.dedicatedGpuBytes)}MB '
          '${AutomationProbe.formatGpuBreakdown(metric.gpuBreakdown, dedicatedGpuBytes: metric.dedicatedGpuBytes)}',
        );
      case StoreNativeSeekCount(:final nameId):
        final count = _probe.currentNativeSeekCount();
        _state.nativeSeekCountBaselines[nameId] = count;
        log.info('TestRunner: STORE_NATIVE_SEEK_COUNT $nameId count=$count');
      case HoverControlsBarButtons(:final steps):
        log.info('TestRunner: HOVER_CONTROLS_BAR_BUTTONS steps=$steps');
        testHarness.hoverControlsBarButtons(steps: steps);
      case HoverControlsBarButtonsNative(:final steps):
        log.info('TestRunner: HOVER_CONTROLS_BAR_BUTTONS_NATIVE steps=$steps');
        await testHarness.hoverControlsBarButtonsNative(steps: steps);
      case ClickMediaHeaderOverlayButtonNative():
        log.info('TestRunner: CLICK_MEDIA_HEADER_OVERLAY_BUTTON_NATIVE');
        await testHarness.clickAnalysisOverlayButtonNative();
      case ClickMediaHeaderOverlayButton():
        log.info('TestRunner: CLICK_MEDIA_HEADER_OVERLAY_BUTTON');
        testHarness.clickAnalysisOverlayButton();
      case HoverMediaHeaderOverlayButton():
        log.info('TestRunner: HOVER_MEDIA_HEADER_OVERLAY_BUTTON');
        testHarness.hoverAnalysisOverlayButton();
      case HoverMediaHeaderOverlayPanelControls():
        log.info('TestRunner: HOVER_MEDIA_HEADER_OVERLAY_PANEL_CONTROLS');
        testHarness.hoverAnalysisOverlayPanelControls();
      case AssertMediaHeaderOverlayPanelVisible(:final visible):
        log.info(
          'TestRunner: ASSERT_MEDIA_HEADER_OVERLAY_PANEL_VISIBLE $visible',
        );
        testHarness.assertAnalysisOverlayPanelVisible(visible);
      case ToggleAnalysisOverlay(:final slotIndex):
        log.info('TestRunner: TOGGLE_ANALYSIS_OVERLAY slot=$slotIndex');
        await automation.toggleAnalysisOverlayForSlot(slotIndex);
      case ToggleAnalysisOverlayPanel():
        log.info('TestRunner: TOGGLE_ANALYSIS_OVERLAY_PANEL');
        await automation.toggleAnalysisOverlayPanel();
      case GenerateAnalysisCache(:final slotIndex):
        log.info('TestRunner: GENERATE_ANALYSIS_CACHE slot=$slotIndex');
        final hash = await automation.generateAnalysisCacheForSlot(slotIndex);
        if (hash == null) {
          throw AssertionError(
            'Failed to generate analysis cache for slot $slotIndex',
          );
        }
        log.info(
          'TestRunner: GENERATE_ANALYSIS_CACHE slot=$slotIndex hash=$hash',
        );
      case SetAnalysisOverlayType(:final type):
        log.info('TestRunner: SET_ANALYSIS_OVERLAY_TYPE ${type.name}');
        automation.setAnalysisOverlayType(type);
      case SetAnalysisOverlayLayers(:final layers):
        log.info(
          'TestRunner: SET_ANALYSIS_OVERLAY_LAYERS '
          '${layers.map((layer) => layer.name).join(',')}',
        );
        automation.setAnalysisOverlayLayers(layers);
      case SetAnalysisOverlayOpacity(:final opacity):
        log.info('TestRunner: SET_ANALYSIS_OVERLAY_OPACITY $opacity');
        automation.setAnalysisOverlayOpacity(opacity);
      case ClearAnalysisChunks():
        log.info('TestRunner: CLEAR_ANALYSIS_CHUNKS');
        final result = await AnalysisCache.clearDerivedChunks();
        if (result.hasFailures) {
          throw AssertionError(
            'Failed to clear analysis chunks: ${result.failuresByHash}',
          );
        }
        log.info(
          'TestRunner: CLEAR_ANALYSIS_CHUNKS cleared ${result.deletedCount} cache entrie(s)',
        );
    }
  }

  MainWindowTestHarness get testHarness => automation.testHarness;

  Future<void> _executeWait(WaitState state, Duration timeout) async {
    final sw = Stopwatch()..start();
    while (sw.elapsed < timeout) {
      final satisfied = switch (state) {
        WaitState.playing => await controller.isPlaying(),
        WaitState.paused => !await controller.isPlaying(),
      };
      if (satisfied) return;
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    throw AssertionError(
      'WAIT_${state.name.toUpperCase()} timed out after ${timeout.inMilliseconds}ms',
    );
  }
}
