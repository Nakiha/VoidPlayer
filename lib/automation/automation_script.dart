import 'dart:io';

import '../actions/automation_action.dart';
import '../actions/player_action.dart';
import '../actions/player_assert.dart';
import '../analysis/analysis_overlay.dart';
import '../app_log.dart';
import '../preferences/playback_preferences.dart';

/// A parsed instruction from a release UI automation script.
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
  final int ptsOffsetUs;

  const ScriptGenerateTestVideo(
    super.time, {
    required this.path,
    required this.frames,
    required this.fps,
    required this.width,
    required this.height,
    this.ptsOffsetUs = 0,
  });
}

class ScriptSetSeekAfterJumpBehavior extends ScriptInstruction {
  final SeekAfterJumpBehavior behavior;
  const ScriptSetSeekAfterJumpBehavior(super.time, this.behavior);
}

class ScriptSetDecodeMode extends ScriptInstruction {
  final DecodeMode mode;
  const ScriptSetDecodeMode(super.time, this.mode);
}

class ScriptSetViewportPixelSizeMode extends ScriptInstruction {
  final ViewportPixelSizeMode mode;
  const ScriptSetViewportPixelSizeMode(super.time, this.mode);
}

class ScriptQuit extends ScriptInstruction {
  final int exitCode;
  const ScriptQuit(super.time, this.exitCode);
}

