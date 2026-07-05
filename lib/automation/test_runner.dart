import 'dart:async';
import 'dart:io';
import 'dart:ui';

import 'package:flutter/scheduler.dart';
import 'package:path/path.dart' as p;

import '../actions/automation_action.dart';
import '../actions/player_action.dart';
import '../analysis/analysis_cache.dart';
import '../app_log.dart';
import '../video_renderer_controller.dart';
import '../viewport/viewport_projection_diagnostics.dart';
import 'automation_assert_executor.dart';
import 'automation_probe.dart';
import 'automation_run_state.dart';
import 'automation_script.dart';
import 'main_window_harness.dart';
import 'ui_automation_bridge.dart';
import 'ui_automation_runtime.dart';

/// Parses a test script file and runs instructions on a timeline.
class TestRunner {
  final String scriptPath;
  final UiAutomationBridge automation;
  final UiAutomationRuntime runtime;
  final AutomationRunState _state = AutomationRunState();
  final _FlutterFrameTimingProbe _flutterTimingProbe =
      _FlutterFrameTimingProbe.instance;
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
    timelinePtsUs: automation.timelinePtsUs,
    quickMarkCount: automation.quickMarkCount,
    dartViewportDiagnostics: automation.dartViewportDiagnostics,
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

    _flutterTimingProbe.ensureInstalled();
    _flutterTimingProbe.reset();
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

      case ScriptWaitTrackCount(:final count, :final timeout):
        log.info(
          'TestRunner ${instr.time}: WAIT_TRACK_COUNT $count ${timeout.inMilliseconds}ms',
        );
        await _executeWaitTrackCount(count, timeout);

