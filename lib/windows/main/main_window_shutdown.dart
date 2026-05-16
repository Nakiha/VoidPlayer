typedef MainWindowShutdownCallback = Future<void> Function();

class MainWindowShutdownRegistry {
  MainWindowShutdownRegistry._();

  static Object? _owner;
  static MainWindowShutdownCallback? _callback;

  static void register(Object owner, MainWindowShutdownCallback callback) {
    _owner = owner;
    _callback = callback;
  }

  static void unregister(Object owner) {
    if (!identical(_owner, owner)) return;
    _owner = null;
    _callback = null;
  }

  static Future<void> closeGracefully() async {
    final callback = _callback;
    if (callback == null) return;
    await callback();
  }
}
