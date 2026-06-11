import 'dart:io';

class HDRSpikeFlags {
  const HDRSpikeFlags._();

  static bool get nativeCompositor =>
      Platform.environment['VOIDPLAYER_NATIVE_COMPOSITOR_SPIKE'] == '1';
}
