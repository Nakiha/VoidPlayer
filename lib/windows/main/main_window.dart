import 'package:flutter/material.dart';

import '../../startup_options.dart';
import 'main_window_controller.dart';
import 'main_window_view.dart';

class MainWindow extends StatefulWidget {
  final String? testScriptPath;
  final StartupOptions startupOptions;

  const MainWindow({
    super.key,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
  });

  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> with TickerProviderStateMixin {
  late final MainWindowController _controller;

  @override
  void initState() {
    super.initState();
    _controller = MainWindowController(
      vsync: this,
      startupOptions: widget.startupOptions,
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
    return ListenableBuilder(
      listenable: _controller.listenable,
      builder: (context, _) => MainWindowView(
        model: _controller.viewModel,
        actions: _controller.viewActions,
      ),
    );
  }
}
