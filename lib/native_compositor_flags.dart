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
    return mode == null ||
        mode == 'auto' ||
        mode == 'renderer-owned-wgpu-sdr' ||
        mode == 'renderer-owned-wgpu-edr' ||
        mode == 'wgpu-metal' ||
        mode == 'wgpu' ||
        mode == 'native' ||
        mode == 'compositor' ||
        mode == 'edr' ||
        mode == 'hdr';
  }

  static bool get sourceProjection =>
      (Platform.isMacOS || Platform.isWindows) && nativeCompositor;
}