/// Parse a CSV automation script file into scheduled instructions.
List<ScriptInstruction> parseAutomationScript(String path) {
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
    case 'ADD_NETWORK_MEDIA':
      if (args.isEmpty) {
        log.warning('ADD_NETWORK_MEDIA missing URL argument: $rawLine');
        return null;
      }
      return ScriptAction(time, AddNetworkMedia(args[0]));
    case 'ADD_SSH_MEDIA':
      if (args.isEmpty) {
        log.warning('ADD_SSH_MEDIA missing remote path argument: $rawLine');
        return null;
      }
      return ScriptAction(time, AddSshMedia(args[0]));
    case 'REMOVE_TRACK':
      if (args.isEmpty) {
        log.warning('REMOVE_TRACK missing slot argument: $rawLine');
        return null;
      }
      return ScriptAction(time, RemoveTrackAction(int.parse(args[0])));
    case 'SWAP_MEDIA_HEADER':
      if (args.length < 2) {
        log.warning(
          'SWAP_MEDIA_HEADER needs slotIndex and targetTrackIndex arguments: $rawLine',
        );
        return null;
      }
      return ScriptAction(
        time,
        SwapMediaHeader(int.parse(args[0]), int.parse(args[1])),
      );
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
    case 'OPEN_SETTINGS':
      return ScriptAction(time, const OpenSettings());
    case 'OPEN_STATS':
      return ScriptAction(time, const OpenStats());
    case 'OPEN_MEDIA_INFO':
      return ScriptAction(time, const OpenMediaInfo());
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
    case 'HOVER_CONTROLS_BAR_BUTTONS':
      return ScriptAutomationAction(
        time,
        HoverControlsBarButtons(
          steps: args.isNotEmpty ? int.parse(args[0]) : 24,
        ),
      );
    case 'HOVER_CONTROLS_BAR_BUTTONS_NATIVE':
      return ScriptAutomationAction(
        time,
        HoverControlsBarButtonsNative(
          steps: args.isNotEmpty ? int.parse(args[0]) : 24,
        ),
      );
    case 'CLICK_MEDIA_HEADER_OVERLAY_BUTTON_NATIVE':
      return ScriptAutomationAction(
        time,
        const ClickMediaHeaderOverlayButtonNative(),
      );
    case 'CLICK_MEDIA_HEADER_OVERLAY_BUTTON':
      return ScriptAutomationAction(
        time,
        const ClickMediaHeaderOverlayButton(),
      );
    case 'TOGGLE_ANALYSIS_OVERLAY':
      if (args.isEmpty) {
        log.warning('TOGGLE_ANALYSIS_OVERLAY needs slot index: $rawLine');
        return null;
      }
      return ScriptAutomationAction(
        time,
        ToggleAnalysisOverlay(int.parse(args[0])),
      );
    case 'TOGGLE_ANALYSIS_OVERLAY_PANEL':
      return ScriptAutomationAction(time, const ToggleAnalysisOverlayPanel());
    case 'SET_ANALYSIS_OVERLAY_TYPE':
      if (args.isEmpty) {
        log.warning('SET_ANALYSIS_OVERLAY_TYPE needs type: $rawLine');
        return null;
      }
      return ScriptAutomationAction(
        time,
        SetAnalysisOverlayType(analysisOverlayTypeFromName(args[0])),
      );
    case 'SET_ANALYSIS_OVERLAY_LAYERS':
      final layers = args
          .expand((arg) => arg.split('|'))
          .where((arg) => arg.trim().isNotEmpty)
          .map(analysisOverlayLayerFromName)
          .toSet();
      return ScriptAutomationAction(time, SetAnalysisOverlayLayers(layers));
    case 'SET_ANALYSIS_OVERLAY_OPACITY':
      if (args.isEmpty) {
        log.warning('SET_ANALYSIS_OVERLAY_OPACITY needs opacity: $rawLine');
        return null;
      }
      return ScriptAutomationAction(
        time,
        SetAnalysisOverlayOpacity(double.parse(args[0])),
      );
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
        ptsOffsetUs: args.length >= 6 ? int.parse(args[5]) : 0,
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
    case 'SET_DECODE_MODE':
      if (args.isEmpty) {
        log.warning('SET_DECODE_MODE missing mode argument: $rawLine');
        return null;
      }
      final mode = DecodeMode.fromStorage(args[0]);
      return ScriptSetDecodeMode(time, mode);
    case 'SET_VIEWPORT_PIXEL_SIZE_MODE':
      if (args.isEmpty) {
        log.warning(
          'SET_VIEWPORT_PIXEL_SIZE_MODE missing mode argument: $rawLine',
        );
        return null;
      }
      final mode = ViewportPixelSizeMode.fromStorage(args[0]);
      return ScriptSetViewportPixelSizeMode(time, mode);

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
    case 'ASSERT_TRACK_ORDER':
      if (args.isEmpty) {
        log.warning('ASSERT_TRACK_ORDER missing file_id arguments: $rawLine');
        return null;
      }
      return ScriptAssert(time, AssertTrackOrder(args.map(int.parse).toList()));
    case 'ASSERT_DURATION':
      if (args.length < 2) {
        log.warning('ASSERT_DURATION needs ptsUs and toleranceMs: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertDuration(int.parse(args[0]), int.parse(args[1])),
      );
    case 'ASSERT_EFFECTIVE_DURATION':
      if (args.length < 2) {
        log.warning(
          'ASSERT_EFFECTIVE_DURATION needs ptsUs and toleranceMs: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertEffectiveDuration(int.parse(args[0]), int.parse(args[1])),
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
    case 'ASSERT_CAPTURE_HAS_DETAIL':
      if (args.isEmpty) {
        log.warning('ASSERT_CAPTURE_HAS_DETAIL needs capture name: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertCaptureHasDetail(
          args[0],
          minLumaStdDev: args.length >= 2 ? double.parse(args[1]) : 4.0,
        ),
      );
    case 'ASSERT_CAPTURE_SPLIT_DIFF':
      if (args.isEmpty) {
        log.warning('ASSERT_CAPTURE_SPLIT_DIFF needs capture name: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertCaptureSplitDiff(
          args[0],
          maxMeanAbsChannel: args.length >= 2
              ? double.parse(args[1])
              : double.infinity,
          maxMeanAbsLuma: args.length >= 3
              ? double.parse(args[2])
              : double.infinity,
          maxMaxChannel: args.length >= 4
              ? double.parse(args[3])
              : double.infinity,
        ),
      );
    case 'ASSERT_CAPTURE_DIFF':
      if (args.length < 2) {
        log.warning(
          'ASSERT_CAPTURE_DIFF needs expected and actual capture names: $rawLine',
        );
        return null;
      }
      return ScriptAssert(
        time,
        AssertCaptureDiff(
          args[0],
          args[1],
          maxMeanAbsChannel: args.length >= 3
              ? double.parse(args[2])
              : double.infinity,
          maxMeanAbsLuma: args.length >= 4
              ? double.parse(args[3])
              : double.infinity,
          maxMaxChannel: args.length >= 5
              ? double.parse(args[4])
              : double.infinity,
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
    case 'ASSERT_ANALYSIS_OVERLAY':
      if (args.isEmpty) {
        log.warning('ASSERT_ANALYSIS_OVERLAY needs active flag: $rawLine');
        return null;
      }
      return ScriptAssert(
        time,
        AssertAnalysisOverlay(
          active: args[0] == '1' || args[0].toLowerCase() == 'true',
          type: args.length >= 2 && args[1].trim().isNotEmpty
              ? analysisOverlayTypeFromName(args[1])
              : null,
          opacity: args.length >= 3 && args[2].trim().isNotEmpty
              ? double.parse(args[2])
              : null,
          opacityTolerance: args.length >= 4 ? double.parse(args[3]) : 0.02,
          trackCount: args.length >= 5 && args[4].trim().isNotEmpty
              ? int.parse(args[4])
              : null,
        ),
      );
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
          args.length >= 4 ? double.parse(args[3]) : null,
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
