import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/native_compositor_flags.dart';

void main() {
  group('NativeCompositorFlags', () {
    test('enables the Windows runner-owned compositor', () {
      expect(
        NativeCompositorFlags.resolve(
          isWindows: true,
          isMacOS: false,
          environment: const {},
        ),
        isTrue,
      );
    });

    test('keeps the macOS native compositor enabled by default', () {
      expect(
        NativeCompositorFlags.resolve(
          isWindows: false,
          isMacOS: true,
          environment: const {},
        ),
        isTrue,
      );
    });

    test('respects an explicit non-native macOS presentation mode', () {
      expect(
        NativeCompositorFlags.resolve(
          isWindows: false,
          isMacOS: true,
          environment: const {
            'VOIDPLAYER_MACOS_PRESENTATION_MODE': 'flutter-texture',
          },
        ),
        isFalse,
      );
    });

    test('does not enable the compositor on unsupported platforms', () {
      expect(
        NativeCompositorFlags.resolve(
          isWindows: false,
          isMacOS: false,
          environment: const {'VOIDPLAYER_NATIVE_COMPOSITOR': '1'},
        ),
        isFalse,
      );
    });
  });
}
