import 'package:flutter/services.dart';

/// A shortcut entry for display in the settings UI.
typedef ShortcutEntry = ({String labelKey, String shortcutLabel});

/// User actions that can be triggered by shortcuts, buttons, or test scripts.
sealed class PlayerAction {
  final String name;
  final LogicalKeyboardKey? shortcut;
  final bool requireControl;
  const PlayerAction(this.name, [this.shortcut, this.requireControl = false]);

  /// All keyboard shortcuts for display in the settings UI.
  ///
  /// When adding a new action with a shortcut, append an entry here so the
  /// settings window picks it up automatically.  [labelKey] must match a key
  /// defined in `app_*.arb`; [shortcutLabel] is the human-readable key label.
  static const List<ShortcutEntry> shortcutEntries = [
    (labelKey: 'actionTogglePlay', shortcutLabel: 'Space'),
    (labelKey: 'actionStepForward', shortcutLabel: '→'),
    (labelKey: 'actionStepBackward', shortcutLabel: '←'),
    (labelKey: 'actionOpenFile', shortcutLabel: 'O'),
    (labelKey: 'actionToggleLayout', shortcutLabel: 'M'),
    (labelKey: 'actionSeekForward', shortcutLabel: 'Shift + →'),
    (labelKey: 'actionSeekBackward', shortcutLabel: 'Shift + ←'),
  ];
}

class TogglePlayPause extends PlayerAction {
  const TogglePlayPause()
    : super('TOGGLE_PLAY_PAUSE', LogicalKeyboardKey.space);
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
  const StepForward() : super('STEP_FORWARD', LogicalKeyboardKey.arrowRight);
}

class StepBackward extends PlayerAction {
  const StepBackward() : super('STEP_BACKWARD', LogicalKeyboardKey.arrowLeft);
}

class OpenFile extends PlayerAction {
  const OpenFile() : super('OPEN_FILE', LogicalKeyboardKey.keyO);
}

class ToggleLayoutMode extends PlayerAction {
  const ToggleLayoutMode()
    : super('TOGGLE_LAYOUT_MODE', LogicalKeyboardKey.keyM);
}

class NewWindow extends PlayerAction {
  const NewWindow() : super('NEW_WINDOW', LogicalKeyboardKey.keyN);
}

class OpenSettings extends PlayerAction {
  const OpenSettings() : super('OPEN_SETTINGS');
}

class OpenStats extends PlayerAction {
  const OpenStats() : super('OPEN_STATS');
}

class OpenMemory extends PlayerAction {
  const OpenMemory() : super('OPEN_MEMORY');
}

class OpenAnalysis extends PlayerAction {
  const OpenAnalysis()
    : super('OPEN_ANALYSIS', LogicalKeyboardKey.backquote, true);
}

class RunAnalysis extends PlayerAction {
  const RunAnalysis() : super('RUN_ANALYSIS');
}

/// Add a media file by path (no file-picker dialog).
class AddMedia extends PlayerAction {
  final String path;
  const AddMedia(this.path) : super('ADD_MEDIA');
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

/// Pan the viewport by a delta.
class Pan extends PlayerAction {
  final double dx;
  final double dy;
  const Pan(this.dx, this.dy) : super('PAN');
}

class SetRenderSize extends PlayerAction {
  final int width;
  final int height;
  const SetRenderSize(this.width, this.height) : super('SET_RENDER_SIZE');
}

class CaptureViewportAction extends PlayerAction {
  final String nameId;
  final String? outputPath;
  const CaptureViewportAction(this.nameId, {this.outputPath})
    : super('CAPTURE_VIEWPORT');
}

class WindowMaximize extends PlayerAction {
  const WindowMaximize() : super('WINDOW_MAXIMIZE');
}

class WindowRestore extends PlayerAction {
  const WindowRestore() : super('WINDOW_RESTORE');
}

class StoreViewCenter extends PlayerAction {
  final String nameId;
  const StoreViewCenter(this.nameId) : super('STORE_VIEW_CENTER');
}
