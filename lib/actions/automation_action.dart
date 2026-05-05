/// Release-supported UI automation commands that are not user/player actions.
sealed class AutomationAction {
  final String name;

  const AutomationAction(this.name);
}

class SetRenderSize extends AutomationAction {
  final int width;
  final int height;

  const SetRenderSize(this.width, this.height) : super('SET_RENDER_SIZE');
}

class CaptureViewportAction extends AutomationAction {
  final String nameId;
  final String? outputPath;

  const CaptureViewportAction(this.nameId, {this.outputPath})
    : super('CAPTURE_VIEWPORT');
}

class WindowMaximize extends AutomationAction {
  const WindowMaximize() : super('WINDOW_MAXIMIZE');
}

class WindowRestore extends AutomationAction {
  const WindowRestore() : super('WINDOW_RESTORE');
}

class StoreViewCenter extends AutomationAction {
  final String nameId;

  const StoreViewCenter(this.nameId) : super('STORE_VIEW_CENTER');
}

class StoreResourceUsage extends AutomationAction {
  final String nameId;

  const StoreResourceUsage(this.nameId) : super('STORE_RESOURCE_USAGE');
}

class StoreNativeSeekCount extends AutomationAction {
  final String nameId;

  const StoreNativeSeekCount(this.nameId) : super('STORE_NATIVE_SEEK_COUNT');
}
