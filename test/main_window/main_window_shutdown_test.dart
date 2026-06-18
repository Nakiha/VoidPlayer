import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/platform/main_window_shutdown.dart';

void main() {
  test('runs the registered shutdown callback', () async {
    var calls = 0;
    final owner = Object();
    MainWindowShutdownRegistry.register(owner, () async {
      calls++;
    });
    addTearDown(() => MainWindowShutdownRegistry.unregister(owner));

    await MainWindowShutdownRegistry.closeGracefully();

    expect(calls, 1);
  });

  test('keeps callback when a different owner unregisters', () async {
    var calls = 0;
    final owner = Object();
    MainWindowShutdownRegistry.register(owner, () async {
      calls++;
    });
    addTearDown(() => MainWindowShutdownRegistry.unregister(owner));

    MainWindowShutdownRegistry.unregister(Object());
    await MainWindowShutdownRegistry.closeGracefully();

    expect(calls, 1);
  });

  test('clears callback when owner unregisters', () async {
    var calls = 0;
    final owner = Object();
    MainWindowShutdownRegistry.register(owner, () async {
      calls++;
    });

    MainWindowShutdownRegistry.unregister(owner);
    await MainWindowShutdownRegistry.closeGracefully();

    expect(calls, 0);
  });
}
