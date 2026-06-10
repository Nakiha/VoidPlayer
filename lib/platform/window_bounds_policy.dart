import 'dart:ui';

const Size kDefaultMainWindowSize = Size(1280, 720);
const Size kMinimumMainWindowSize = Size(520, 360);

bool isRestorableWindowRect(
  Rect? rect, {
  Size minimumSize = kMinimumMainWindowSize,
  bool Function(Rect rect)? isOnScreen,
}) {
  if (rect == null) return false;
  if (!rect.left.isFinite ||
      !rect.top.isFinite ||
      !rect.width.isFinite ||
      !rect.height.isFinite) {
    return false;
  }
  if (rect.width < minimumSize.width || rect.height < minimumSize.height) {
    return false;
  }
  return isOnScreen?.call(rect) ?? true;
}
