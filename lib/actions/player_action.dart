import 'package:flutter/services.dart';

/// A shortcut entry for display in the settings UI.
typedef ShortcutEntry = ({
  String actionName,
  String labelKey,
  String shortcutLabel,
});

/// User actions that can be triggered by shortcuts, buttons, or test scripts.
sealed class PlayerAction {
  final String name;
  final LogicalKeyboardKey? shortcut;
  final String? labelKey;
  final bool requireControl;
  final bool repeatable;
  const PlayerAction(
    this.name, [
    this.shortcut,
    this.requireControl = false,
    this.repeatable = false,
    this.labelKey,
  ]);

  static const List<PlayerAction> shortcutCatalog = [
    TogglePlayPause(),
    StepForward(),
    StepBackward(),
    OpenFile(),
    ToggleLayoutMode(),
    ToggleFullScreen(),
    ExitFullScreen(),
  ];

  /// All real keyboard shortcuts for display in the settings UI.
  static List<ShortcutEntry> get shortcutEntries => [
    for (final action in shortcutCatalog)
      if (action.labelKey != null && action.shortcutLabel != null)
        (
          actionName: action.name,
          labelKey: action.labelKey!,
          shortcutLabel: action.shortcutLabel!,
        ),
  ];

  String? get shortcutLabel {
    final key = shortcut;
    if (key == null) return null;
    final keyLabel = _shortcutKeyLabel(key);
    if (keyLabel == null) return null;
    return requireControl ? 'Ctrl + $keyLabel' : keyLabel;
  }

  static String? _shortcutKeyLabel(LogicalKeyboardKey key) {
    if (key == LogicalKeyboardKey.space) return 'Space';
    if (key == LogicalKeyboardKey.arrowRight) return '→';
    if (key == LogicalKeyboardKey.arrowLeft) return '←';
    if (key == LogicalKeyboardKey.f11) return 'F11';
    if (key == LogicalKeyboardKey.escape) return 'Esc';
    final keyLabel = key.keyLabel;
    return keyLabel.isEmpty ? key.debugName : keyLabel;
  }
}

class TogglePlayPause extends PlayerAction {
  const TogglePlayPause()
    : super(
        'TOGGLE_PLAY_PAUSE',
        LogicalKeyboardKey.space,
        false,
        false,
        'actionTogglePlay',
      );
}

class Play extends PlayerAction {
  const Play() : super('PLAY');
}

class Pause extends PlayerAction {
  const Pause() : super('PAUSE');
}

class SeekTo extends PlayerAction {
  final int ptsUs;
  const SeekTo(this.ptsUs) : super('SEEK_TO');
}

class ClickTimelineFraction extends PlayerAction {
  final double fraction;
  const ClickTimelineFraction(this.fraction) : super('CLICK_TIMELINE_FRACTION');
}

class SetSpeed extends PlayerAction {
  final double speed;
  const SetSpeed(this.speed) : super('SET_SPEED');
}

class StepForward extends PlayerAction {
  const StepForward()
    : super(
        'STEP_FORWARD',
        LogicalKeyboardKey.arrowRight,
        false,
        true,
        'actionStepForward',
      );
}

class StepBackward extends PlayerAction {
  const StepBackward()
    : super(
        'STEP_BACKWARD',
        LogicalKeyboardKey.arrowLeft,
        false,
        true,
        'actionStepBackward',
      );
}

class OpenFile extends PlayerAction {
  const OpenFile()
    : super(
        'OPEN_FILE',
        LogicalKeyboardKey.keyO,
        false,
        false,
        'actionOpenFile',
      );
}

class ToggleLayoutMode extends PlayerAction {
  const ToggleLayoutMode()
    : super(
        'TOGGLE_LAYOUT_MODE',
        LogicalKeyboardKey.keyM,
        false,
        false,
        'actionToggleLayout',
      );
}

