import 'package:flutter/material.dart';

import '../../actions/action_registry.dart';
import '../../preferences/playback_preferences.dart';
import '../../startup_options.dart';
import 'main_window_controller.dart';
import 'main_window_view.dart';

class MainWindow extends StatefulWidget {
  final ActionRegistry actionRegistry;
  final String? testScriptPath;
  final StartupOptions startupOptions;
  final PlaybackPreferences? playbackPreferences;

  const MainWindow({
    super.key,
    required this.actionRegistry,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
    this.playbackPreferences,
  });

  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> with TickerProviderStateMixin {
  late final MainWindowController _controller;
  int? _lastViewportBackgroundColor;

  @override
  void initState() {
    super.initState();
    _controller = MainWindowController(
      actionRegistry: widget.actionRegistry,
      vsync: this,
      startupOptions: widget.startupOptions,
      playbackPreferences: widget.playbackPreferences,
      mounted: () => mounted,
    )..start(testScriptPath: widget.testScriptPath);
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    _syncViewportBackgroundColor(context);
    return ListenableBuilder(
      listenable: _controller.listenable,
      builder: (context, _) => MainWindowView(
        model: _controller.viewModel,
        actions: _controller.viewActions,
      ),
    );
  }

  void _syncViewportBackgroundColor(BuildContext context) {
    final theme = Theme.of(context);
    final color = theme.brightness == Brightness.light
        ? theme.colorScheme.surfaceContainerHighest
        : theme.colorScheme.surfaceContainerLowest;
    final value = color.toARGB32();
    if (_lastViewportBackgroundColor == value) return;
    _lastViewportBackgroundColor = value;
    _controller.setViewportBackgroundColor(color);
  }
}