      case ScriptWaitPresentedFrameRange(
        :final fileId,
        :final minUs,
        :final maxUs,
        :final timeout,
        :final interval,
      ):
        log.info(
          'TestRunner ${instr.time}: WAIT_PRESENTED_FRAME_RANGE '
          'fileId=$fileId range=[$minUs,$maxUs] '
          'timeout=${timeout.inMilliseconds}ms',
        );
        await _executeWaitPresentedFrameRange(
          fileId: fileId,
          minUs: minUs,
          maxUs: maxUs,
          timeout: timeout,
          interval: interval,
        );

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
    await automation.executePlayerAction(action);
  }

  Future<void> _executeAutomationAction(AutomationAction action) async {
    switch (action) {
      case SetRenderSize(:final width, :final height):
        log.info('TestRunner: SET_RENDER_SIZE ${width}x$height');
        await controller.resize(width, height);
      case CaptureViewportAction(:final nameId, :final outputPath):
        final capture = await controller.captureViewport(
          outputPath: _resolveCaptureOutputPath(outputPath),
        );
        _state.captures[nameId] = capture;
        log.info(
          'TestRunner: CAPTURE_VIEWPORT $nameId hash=${capture.hash} ${capture.width}x${capture.height}'
          ' avgLuma=${capture.avgLuma.toStringAsFixed(2)}'
          ' nonBlack=${capture.nonBlackRatio.toStringAsFixed(4)}'
          '${capture.outputPath != null ? ' -> ${capture.outputPath}' : ''}',
        );
      case CaptureViewportRegionAction(
        :final nameId,
        :final x,
        :final y,
        :final width,
        :final height,
        :final maxSize,
        :final outputPath,
      ):
        final capture = await controller.captureViewportRegion(
          x: x,
          y: y,
          width: width,
          height: height,
          maxSize: maxSize,
          outputPath: _resolveCaptureOutputPath(outputPath),
        );
        _state.captures[nameId] = capture;
        log.info(
          'TestRunner: CAPTURE_VIEWPORT_REGION $nameId '
          'roi=$x,$y ${width}x$height max=$maxSize '
          'hash=${capture.hash} ${capture.width}x${capture.height}'
          ' avgLuma=${capture.avgLuma.toStringAsFixed(2)}'
          ' nonBlack=${capture.nonBlackRatio.toStringAsFixed(4)}'
          '${capture.outputPath != null ? ' -> ${capture.outputPath}' : ''}',
        );
      case CaptureFlutterAction(:final nameId, :final outputPath):
        final capture = await testHarness.captureFlutterFrame(
          outputPath: _resolveCaptureOutputPath(outputPath),
        );
        _state.captures[nameId] = capture;
        log.info(
          'TestRunner: CAPTURE_FLUTTER $nameId hash=${capture.hash} ${capture.width}x${capture.height}'
          ' avgLuma=${capture.avgLuma.toStringAsFixed(2)}'
          ' nonBlack=${capture.nonBlackRatio.toStringAsFixed(4)}'
          '${capture.outputPath != null ? ' -> ${capture.outputPath}' : ''}',
        );
      case CaptureWindowAction(:final nameId, :final outputPath):
        final capture = await controller.captureWindow(
          outputPath: _resolveCaptureOutputPath(outputPath),
        );
        _state.captures[nameId] = capture;
        log.info(
          'TestRunner: CAPTURE_WINDOW $nameId hash=${capture.hash} ${capture.width}x${capture.height}'
          ' avgLuma=${capture.avgLuma.toStringAsFixed(2)}'
          ' nonBlack=${capture.nonBlackRatio.toStringAsFixed(4)}'
          '${capture.outputPath != null ? ' -> ${capture.outputPath}' : ''}',
        );
      case DebugFlutterSurfaceInfoAction():
        final info = await controller.debugFlutterSurfaceInfo();
        log.info(
          'TestRunner: DEBUG_FLUTTER_SURFACE_INFO '
          'texturePointer=${info['texturePointer']} '
          'texturePixelFormat=${info['texturePixelFormat']} '
          'textureSize=${info['textureWidth']}x${info['textureHeight']} '
          'ioSurfaceId=${info['ioSurfaceId']} '
          'wideGamut=${info['wideGamut']} '
          'nativeTextureObjectAvailable=${info['nativeTextureObjectAvailable']} '
          'nativeIOSurfaceObjectAvailable=${info['nativeIOSurfaceObjectAvailable']}',
        );
      case DebugNativeCompositorAction():
        final info = await controller.debugNativeCompositor();
        log.info(
          'TestRunner: DEBUG_NATIVE_COMPOSITOR '
          'enabled=${info['nativeCompositorEnabled']} '
          'frames=${info['nativeCompositorFrames']} '
          'succeeded=${info['nativeCompositorLastCompositeSucceeded']} '
          'mode=${info['nativeCompositorOutputMode']} '
          'pixelFormat=${info['nativeCompositorOutputPixelFormat']} '
          'edr=${info['nativeCompositorEDREnabled']} '
          'edrMaxRGBX1000=${info['nativeCompositorEDRVideoMaxRGBX1000']} '
          'edrOver1X1000=${info['nativeCompositorEDRVideoPixelsOver1X1000']} '
          'videoSRGBToLinear=${info['nativeCompositorVideoSRGBToLinearEnabled']} '
          'flutterSRGBToLinear=${info['nativeCompositorFlutterSRGBToLinearEnabled']} '
          'skippedInFlight=${info['nativeCompositorSkippedInFlightFrames']} '
          'skippedStatic=${info['nativeCompositorSkippedStaticFrames']} '
          'video=${info['nativeCompositorVideoTextureAvailable']} '
          'flutter=${info['nativeCompositorFlutterTextureAvailable']} '
          'flutterAlphaX1000=${info['nativeCompositorFlutterAlphaAverageX1000']} '
          'flutterTransparentX1000=${info['nativeCompositorFlutterTransparentRatioX1000']} '
          'hole=${info['nativeCompositorHoleLeftX1000']},${info['nativeCompositorHoleTopX1000']}-'
          '${info['nativeCompositorHoleRightX1000']},${info['nativeCompositorHoleBottomX1000']} '
          'drawable=${info['nativeCompositorDrawableWidth']}x${info['nativeCompositorDrawableHeight']} '
          'failure=${info['nativeCompositorLastFailure']}',
        );
      case DebugFailNativeCompositorAction(:final reason):
        log.info('TestRunner: DEBUG_FAIL_NATIVE_COMPOSITOR reason=$reason');
        await controller.debugFailNativeCompositor(reason: reason);
      case DebugSimulateWindowsDeviceLossAction(:final target, :final reason):
        log.info(
          'TestRunner: DEBUG_SIMULATE_WINDOWS_DEVICE_LOSS '
          'target=$target reason=$reason',
        );
        await controller.debugSimulateWindowsDeviceLoss(
          target: target,
          reason: reason,
        );
      case ResetNativePerfCountersAction():
        log.info('TestRunner: RESET_NATIVE_PERF_COUNTERS');
        ViewportProjectionDiagnostics.instance.reset();
        await controller.resetNativePerfCounters();
      case ResetDartViewportDiagnosticsAction():
        log.info('TestRunner: RESET_DART_VIEWPORT_DIAGNOSTICS');
        ViewportProjectionDiagnostics.instance.reset();
      case BeginNativeInteractionSampleAction(:final label):
        log.info('TestRunner: BEGIN_NATIVE_INTERACTION_SAMPLE $label');
        await controller.beginNativeInteractionSample(label: label);
      case EndNativeInteractionSampleAction(:final label):
        log.info('TestRunner: END_NATIVE_INTERACTION_SAMPLE $label');
        await controller.endNativeInteractionSample(label: label);
      case DebugNativeTimingAction():
        final info = await controller.getDiagnostics();
        final dartViewport = automation.dartViewportDiagnostics();
        String value(String key) => '${info[key] ?? ''}';
        String dartValue(String key) => '${dartViewport[key] ?? ''}';
        final label = action.label.isEmpty ? '' : 'stage=${action.label} ';
        log.info(
          'TestRunner: DEBUG_NATIVE_TIMING '
          '$label'
          'frameAvailable=${value('frameAvailableCount')}@${value('frameAvailableHz')}Hz '
          'callbackQueued=${value('macosFrameCallbackQueuedCount')}@${value('macosFrameCallbackQueuedHz')}Hz '
          'callbackProcessed=${value('macosFrameCallbackProcessedCount')}@${value('macosFrameCallbackProcessedHz')}Hz '
          'callbackCoalesced=${value('macosFrameCallbackCoalescedCount')}@${value('macosFrameCallbackCoalescedHz')}Hz '
          'callbackInline=${value('macosFrameCallbackInlineDirtyDrainCount')} '
          'callbackWaitP95Ms=${value('macosFrameCallbackMainWaitP95Ms')} '
          'callbackHandleP95Ms=${value('macosFrameCallbackHandleP95Ms')} '
          'decodeFps=${value('primaryTrackDecodeFps')} '
          'decodeAvgMs=${value('primaryTrackDecodeAvgMs')} '
          'decodeMaxMs=${value('primaryTrackDecodeMaxMs')} '
          'decodeFrames=${value('primaryTrackFramesDecoded')} '
          'trackBuf=${value('primaryTrackBufferCount')}/${value('primaryTrackBufferCapacity')} '
          'trackCurrentPtsUs=${value('primaryTrackCurrentPtsUs')} '
          'decSendAvgMs=${value('primaryTrackDecodeStagePacketSendAvgMs')} '
          'decSendMaxMs=${value('primaryTrackDecodeStagePacketSendMaxMs')} '
          'decRecvAvgMs=${value('primaryTrackDecodeStageReceiveAvgMs')} '
          'decRecvMaxMs=${value('primaryTrackDecodeStageReceiveMaxMs')} '
          'decRecvFrames=${value('primaryTrackDecodeStageReceiveFrameCount')} '
          'decConvertAvgMs=${value('primaryTrackDecodeStageConvertAvgMs')} '
          'decConvertMaxMs=${value('primaryTrackDecodeStageConvertMaxMs')} '
          'decConvertPlanarAvgMs=${value('primaryTrackDecodeStageConvertDirectPlanarAvgMs')} '
          'decConvertPlanarMaxMs=${value('primaryTrackDecodeStageConvertDirectPlanarMaxMs')} '
          'decConvertNv12LayoutAvgMs=${value('primaryTrackDecodeStageConvertNv12LayoutAvgMs')} '
          'decConvertNv12AllocAvgMs=${value('primaryTrackDecodeStageConvertNv12AllocAvgMs')} '
          'decConvertNv12PackAvgMs=${value('primaryTrackDecodeStageConvertNv12PackAvgMs')} '
          'decConvertNv12PackMaxMs=${value('primaryTrackDecodeStageConvertNv12PackMaxMs')} '
          'decPublishAvgMs=${value('primaryTrackDecodeStagePublishAvgMs')} '
          'decPublishMaxMs=${value('primaryTrackDecodeStagePublishMaxMs')} '
          'decPublishLockAvgMs=${value('primaryTrackDecodeStagePublishLockAvgMs')} '
          'decPublishWaitAvgMs=${value('primaryTrackDecodeStagePublishWaitAvgMs')} '
          'decPublishWaitMaxMs=${value('primaryTrackDecodeStagePublishWaitMaxMs')} '
          'decPublishRingPushAvgMs=${value('primaryTrackDecodeStagePublishRingPushAvgMs')} '
          'decPublishRingPushMaxMs=${value('primaryTrackDecodeStagePublishRingPushMaxMs')} '
          'decPublishRingAssignAvgMs=${value('primaryTrackDecodeStagePublishRingAssignAvgMs')} '
          'decPublishRingAssignMaxMs=${value('primaryTrackDecodeStagePublishRingAssignMaxMs')} '
          'decPublishRingOverwriteAvgBytes=${value('primaryTrackDecodeStagePublishRingOverwriteAvgBytes')} '
          'decPublishRingOverwriteMaxBytes=${value('primaryTrackDecodeStagePublishRingOverwriteMaxBytes')} '
          'decFlushAvgMs=${value('primaryTrackDecodeStageFlushAvgMs')} '
          'decFlushMaxMs=${value('primaryTrackDecodeStageFlushMaxMs')} '
          'targetGen=${value('macosFrameCallbackTargetGeneration')} '
          'targetGenChanges=${value('macosFrameCallbackTargetGenerationChangeCount')} '
          'targetWarmupSamples=${value('macosFrameCallbackTargetWarmupSampleCount')} '
          'targetWarmupP95Ms=${value('macosFrameCallbackTargetWarmupP95Ms')} '
          'nativeUploadP95Ms=${value('rendererOwnedUploadIntervalP95Ms')} '
          'nativeTargetWarmupSamples=${value('rendererOwnedTargetWarmupSampleCount')} '
          'nativeTargetWarmupP95Ms=${value('rendererOwnedTargetWarmupP95Ms')} '
          'presentation=${value('nativeFramePresentationCount')}@${value('nativeFramePresentationFps')}Hz '
          'rendererOwned=${value('nativeFrameRendererOwnedPresentCount')} '
          'rendererOwnedRatioX1000=${value('nativeFrameRendererOwnedRatioX1000')} '
          'ptsSamples=${value('presentedFramePtsSampleCount')} '
          'ptsDistinct=${value('presentedFramePtsDistinctCount')} '
          'ptsDuplicate=${value('presentedFramePtsDuplicateCount')} '
          'ptsLargeGap=${value('presentedFramePtsLargeGapCount')} '
          'hostAvgMs=${value('presentedFrameHostIntervalAvgMs')} '
          'hostP95Ms=${value('presentedFrameHostIntervalP95Ms')} '
          'hostMaxMs=${value('presentedFrameHostIntervalMaxMs')} '
          'expectedIntervalUs=${value('presentedFrameExpectedIntervalUs')} '
          'drop=${value('presentedFrameDropCount')} '
          'errors=${value('presentedFrameErrorCount')} '
          'compositorFrames=${value('nativeCompositorFrames')} '
          'compositorHz=${value('nativeCompositorCompositeHz')} '
          'sourceCacheHz=${value('nativeCompositorSourceCacheHz')} '
          'sourceProjectionHz=${value('nativeCompositorSourceProjectionHz')} '
          'dartRawPanZoom=${dartValue('dartViewportPointerPanZoomUpdateCount')}@${dartValue('dartViewportPointerPanZoomUpdateHz')}Hz '
          'dartPanZoomPan=${dartValue('dartViewportPointerPanZoomPanDispatchCount')}@${dartValue('dartViewportPointerPanZoomPanDispatchHz')}Hz '
          'dartPanZoomScale=${dartValue('dartViewportPointerPanZoomScaleDispatchCount')}@${dartValue('dartViewportPointerPanZoomScaleDispatchHz')}Hz '
          'dartMousePan=${dartValue('dartViewportPointerMovePanDispatchCount')}@${dartValue('dartViewportPointerMovePanDispatchHz')}Hz '
          'dartActionPan=${dartValue('dartViewportViewportActionPanCount')}@${dartValue('dartViewportViewportActionPanHz')}Hz '
          'dartActionZoom=${dartValue('dartViewportViewportActionZoomCount')}@${dartValue('dartViewportViewportActionZoomHz')}Hz '
          'dartLayoutPan=${dartValue('dartViewportLayoutPanCount')}@${dartValue('dartViewportLayoutPanHz')}Hz '
          'dartLayoutZoom=${dartValue('dartViewportLayoutZoomCount')}@${dartValue('dartViewportLayoutZoomHz')}Hz '
          'dartProjectionPublish=${dartValue('dartViewportProjectionPublishAttemptCount')}@${dartValue('dartViewportProjectionPublishAttemptHz')}Hz '
          'dartProjectionPrepare=${dartValue('dartViewportProjectionPrepareCount')}@${dartValue('dartViewportProjectionPrepareHz')}Hz '
          'dartProjectionSend=${dartValue('dartViewportProjectionChannelSendCount')}@${dartValue('dartViewportProjectionChannelSendHz')}Hz '
          'dartProjectionSkippedIneligible=${dartValue('dartViewportProjectionPrepareSkippedIneligibleCount')} '
          'swiftProjectionReceive=${value('nativeCompositorSourceProjectionMethodReceiveCount')}@${value('nativeCompositorSourceProjectionMethodReceiveHz')}Hz '
          'swiftProjectionApply=${value('nativeCompositorSourceProjectionApplyCount')}@${value('nativeCompositorSourceProjectionHz')}Hz '
          'windowsPhase=${value('windowsNativeCompositorPhase')} '
          'windowsMode=${value('windowsHotPathMode')} '
          'windowsHotPath=${value('windowsHotPathActive')} '
          'windowsDisplayHz=${value('windowsHotPathDisplayHz')} '
          'windowsBudgetUs=${value('windowsHotPathFrameBudgetUs')} '
          'windowsPresentP95Us=${value('windowsHotPathPresentIntervalP95Us')} '
          'windowsCompositeP95Us=${value('windowsHotPathCompositeP95Us')} '
          'windowsDrawP95Us=${value('windowsHotPathDrawP95Us')} '
          'windowsPresentBlockP95Us=${value('windowsHotPathPresentBlockP95Us')} '
          'windowsAcquireWaitP95Us=${value('windowsHotPathAcquireWaitP95Us')} '
          'windowsInputToPresentP95Us=${value('windowsHotPathInputToPresentP95Us')} '
          'windowsDropRateX1000=${value('windowsHotPathDropRateX1000')} '
          'windowsProjectionUpdates=${value('windowsHotPathProjectionOnlyUpdateCount')} '
          'windowsProjectionRedraws=${value('windowsHotPathViewportRedrawDuringProjectionCount')} '
          'windowsSourceReuse=${value('windowsHotPathSourceCacheReuseCount')} '
          'windowsOverlayReuse=${value('windowsHotPathOverlayReuseCount')} '
          'windowsOverlayRaster=${value('windowsHotPathOverlayRasterCount')} '
          'windowsOverlayUpload=${value('windowsHotPathOverlayUploadCount')} '
          'windowsRetainedActive=${value('windowsRetainedGraphActive')} '
          'windowsRetainedMode=${value('windowsRetainedGraphMode')} '
          'windowsRetainedFallback=${value('windowsRetainedGraphFallbackReason')} '
          'windowsRetainedCommit=${value('windowsRetainedGraphCommitCount')} '
          'windowsRetainedProjectionCommit=${value('windowsRetainedGraphProjectionCommitCount')} '
          'windowsRetainedSourceBake=${value('windowsRetainedGraphSourceBakeCount')} '
          'windowsRetainedFlutterBake=${value('windowsRetainedGraphFlutterBakeCount')} '
          'windowsRetainedSkipPresent=${value('windowsRetainedGraphProjectionSkipPresentCount')} '
          'windowsRetainedDeferredContent=${value('windowsRetainedGraphDeferredContentCount')} '
          'windowsRetainedCommitDefer=${value('windowsRetainedGraphCommitDeferCount')} '
          'windowsRetainedFlutterBakeP95Us=${value('windowsRetainedGraphFlutterBakeP95Us')} '
          'windowsRetainedSourceBakeP95Us=${value('windowsRetainedGraphSourceBakeP95Us')} '
          'windowsRetainedApplyP95Us=${value('windowsRetainedGraphApplyP95Us')} '
          'windowsRetainedCommitP95Us=${value('windowsRetainedGraphCommitP95Us')} '
          'windowsDCompPresent=${value('windowsDCompPresentCount')} '
          'windowsDCompComposite=${value('windowsDCompCompositeCount')} '
          'windowsDCompDrop=${value('windowsDCompDropCount')} '
          'windowsDCompPresentP95Us=${value('windowsDCompPresentIntervalP95Us')} '
          'windowsDCompCompositeP95Us=${value('windowsDCompCompositeP95Us')} '
          'windowsDCompDrawP95Us=${value('windowsDCompDrawP95Us')} '
          'windowsDCompPresentBlockP95Us=${value('windowsDCompPresentBlockP95Us')} '
          'windowsDCompAcquireWaitP95Us=${value('windowsDCompAcquireWaitP95Us')} '
          'windowsExportPublish=${value('windowsFlutterExportPublishCount')} '
          'windowsExportRequest=${value('windowsFlutterExportRequestCount')} '
          'windowsExportDispatch=${value('windowsFlutterExportRequestDispatchCount')} '
          'windowsExportVsync=${value('windowsFlutterExportVsyncCount')} '
          'windowsExportPresent=${value('windowsFlutterExportPresentCount')} '
          'windowsExportFlush=${value('windowsFlutterExportFlushCount')} '
          'windowsExportFinish=${value('windowsFlutterExportFinishCount')} '
          'windowsExportBackpressure=${value('windowsFlutterExportBackpressureCount')} '
          'windowsExportPending=${value('windowsFlutterExportPendingFramePumpFrames')} '
          'windowsExportStale=${value('windowsFlutterExportStaleTimeoutCount')} '
          'windowsExportUnrequestedSignal=${value('windowsFlutterExportUnrequestedSignalCount')} '
          'windowsExportUnrequestedThrottle=${value('windowsFlutterExportUnrequestedThrottleCount')} '
          'sourceRingBake=${value('sourceRingBakeCount')}@${value('sourceRingBakeHz')}Hz '
          'sourceRingBakeP95Ms=${value('sourceRingBakeP95Ms')} '
          'sourceRingBakeLastMs=${value('sourceRingBakeLastMs')} '
          'sourceRingReq=${value('sourceRingRefreshRequestCount')}@${value('sourceRingRefreshRequestHz')}Hz '
          'sourceRingQueueP95Ms=${value('sourceRingRefreshQueueWaitP95Ms')} '
          'sourceRingPublish=${value('sourceRingPublishCount')}@${value('sourceRingPublishHz')}Hz '
          'sourceRingTopology=${value('sourceRingTopologyRevision')} '
          'sourceRingRequiredMask=${value('sourceRingRequiredMask')} '
          'sourceRingDrawnMask=${value('sourceRingDrawnMask')} '
          'sourceRingMissingMask=${value('sourceRingMissingMask')} '
          'sourceRingIncompleteSuppressed=${value('sourceRingIncompletePublishSuppressedCount')} '
          'sourceRingIncompleteReason=${value('sourceRingLastIncompleteReason')} '
          'sourceRingSlotSig=${value('sourceRingPublishedSlotSignature')} '
          'sourceRingFileIds=${value('sourceRingPublishedFileIdSignature')} '
          'sourceRingActualFileIds=${value('sourceRingPublishedActualFileIdSignature')} '
          'sourceRingDupSlots=${value('sourceRingPublishedDuplicateSlotCount')} '
          'sourceRingDupFileIds=${value('sourceRingPublishedDuplicateFileIdCount')} '
          'sourceRingDupActualFileIds=${value('sourceRingPublishedDuplicateActualFileIdCount')} '
          'sourceRingDupBuffers=${value('sourceRingPublishedDuplicateBufferCount')} '
          'sourceReadySlotSig=${value('nativeCompositorSourceSlotSignature')} '
          'sourceReadyDupSlots=${value('nativeCompositorSourceDuplicateSlotCount')} '
          'sourceReadyDupFileIds=${value('nativeCompositorSourceDuplicateFileIdCount')} '
          'sourceReadyDupTextures=${value('nativeCompositorSourceDuplicateTextureCount')} '
          'sourceRingReqToPubP95Ms=${value('sourceRingRequestToPublishP95Ms')} '
          'sourceRingPtsUs=${value('sourceRingLastPublishedPtsUs')} '
          'sourceRingDurationUs=${value('sourceRingLastPublishedDurationUs')} '
          'sourceRingPtsStepP95Ms=${value('sourceRingPublishedPtsStepP95Ms')} '
          'sourceRingPtsDuplicate=${value('sourceRingPublishedPtsDuplicateCount')} '
          'sourceRingPtsLargeStep=${value('sourceRingPublishedPtsLargeStepCount')} '
          'sourceRingPtsRegression=${value('sourceRingPublishedPtsRegressionCount')} '
          'softwareStorage=${value('softwareFrameStorageKind')} '
          'softwarePackFallback=${value('softwareFramePackFallbackCount')} '
          'sourceRingCoalesced=${value('sourceRingRefreshCoalescedCount')} '
          'sourceRingMiss=${value('sourceRingPublishMissCount')} '
          'traceHz=${value('nativeCompositorTraceHz')} '
          'traceReceived=${value('nativeCompositorTraceReceivedCount')} '
          'traceApplied=${value('nativeCompositorTraceAppliedCount')} '
          'traceComposited=${value('nativeCompositorTraceCompositedCount')} '
          'traceCoalesced=${value('nativeCompositorTraceCoalescedBeforeCompositeCount')} '
          'traceLastRoute=${value('nativeCompositorTraceLastRoute')} '
          'dartToSwiftP95Ms=${value('nativeCompositorDartToSwiftP95Ms')} '
          'swiftQueueP95Ms=${value('nativeCompositorSwiftQueueP95Ms')} '
          'receiveToCompositeP95Ms=${value('nativeCompositorReceiveToCompositeP95Ms')} '
          'producerSubmit=${value('rendererOwnedCompositeProducerSubmitCount')} '
          'readyVideoP95Ms=${value('readyVideoAcquireP95Ms')} '
          'readySourceP95Ms=${value('readySourceAcquireP95Ms')} '
          'producerVideoHz=${value('producerVideoPublishHz')} '
          'producerSourceHz=${value('producerSourcePublishHz')} '
          'reuseVideo=${value('displayTickReuseVideoCount')} '
          'reuseSource=${value('displayTickReuseSourceCount')} '
          'blockedProducer=${value('displayTickBlockedProducerCount')} '
          'skippedInFlight=${value('nativeCompositorSkippedInFlightFrames')} '
          'skippedStatic=${value('nativeCompositorSkippedStaticFrames')} '
          'layoutIntent=${value('layoutIntentCount')} '
          'layoutSubmit=${value('layoutSubmitCount')} '
          'layoutDraw=${value('layoutDrawCount')} '
          'layoutPublished=${value('layoutPublishedCount')} '
          'displayTicks=${value('displayDeliveredTickCount')} '
          'viewportComposite=${value('viewportCompositeCount')} '
          'sourceHits=${value('sourceFrameCacheHitCount')} '
          'flutterTextureFrameSkippedPlaying=${value('flutterTextureFrameAvailableSkippedWhilePlayingCount')} '
          'compositorVideoRefresh=${value('compositorVideoTextureRefreshCount')} '
          'compositorVideoRefreshSkippedPlaying=${value('compositorVideoTextureRefreshSkippedWhilePlayingCount')} '
          'targetRebuild=${value('pixelBufferRebuildCount')} '
          'targetAlloc=${value('pixelBufferAllocationCount')} '
          'targetRebuildReuse=${value('pixelBufferRebuildReuseCount')} '
          'targetLastAlloc=${value('pixelBufferRebuildLastAllocatedCount')} '
          'targetLastReuse=${value('pixelBufferRebuildLastReusedCount')} '
          'targetLastMs=${value('pixelBufferRebuildLastDurationMs')} '
          'retiredBuffers=${value('retiredPixelBufferCount')} '
          'prewarm=${value('pixelBufferPrewarmRequestCount')}/'
          '${value('pixelBufferPrewarmReadyCount')}/'
          '${value('pixelBufferPrewarmHitCount')}/'
          '${value('pixelBufferPrewarmDroppedCount')} '
          'metalExhaustion=${value('textureMetalBufferExhaustionCount')} '
          'inFlightMetal=${value('textureInFlightMetalBufferCount')}',
        );
      case DebugFlutterTimingAction():
        final summary = await _flutterTimingProbe.collectAndReset();
        log.info(
          'TestRunner: DEBUG_FLUTTER_TIMING '
          'frames=${summary.frameCount} '
          'buildAvgMs=${summary.buildAvgMs} '
          'buildP95Ms=${summary.buildP95Ms} '
          'buildMaxMs=${summary.buildMaxMs} '
          'rasterAvgMs=${summary.rasterAvgMs} '
          'rasterP95Ms=${summary.rasterP95Ms} '
          'rasterMaxMs=${summary.rasterMaxMs} '
          'totalAvgMs=${summary.totalAvgMs} '
          'totalP95Ms=${summary.totalP95Ms} '
          'totalMaxMs=${summary.totalMaxMs} '
          'over16ms=${summary.over16Ms} '
          'over33ms=${summary.over33Ms}',
        );
      case AssertFlutterTimingAction(
        :final minFrames,
        :final maxTotalP95Ms,
        :final maxOver33Ms,
        :final maxOver16Ms,
      ):
        final summary = await _flutterTimingProbe.collectAndReset();
        log.info(
          'TestRunner: ASSERT_FLUTTER_TIMING '
          'frames=${summary.frameCount} '
          'minFrames=$minFrames '
          'totalP95Ms=${summary.totalP95Ms} '
          'maxTotalP95Ms=$maxTotalP95Ms '
          'over16ms=${summary.over16Ms} '
          'maxOver16Ms=$maxOver16Ms '
          'over33ms=${summary.over33Ms} '
          'maxOver33Ms=$maxOver33Ms',
        );
        final maxTotalP95Us = maxTotalP95Ms * 1000;
        if (summary.frameCount < minFrames ||
            summary.totalP95Us > maxTotalP95Us ||
            summary.over33Ms > maxOver33Ms ||
            summary.over16Ms > maxOver16Ms) {
          throw AssertionError(
            'Flutter timing outside limits: '
            'frames=${summary.frameCount} minFrames=$minFrames '
            'totalP95Ms=${summary.totalP95Ms} maxTotalP95Ms=$maxTotalP95Ms '
            'over16ms=${summary.over16Ms} maxOver16Ms=$maxOver16Ms '
            'over33ms=${summary.over33Ms} maxOver33Ms=$maxOver33Ms',
          );
        }
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
      case ClickFlutterPoint(:final x, :final y):
        log.info(
          'TestRunner: CLICK_FLUTTER_POINT '
          'x=${x.toStringAsFixed(1)} y=${y.toStringAsFixed(1)}',
        );
        testHarness.clickFlutterPoint(Offset(x, y));
      case ClickControlsPlayButton():
        log.info('TestRunner: CLICK_CONTROLS_PLAY_BUTTON');
        testHarness.clickControlsPlayButton();
      case ClickToolbarMediaInfoNative():
        log.info('TestRunner: CLICK_TOOLBAR_MEDIA_INFO_NATIVE');
        await testHarness.clickToolbarMediaInfoNative();
      case DragViewport(:final dx, :final dy, :final steps, :final stepMs):
        log.info(
          'TestRunner: DRAG_VIEWPORT dx=$dx dy=$dy steps=$steps stepMs=$stepMs',
        );
        await testHarness.dragViewport(
          Offset(dx, dy),
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
        );
      case DragViewportNative(
        :final dx,
        :final dy,
        :final steps,
        :final stepMs,
        :final button,
      ):
        log.info(
          'TestRunner: DRAG_VIEWPORT_NATIVE dx=$dx dy=$dy steps=$steps '
          'stepMs=$stepMs button=$button',
        );
        await testHarness.dragViewportNative(
          Offset(dx, dy),
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
          button: button,
        );
      case DragSplitHandleNative(
        :final targetFraction,
        :final steps,
        :final stepMs,
      ):
        log.info(
          'TestRunner: DRAG_SPLIT_HANDLE_NATIVE target=$targetFraction '
          'steps=$steps stepMs=$stepMs',
        );
        await testHarness.dragSplitHandleNative(
          targetFraction,
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
        );
      case WheelViewportNative(
        :final delta,
        :final steps,
        :final stepMs,
        :final xFraction,
        :final yFraction,
      ):
        log.info(
          'TestRunner: WHEEL_VIEWPORT_NATIVE delta=$delta steps=$steps '
          'stepMs=$stepMs at=($xFraction,$yFraction)',
        );
        await testHarness.wheelViewportNative(
          delta: delta,
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
          xFraction: xFraction,
          yFraction: yFraction,
        );
      case PanZoomViewport(
        :final panDx,
        :final panDy,
        :final scale,
        :final steps,
        :final stepMs,
        :final xFraction,
        :final yFraction,
      ):
        log.info(
          'TestRunner: PAN_ZOOM_VIEWPORT pan=($panDx,$panDy) '
          'scale=$scale steps=$steps stepMs=$stepMs at=($xFraction,$yFraction)',
        );
        await testHarness.panZoomViewport(
          panDelta: Offset(panDx, panDy),
          scale: scale,
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
          xFraction: xFraction,
          yFraction: yFraction,
        );
      case DragViewportSampleOverlay(
        :final dx,
        :final dy,
        :final steps,
        :final stepMs,
        :final minScoreRatio,
        :final maxDropSamples,
      ):
        log.info(
          'TestRunner: DRAG_VIEWPORT_SAMPLE_OVERLAY dx=$dx dy=$dy '
          'steps=$steps stepMs=$stepMs minScoreRatio=$minScoreRatio '
          'maxDropSamples=$maxDropSamples',
        );
        final metric = await testHarness.dragViewportAndSampleOverlay(
          Offset(dx, dy),
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
          minScoreRatio: minScoreRatio,
          maxDropSamples: maxDropSamples,
        );
        log.info(metric.summary());
        if (!metric.stable) {
          throw AssertionError(metric.failureMessage);
        }
      case DragViewportSampleNativeDiagnosticBool(
        :final dx,
        :final dy,
        :final key,
        :final value,
        :final steps,
        :final stepMs,
        :final minMatches,
      ):
        log.info(
          'TestRunner: DRAG_VIEWPORT_SAMPLE_NATIVE_DIAGNOSTIC_BOOL '
          'dx=$dx dy=$dy key=$key value=$value steps=$steps '
          'stepMs=$stepMs minMatches=$minMatches',
        );
        var samples = 0;
        final matches = await testHarness.dragViewportAndSample(
          Offset(dx, dy),
          steps: steps,
          stepDelay: Duration(milliseconds: stepMs),
          sample: (label) async {
            samples++;
            final diagnostics = await controller.getDiagnostics();
            final actual = diagnostics[key];
            final matched = actual == value;
            log.info(
              'Test action: DRAG_VIEWPORT_SAMPLE_NATIVE_DIAGNOSTIC_BOOL '
              'sample=$label key=$key actual=$actual expected=$value '
              'matched=$matched',
            );
            return matched;
          },
        );
        if (matches < minMatches) {
          throw AssertionError(
            'Expected native diagnostic $key=$value during drag at least '
            '$minMatches time(s), got $matches/$samples samples',
          );
        }
      case AssertViewportOverlayLineStyle(
        :final minPairedCenters,
        :final minPairedRatio,
      ):
        final capture = await controller.captureViewport();
        if (!capture.overlayLineStyleMetricsAvailable) {
          throw AssertionError(
            'Viewport overlay line style metrics are unavailable for '
            '${capture.hash}',
          );
        }
        final metric = _ViewportOverlayLineStyleMetric(
          pairedCenters: capture.overlayLinePairedCenters,
          weakWhiteCenters: capture.overlayLineWeakWhiteCenters,
          blackOnlyCenters: capture.overlayLineBlackOnlyCenters,
        );
        log.info(metric.summary());
        if (!metric.meets(
          minPairedCenters: minPairedCenters,
          minPairedRatio: minPairedRatio,
        )) {
          throw AssertionError(
            metric.failureMessage(
              minPairedCenters: minPairedCenters,
              minPairedRatio: minPairedRatio,
            ),
          );
        }
      case HoverControlsBarButtons(:final steps):
        log.info('TestRunner: HOVER_CONTROLS_BAR_BUTTONS steps=$steps');
        testHarness.hoverControlsBarButtons(steps: steps);
      case HoverControlsBarButtonsNative(:final steps):
        log.info('TestRunner: HOVER_CONTROLS_BAR_BUTTONS_NATIVE steps=$steps');
        await testHarness.hoverControlsBarButtonsNative(steps: steps);
      case HoverTimeline(:final steps, :final stepMs):
        log.info('TestRunner: HOVER_TIMELINE steps=$steps stepMs=$stepMs');
        await testHarness.hoverTimeline(steps: steps, stepMs: stepMs);
      case ClickMediaHeaderOverlayButtonNative():
        log.info('TestRunner: CLICK_MEDIA_HEADER_OVERLAY_BUTTON_NATIVE');
        await testHarness.clickAnalysisOverlayButtonNative();
      case ClickMediaHeaderOverlayButton():
        log.info('TestRunner: CLICK_MEDIA_HEADER_OVERLAY_BUTTON');
        testHarness.clickAnalysisOverlayButton();
      case ClickMediaHeaderRemoveButton(:final fileId):
        log.info('TestRunner: CLICK_MEDIA_HEADER_REMOVE_BUTTON fileId=$fileId');
        testHarness.clickMediaHeaderRemoveButton(fileId);
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
      case ClearMarks():
        log.info('TestRunner: CLEAR_MARKS');
        automation.clearMarks();
      case ToggleMarksSidebar():
        log.info('TestRunner: TOGGLE_MARKS_SIDEBAR');
        automation.toggleMarksSidebar();
      case AddQuickMark():
        log.info(
          'TestRunner: ADD_QUICK_MARK slot=${action.slotIndex} '
          'rect=(${action.left},${action.top},'
          '${action.width},${action.height}) '
          'defect=${action.defectType} severity=${action.severity}',
        );
        await automation.addQuickMark(action);
      case ExportMarks(:final outputPath):
        final resolvedPath = _resolveCaptureOutputPath(outputPath)!;
        log.info('TestRunner: EXPORT_MARKS $resolvedPath');
        await automation.exportMarksToFile(resolvedPath);
      case SetMediaSourceId(:final slotIndex, :final sourceId):
        log.info(
          'TestRunner: SET_MEDIA_SOURCE_ID slot=$slotIndex sourceId=$sourceId',
        );
        await automation.setMediaSourceIdForSlot(slotIndex, sourceId);
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

  String? _resolveCaptureOutputPath(String? outputPath) {
    if (outputPath == null || outputPath.trim().isEmpty) {
      return outputPath;
    }
    if (!Platform.isMacOS || p.isAbsolute(outputPath)) {
      return outputPath;
    }
    return p.join(File(scriptPath).parent.path, outputPath);
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

  Future<void> _executeWaitTrackCount(int count, Duration timeout) async {
    final sw = Stopwatch()..start();
    var lastCount = -1;
    while (sw.elapsed < timeout) {
      final tracks = await controller.getTracks();
      lastCount = tracks.length;
      if (lastCount == count) return;
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    throw AssertionError(
      'WAIT_TRACK_COUNT timed out after ${timeout.inMilliseconds}ms: '
      'expected $count, got $lastCount',
    );
  }

  Future<void> _executeWaitPresentedFrameRange({
    required int fileId,
    required int minUs,
    required int maxUs,
    required Duration timeout,
    required Duration interval,
  }) async {
    final sw = Stopwatch()..start();
    int? lastPtsUs;
    while (sw.elapsed < timeout) {
      final timing = await controller.currentPresentedFrame(fileId);
      lastPtsUs = timing?.ptsUs;
      if (lastPtsUs != null && lastPtsUs >= minUs && lastPtsUs <= maxUs) {
        return;
      }
      await Future<void>.delayed(interval);
    }
    throw AssertionError(
      'WAIT_PRESENTED_FRAME_RANGE timed out after ${timeout.inMilliseconds}ms: '
      'expected fileId=$fileId in [$minUs, $maxUs] μs, got $lastPtsUs',
    );
  }
}

class _FlutterFrameTimingProbe {
  static final _FlutterFrameTimingProbe instance = _FlutterFrameTimingProbe._();

  final List<FrameTiming> _timings = <FrameTiming>[];
  bool _installed = false;

  _FlutterFrameTimingProbe._();

  bool ensureInstalled() {
    if (_installed) return true;
    SchedulerBinding? binding;
    try {
      binding = SchedulerBinding.instance;
    } catch (_) {
      return false;
    }
    binding.addTimingsCallback(_onTimings);
    _installed = true;
    return true;
  }

  void reset() {
    _timings.clear();
  }

  Future<_FlutterFrameTimingSummary> collectAndReset() async {
    if (!ensureInstalled()) {
      return _FlutterFrameTimingSummary.fromTimings(const <FrameTiming>[]);
    }
    SchedulerBinding.instance.scheduleFrame();
    await SchedulerBinding.instance.endOfFrame;
    await Future<void>.delayed(const Duration(milliseconds: 1));
    final snapshot = List<FrameTiming>.of(_timings);
    _timings.clear();
    return _FlutterFrameTimingSummary.fromTimings(snapshot);
  }

  void _onTimings(List<FrameTiming> timings) {
    _timings.addAll(timings);
  }
}

class _FlutterFrameTimingSummary {
  final int frameCount;
  final String buildAvgMs;
  final String buildP95Ms;
  final String buildMaxMs;
  final String rasterAvgMs;
  final String rasterP95Ms;
  final String rasterMaxMs;
  final String totalAvgMs;
  final String totalP95Ms;
  final String totalMaxMs;
  final int totalP95Us;
  final int over16Ms;
  final int over33Ms;

  const _FlutterFrameTimingSummary({
    required this.frameCount,
    required this.buildAvgMs,
    required this.buildP95Ms,
    required this.buildMaxMs,
    required this.rasterAvgMs,
    required this.rasterP95Ms,
    required this.rasterMaxMs,
    required this.totalAvgMs,
    required this.totalP95Ms,
    required this.totalMaxMs,
    required this.totalP95Us,
    required this.over16Ms,
    required this.over33Ms,
  });

  factory _FlutterFrameTimingSummary.fromTimings(List<FrameTiming> timings) {
    if (timings.isEmpty) {
      return const _FlutterFrameTimingSummary(
        frameCount: 0,
        buildAvgMs: '0.000',
        buildP95Ms: '0.000',
        buildMaxMs: '0.000',
        rasterAvgMs: '0.000',
        rasterP95Ms: '0.000',
        rasterMaxMs: '0.000',
        totalAvgMs: '0.000',
        totalP95Ms: '0.000',
        totalMaxMs: '0.000',
        totalP95Us: 0,
        over16Ms: 0,
        over33Ms: 0,
      );
    }

    final build = timings
        .map((timing) => timing.buildDuration.inMicroseconds)
        .toList(growable: false);
    final raster = timings
        .map((timing) => timing.rasterDuration.inMicroseconds)
        .toList(growable: false);
    final total = timings
        .map((timing) => timing.totalSpan.inMicroseconds)
        .toList(growable: false);
    final totalP95Us = _percentileUs(total, 0.95);
    return _FlutterFrameTimingSummary(
      frameCount: timings.length,
      buildAvgMs: _formatMs(_averageUs(build)),
      buildP95Ms: _formatMs(_percentileUs(build, 0.95)),
      buildMaxMs: _formatMs(_maxUs(build)),
      rasterAvgMs: _formatMs(_averageUs(raster)),
      rasterP95Ms: _formatMs(_percentileUs(raster, 0.95)),
      rasterMaxMs: _formatMs(_maxUs(raster)),
      totalAvgMs: _formatMs(_averageUs(total)),
      totalP95Ms: _formatMs(totalP95Us),
      totalMaxMs: _formatMs(_maxUs(total)),
      totalP95Us: totalP95Us,
      over16Ms: total.where((us) => us > 16000).length,
      over33Ms: total.where((us) => us > 33000).length,
    );
  }

  static double _averageUs(List<int> values) {
    if (values.isEmpty) return 0;
    return values.reduce((a, b) => a + b) / values.length;
  }

  static int _maxUs(List<int> values) {
    if (values.isEmpty) return 0;
    return values.reduce((a, b) => a > b ? a : b);
  }

  static int _percentileUs(List<int> values, double percentile) {
    if (values.isEmpty) return 0;
    final sorted = List<int>.of(values)..sort();
    final index = ((sorted.length - 1) * percentile).ceil().clamp(
      0,
      sorted.length - 1,
    );
    return sorted[index];
  }

  static String _formatMs(num us) => (us / 1000.0).toStringAsFixed(3);
}

class _ViewportOverlayLineStyleMetric {
  final int pairedCenters;
  final int weakWhiteCenters;
  final int blackOnlyCenters;

  const _ViewportOverlayLineStyleMetric({
    required this.pairedCenters,
    required this.weakWhiteCenters,
    required this.blackOnlyCenters,
  });

  int get suspiciousCenters => weakWhiteCenters + blackOnlyCenters;

  int get classifiedCenters => pairedCenters + suspiciousCenters;

  double get pairedRatio =>
      classifiedCenters == 0 ? 0.0 : pairedCenters / classifiedCenters;

  bool meets({required int minPairedCenters, required double minPairedRatio}) =>
      pairedCenters >= minPairedCenters && pairedRatio >= minPairedRatio;

  String failureMessage({
    required int minPairedCenters,
    required double minPairedRatio,
  }) =>
      'Viewport overlay line style is unstable: '
      'pairedCenters=$pairedCenters weakWhiteCenters=$weakWhiteCenters '
      'blackOnlyCenters=$blackOnlyCenters '
      'pairedRatio=${pairedRatio.toStringAsFixed(3)} '
      'requiredPairedCenters=$minPairedCenters '
      'requiredPairedRatio=${minPairedRatio.toStringAsFixed(3)}';

  String summary() =>
      'ASSERT_VIEWPORT_OVERLAY_LINE_STYLE summary: '
      'pairedCenters=$pairedCenters weakWhiteCenters=$weakWhiteCenters '
      'blackOnlyCenters=$blackOnlyCenters '
      'pairedRatio=${pairedRatio.toStringAsFixed(3)}';
}