class ToggleFullScreen extends PlayerAction {
  const ToggleFullScreen()
    : super(
        'TOGGLE_FULL_SCREEN',
        LogicalKeyboardKey.f11,
        false,
        false,
        'actionToggleFullScreen',
      );
}

class ExitFullScreen extends PlayerAction {
  const ExitFullScreen()
    : super(
        'EXIT_FULL_SCREEN',
        LogicalKeyboardKey.escape,
        false,
        false,
        'actionExitFullScreen',
      );
}

class OpenSettings extends PlayerAction {
  const OpenSettings() : super('OPEN_SETTINGS');
}

class OpenStats extends PlayerAction {
  const OpenStats() : super('OPEN_STATS');
}

class OpenMediaInfo extends PlayerAction {
  const OpenMediaInfo() : super('OPEN_MEDIA_INFO');
}

class OpenMemory extends PlayerAction {
  const OpenMemory() : super('OPEN_MEMORY');
}

class RunAnalysis extends PlayerAction {
  const RunAnalysis() : super('RUN_ANALYSIS');
}

/// Add a media file by path (no file-picker dialog).
class AddMedia extends PlayerAction {
  final String path;
  const AddMedia(this.path) : super('ADD_MEDIA');
}

/// Add a network media stream by URL (no dialog).
class AddNetworkMedia extends PlayerAction {
  final String url;
  const AddNetworkMedia(this.url) : super('ADD_NETWORK_MEDIA');
}

/// Add an SSH remote media file by copying it into the local remote cache.
class AddSshMedia extends PlayerAction {
  final String remotePath;
  const AddSshMedia(this.remotePath) : super('ADD_SSH_MEDIA');
}

/// Set zoom ratio directly.
class SetZoom extends PlayerAction {
  final double ratio;
  const SetZoom(this.ratio) : super('SET_ZOOM');
}

/// Set layout mode explicitly (0=sideBySide, 1=splitScreen).
class SetLayoutMode extends PlayerAction {
  final int mode;
  const SetLayoutMode(this.mode) : super('SET_LAYOUT_MODE');
}

/// Set split position (0.0–1.0).
class SetSplitPos extends PlayerAction {
  final double position;
  const SetSplitPos(this.position) : super('SET_SPLIT_POS');
}

/// Remove a track by file_id.
class RemoveTrackAction extends PlayerAction {
  final int fileId;
  const RemoveTrackAction(this.fileId) : super('REMOVE_TRACK');
}

/// Swap two media-header display positions.
class SwapMediaHeader extends PlayerAction {
  final int slotIndex;
  final int targetTrackIndex;
  const SwapMediaHeader(this.slotIndex, this.targetTrackIndex)
    : super('SWAP_MEDIA_HEADER');
}

/// Adjust a track sync offset by delta milliseconds.
class AdjustTrackOffset extends PlayerAction {
  final int slot;
  final int deltaMs;
  const AdjustTrackOffset(this.slot, this.deltaMs)
    : super('ADJUST_TRACK_OFFSET');
}

class SetLoopEnabled extends PlayerAction {
  final bool enabled;
  const SetLoopEnabled(this.enabled) : super('SET_LOOP_ENABLED');
}

class SetLoopRange extends PlayerAction {
  final int startUs;
  final int endUs;
  const SetLoopRange(this.startUs, this.endUs) : super('SET_LOOP_RANGE');
}

class DragLoopHandle extends PlayerAction {
  final String handle;
  final int targetUs;
  final int steps;
  const DragLoopHandle(this.handle, this.targetUs, {this.steps = 12})
    : super('DRAG_LOOP_HANDLE');
}

class DragSplitHandle extends PlayerAction {
  final double targetFraction;
  final int steps;
  const DragSplitHandle(this.targetFraction, {this.steps = 12})
    : super('DRAG_SPLIT_HANDLE');
}

/// Pan the viewport by a delta.
class Pan extends PlayerAction {
  final double dx;
  final double dy;
  const Pan(this.dx, this.dy) : super('PAN');
}
