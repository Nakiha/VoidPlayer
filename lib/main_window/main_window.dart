import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../actions/action_registry.dart';
import '../config/app_settings_repository.dart';
import '../feedback/app_feedback.dart';
import '../l10n/app_localizations.dart';
import '../marks/quick_mark_persistence.dart';
import '../native_player/native_player_protocol.dart';
import '../platform/main_window_platform.dart';
import '../platform/main_window_shutdown.dart';
import '../platform/native_file_picker.dart';
import '../platform/platform_capabilities.dart';
import '../platform/pointer_button_state_provider.dart';
import '../preferences/playback_preferences.dart';
import '../startup_options.dart';
import 'main_window_controller.dart';
import 'main_window_view.dart';

class MainWindow extends StatefulWidget {
  final ActionRegistry actionRegistry;
  final String? testScriptPath;
  final StartupOptions startupOptions;
  final PlatformCapabilities platformCapabilities;
  final MainWindowPlatform? platformWindow;
  final NativeFilePicker? nativeFilePicker;
  final PointerButtonStateProvider pointerButtonStateProvider;
  final AppSettingsRepository? appSettings;
  final PlaybackPreferences? playbackPreferences;
  final QuickMarkRepository? quickMarkRepository;

  const MainWindow({
    super.key,
    required this.actionRegistry,
    this.testScriptPath,
    this.startupOptions = const StartupOptions(),
    this.platformCapabilities = PlatformCapabilities.windows,
    this.platformWindow,
    this.nativeFilePicker,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
    this.appSettings,
    this.playbackPreferences,
    this.quickMarkRepository,
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
      platformCapabilities: widget.platformCapabilities,
      platformWindow: widget.platformWindow,
      nativeFilePicker: widget.nativeFilePicker,
      appSettings: widget.appSettings,
      playbackPreferences: widget.playbackPreferences,
      quickMarkRepository:
          widget.quickMarkRepository ??
          SqliteQuickMarkRepository.defaultLocation(),
      mounted: () => mounted,
      onDuplicateMediaSkipped: _showDuplicateMediaSkipped,
      onUserActionFailed: _showUserActionFailed,
    )..start(testScriptPath: widget.testScriptPath);
    MainWindowShutdownRegistry.register(this, _controller.closeGracefully);
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _syncViewportBackgroundColor(context);
  }

  @override
  void dispose() {
    MainWindowShutdownRegistry.unregister(this);
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _controller.listenable,
      builder: (context, _) => MainWindowView(
        model: _controller.viewModel,
        handles: _controller.viewHandles,
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

  void _showDuplicateMediaSkipped(int count) {
    if (!mounted) return;
    final l = AppLocalizations.of(context);
    final message = l == null
        ? 'Media already added. Skipped $count duplicate item(s).'
        : l.duplicateMediaSkipped(count);
    AppFeedbackScope.read(context).show(
      AppFeedbackMessage(text: message, severity: AppFeedbackSeverity.warning),
    );
  }

  void _showUserActionFailed(String operation, Object error) {
    if (!mounted) return;
    AppFeedbackScope.read(
      context,
    ).showError(formatMainWindowUserActionFailure(operation, error));
  }
}

String formatMainWindowUserActionFailure(String operation, Object error) {
  return '$operation failed: ${_userActionFailureDetail(error)}';
}

String _userActionFailureDetail(Object error) {
  if (error is PlatformException) {
    final message = error.message?.trim();
    if (message != null && message.isNotEmpty) return message;
    final code = error.code.trim();
    if (code.isNotEmpty) return code;
  }
  if (error is NativeProtocolException) {
    return error.reason;
  }
  return error.toString();
}
