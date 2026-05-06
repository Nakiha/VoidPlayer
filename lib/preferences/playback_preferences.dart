enum SeekAfterJumpBehavior {
  forcePause('forcePause'),
  keepPreviousState('keepPreviousState');

  const SeekAfterJumpBehavior(this.storageValue);

  final String storageValue;

  static SeekAfterJumpBehavior fromStorage(String value) {
    return SeekAfterJumpBehavior.values.firstWhere(
      (behavior) => behavior.storageValue == value,
      orElse: () => SeekAfterJumpBehavior.keepPreviousState,
    );
  }
}

enum DecodeMode {
  preferHardware('preferHardware'),
  forceSoftware('forceSoftware');

  const DecodeMode(this.storageValue);

  final String storageValue;

  bool get useHardwareDecode => this == DecodeMode.preferHardware;

  static DecodeMode fromStorage(String value) {
    return DecodeMode.values.firstWhere(
      (mode) => mode.storageValue == value,
      orElse: () => DecodeMode.preferHardware,
    );
  }
}

enum ViewportPixelSizeMode {
  uniformVideoPixels('uniformVideoPixels'),
  fillView('fillView');

  const ViewportPixelSizeMode(this.storageValue);

  final String storageValue;

  int get layoutValue => switch (this) {
    ViewportPixelSizeMode.uniformVideoPixels => 0,
    ViewportPixelSizeMode.fillView => 1,
  };

  static ViewportPixelSizeMode fromStorage(String value) {
    return ViewportPixelSizeMode.values.firstWhere(
      (mode) => mode.storageValue == value,
      orElse: () => ViewportPixelSizeMode.uniformVideoPixels,
    );
  }

  static ViewportPixelSizeMode fromLayoutValue(int value) {
    return value == ViewportPixelSizeMode.fillView.layoutValue
        ? ViewportPixelSizeMode.fillView
        : ViewportPixelSizeMode.uniformVideoPixels;
  }
}

abstract class PlaybackPreferences {
  SeekAfterJumpBehavior get seekAfterJumpBehavior;
  DecodeMode get decodeMode;
  ViewportPixelSizeMode get viewportPixelSizeMode;

  bool get useHardwareDecode => decodeMode.useHardwareDecode;
}
