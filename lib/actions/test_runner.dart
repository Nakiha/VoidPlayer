import 'dart:async';
import 'dart:io';

import '../app_log.dart';
import 'action_registry.dart';
import '../video_renderer_controller.dart';
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

class ScriptQuit extends ScriptInstruction {
  final int exitCode;
  const ScriptQuit(super.time, this.exitCode);
}

/// Parses a test script file and runs instructions on a timeline.
class TestRunner {
  final String scriptPath;
  final VideoRendererController controller;

  TestRunner({required this.scriptPath, required this.controller});

  /// Parse and execute the test script. Exits the process on QUIT or failure.
  Future<void> run() async {
    final instructions = _parseScript(scriptPath);
    if (instructions.isEmpty) {
      log.severe('Test script is empty: $scriptPath');
      exit(1);
    }

    log.info('TestRunner: running ${instructions.length} instructions from $scriptPath');

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
        actionRegistry.execute(action.name, action);

      case ScriptAssert(:final assertion):
        log.info('TestRunner ${instr.time}: assert ${assertion.runtimeType}');
        await _executeAssert(assertion);

      case ScriptWait(:final state, :final timeout):
        log.info('TestRunner ${instr.time}: WAIT_${state.name.toUpperCase()} ${timeout.inMilliseconds}ms');
        await _executeWait(state, timeout);

      case ScriptQuit(:final exitCode):
        log.info('TestRunner ${instr.time}: QUIT $exitCode');
        exit(exitCode);
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
    }
  }

  Future<void> _executeWait(WaitState state, Duration timeout) async {
    final sw = Stopwatch()..start();
    while (sw.elapsed < timeout) {
      final satisfied = switch (state) {
        WaitState.playing => await controller.isPlaying(),
        WaitState.paused  => !await controller.isPlaying(),
      };
      if (satisfied) return;
      await Future.delayed(const Duration(milliseconds: 50));
    }
    throw AssertionError('WAIT_${state.name.toUpperCase()} timed out after ${timeout.inMilliseconds}ms');
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
    if (line.isEmpty || line.startsWith('#')) continue;

    final parts = line.split(',').map((s) => s.trim()).toList();
    if (parts.length < 2) {
      log.warning('Test script line ${i + 1}: invalid format: $line');
      continue;
    }

    final time = Duration(milliseconds: (double.parse(parts[0]) * 1000).round());
    final cmd = parts[1].toUpperCase();

    final instr = _parseInstruction(time, cmd, parts.sublist(2), line);
    if (instr != null) instructions.add(instr);
  }

  // Sort by time
  instructions.sort((a, b) => a.time.compareTo(b.time));
  return instructions;
}

ScriptInstruction? _parseInstruction(Duration time, String cmd, List<String> args, String rawLine) {
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
      return ScriptAction(time, Pan(double.parse(args[0]), double.parse(args[1])));

    // Waits
    case 'WAIT_PLAYING':
      final timeoutMs = args.isNotEmpty ? int.parse(args[0]) : 3000;
      return ScriptWait(time, WaitState.playing, Duration(milliseconds: timeoutMs));
    case 'WAIT_PAUSED':
      final timeoutMs = args.isNotEmpty ? int.parse(args[0]) : 3000;
      return ScriptWait(time, WaitState.paused, Duration(milliseconds: timeoutMs));

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
      return ScriptAssert(time, AssertPosition(int.parse(args[0]), int.parse(args[1])));
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
      return ScriptAssert(time, AssertDuration(int.parse(args[0]), int.parse(args[1])));

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
      return ScriptAssert(time, AssertZoom(double.parse(args[0]), double.parse(args[1])));

    // Control
    case 'QUIT':
      final exitCode = args.isNotEmpty ? int.parse(args[0]) : 0;
      return ScriptQuit(time, exitCode);

    default:
      log.warning('Unknown test script command: $cmd');
      return null;
  }
}
