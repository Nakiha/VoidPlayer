import 'package:flutter/services.dart';

/// User actions that can be triggered by shortcuts, buttons, or test scripts.
sealed class PlayerAction {
  final String name;
  final LogicalKeyboardKey? shortcut;
  const PlayerAction(this.name, [this.shortcut]);
}

class TogglePlayPause extends PlayerAction {
  const TogglePlayPause() : super('TOGGLE_PLAY_PAUSE', LogicalKeyboardKey.space);
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
  const ToggleLayoutMode() : super('TOGGLE_LAYOUT_MODE', LogicalKeyboardKey.keyM);
}
