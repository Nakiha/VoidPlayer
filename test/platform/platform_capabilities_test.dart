import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/platform/platform_capabilities.dart';

void main() {
  test('capability user message prefers detail over status label', () {
    const capability = PlatformCapability(
      CapabilityState.sandboxLimited,
      detail: 'Local files require user-selected sandbox access.',
    );

    expect(capability.statusLabel, 'Sandbox limited');
    expect(
      capability.userMessage,
      'Local files require user-selected sandbox access.',
    );
  });

  test('capability user message falls back to status label', () {
    const capability = PlatformCapability(CapabilityState.experimental);

    expect(capability.userMessage, 'Experimental');
  });

  test('firstCapabilityUserMessage prioritizes unavailable capability', () {
    final message = firstCapabilityUserMessage([
      const PlatformCapability(
        CapabilityState.sandboxLimited,
        detail: 'Local files require sandbox access.',
      ),
      const PlatformCapability(
        CapabilityState.unsupported,
        detail: 'Network media is not enabled.',
      ),
    ]);

    expect(message, 'Network media is not enabled.');
  });

  test(
    'firstCapabilityUserMessage reports limited capability when all work',
    () {
      final message = firstCapabilityUserMessage([
        const PlatformCapability(CapabilityState.supported),
        const PlatformCapability(
          CapabilityState.localOnly,
          detail: 'Overlay rendering is available for local playback.',
        ),
      ]);

      expect(message, 'Overlay rendering is available for local playback.');
    },
  );
}
