import 'dart:io';

class NativeCompositorFlags {
  const NativeCompositorFlags._();

  static bool get nativeCompositor {
    if (Platform.isWindows) {
      final mode = Platform.environment['VOIDPLAYER_WINDOWS_PRESENTATION_MODE']
          ?.toLowerCase();
      return mode == null ||
          mode.isEmpty ||
          mode == 'auto' ||
          mode == 'native-compositor-sdr' ||
          mode == 'native-compositor-scrgb';
    }
    if (!Platform.isMacOS) {
      return false;
    }
    final mode = Platform.environment['VOIDPLAYER_MACOS_PRESENTATION_MODE']
        ?.toLowerCase();
    if (mode == 'flutter-texture-sdr' || mode == 'flutter') {
      return false;
    }
    return mode == null ||
        mode == 'auto' ||
        mode == 'native-compositor-sdr' ||
        mode == 'native-compositor-edr' ||
        mode == 'native' ||
        mode == 'compositor' ||
        mode == 'edr' ||
        mode == 'hdr' ||
        Platform.environment['VOIDPLAYER_NATIVE_COMPOSITOR'] == '1' ||
        Platform.environment['VOIDPLAYER_NATIVE_COMPOSITOR_SPIKE'] == '1';
  }

  static bool get sourceProjection =>
      (Platform.isMacOS || Platform.isWindows) && nativeCompositor;
}
