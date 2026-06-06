import 'package:flutter/gestures.dart';

enum ViewportDragIntent { none, pan }

/// Centralizes pointer-to-viewport intent mapping so mouse, touchpad, and
/// future armed annotation gestures do not grow separate ad hoc branches.
class ViewportInteractionPolicy {
  const ViewportInteractionPolicy();

  ViewportDragIntent dragIntentForButtons(int buttons) {
    if ((buttons & kPrimaryButton) != 0) {
      return ViewportDragIntent.pan;
    }
    return ViewportDragIntent.none;
  }

  bool isSecondaryButtonDown(int buttons) => (buttons & kSecondaryButton) != 0;
}

const defaultViewportInteractionPolicy = ViewportInteractionPolicy();
