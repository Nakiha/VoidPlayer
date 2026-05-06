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

abstract class PlaybackPreferences {
  SeekAfterJumpBehavior get seekAfterJumpBehavior;
  DecodeMode get decodeMode;

  bool get useHardwareDecode => decodeMode.useHardwareDecode;
}
