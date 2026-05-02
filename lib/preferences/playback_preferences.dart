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
