import 'dart:io';

class NativeCompositorFlags {
  const NativeCompositorFlags._();

  static bool get nativeCompositor => resolve(
    isWindows: Platform.isWindows,
    isMacOS: Platform.isMacOS,
    environment: Platform.environment,
  );

  static bool resolve({
    required bool isWindows,
    required bool isMacOS,
    required Map<String, String> environment,
  }) {
    // Windows native playback always presents through the runner-owned D3D11
    // compositor. The runtime event still decides when the viewport becomes
    // transparent; this flag only enables that Flutter composition shape.
    if (isWindows) {
      return true;
    }
    if (!isMacOS) {
      return false;
    }
    final mode = environment['VOIDPLAYER_MACOS_PRESENTATION_MODE']
        ?.toLowerCase();
    return mode == null ||
        mode == 'auto' ||
        mode == 'native-compositor-sdr' ||
        mode == 'native-compositor-edr' ||
        mode == 'native' ||
        mode == 'compositor' ||
        mode == 'edr' ||
        mode == 'hdr' ||
        environment['VOIDPLAYER_NATIVE_COMPOSITOR'] == '1';
  }
}
