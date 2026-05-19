import 'package:flutter/material.dart';

abstract interface class SystemAccentWatcher {
  void start();
  void dispose();
}

class NoopSystemAccentWatcher implements SystemAccentWatcher {
  const NoopSystemAccentWatcher({ValueChanged<Color>? onChanged});

  @override
  void start() {}

  @override
  void dispose() {}
}
