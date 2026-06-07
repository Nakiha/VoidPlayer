import 'package:flutter/material.dart';

import '../../actions/action_registry.dart';
import '../../config/app_settings_repository.dart';
import '../../marks/quick_mark_persistence.dart';
import '../../platform/analysis_process_host.dart';
import '../../platform/main_window_platform.dart';
import '../../platform/native_file_picker.dart';
import '../../platform/platform_capabilities.dart';
import '../../platform/pointer_button_state_provider.dart';
import '../../preferences/playback_preferences.dart';
import '../../startup_options.dart';
import 'main_window_controller.dart';
import 'main_window_shutdown.dart';
import 'main_window_view.dart';

class MainWindow extends StatefulWidget {
  final ActionRegistry actionRegistry;
  final String? testScriptPath;
  final StartupOptions startupOptions;
  final AnalysisProcessHost? analysisProcesses;
  final PlatformCapabilities platformCapabilities;
  final MainWindowPlatform? platformWindow;
  final NativeFilePicker? nativeFilePicker;
  final PointerButtonStateProvider pointerButtonStateProvider;
  final AppSettingsRepository? appSettings;
  final PlaybackPreferences? playbackPreferences;
  final QuickMarkRepository? quickMarkRepository;
  final Color? accentColor;

  const MainWindow({
    super.key,
    required this.actionRegistry,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
    this.analysisProcesses,
    this.platformCapabilities = PlatformCapabilities.windows,
    this.platformWindow,
    this.nativeFilePicker,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
    this.appSettings,
    this.playbackPreferences,
    this.quickMarkRepository,
    this.accentColor,
  });

  @override
  State<MainWindow> createState() => _MainWindowState();
}

class _MainWindowState extends State<MainWindow> with TickerProviderStateMixin {
  late final MainWindowController _controller;
  int? _lastViewportBackgroundColor;
  int? _lastAnalysisAccentColor;

  @override
  void initState() {
    super.initState();
    _controller = MainWindowController(
      actionRegistry: widget.actionRegistry,
      vsync: this,
      startupOptions: widget.startupOptions,
      analysisProcesses: widget.analysisProcesses,
      platformCapabilities: widget.platformCapabilities,
      platformWindow: widget.platformWindow,
      nativeFilePicker: widget.nativeFilePicker,
      appSettings: widget.appSettings,
      playbackPreferences: widget.playbackPreferences,
      quickMarkRepository:
          widget.quickMarkRepository ??
          FileQuickMarkRepository.defaultLocation(),
      mounted: () => mounted,
    )..start(testScriptPath: widget.testScriptPath);
    MainWindowShutdownRegistry.register(this, _controller.closeGracefully);
  }

  @override
  void dispose() {
    MainWindowShutdownRegistry.unregister(this);
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    _syncViewportBackgroundColor(context);
    _syncAnalysisAccentColor();
    return ListenableBuilder(
      listenable: _controller.listenable,
      builder: (context, _) => MainWindowView(
        model: _controller.viewModel,
        actions: _controller.viewActions,
        pointerButtonStateProvider: widget.pointerButtonStateProvider,
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

  void _syncAnalysisAccentColor() {
    final accentColor = widget.accentColor;
    if (accentColor == null) return;
    final value = accentColor.toARGB32();
    if (_lastAnalysisAccentColor == value) return;
    _lastAnalysisAccentColor = value;
    _controller.setAnalysisAccentColor(accentColor);
  }
}
