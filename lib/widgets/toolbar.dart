import 'dart:async';

import 'package:flutter/material.dart';

import '../analysis/analysis_cache.dart';
import '../analysis/analysis_manager.dart';
import '../analysis/analysis_toolbar_data_source.dart';
import '../feedback/app_feedback.dart';
import '../l10n/app_localizations.dart';
import '../track_manager.dart';
import '../utils/async_guard.dart';
import 'app_menu_combo.dart';
import 'open_network_stream_dialog.dart';
import 'open_ssh_remote_file_dialog.dart';
import 'segmented_widget.dart';

const _analysisPanelWidth = 360.0;
const _analysisPanelMaxHeight = 240.0;
const _analysisPanelHeaderHeight = 34.0;
const _analysisPanelRowHeight = 44.0;

/// Top toolbar matching PySide6 ToolBar (40px height, margins: 4).
class AppToolBar extends StatelessWidget {
  final int viewMode; // 0=sideBySide, 1=splitScreen
  final ValueChanged<int> onViewModeChanged;
  final Future<void> Function() onOpenFile;
  final Future<void> Function(String url) onOpenNetworkMedia;
  final Future<void> Function(String remotePath) onOpenSshRemoteMedia;
  final VoidCallback onMediaInfo;
  final Future<void> Function() onAnalysis;
  final VoidCallback onProfiler;
  final VoidCallback onSettings;
  final VoidCallback onMarksSidebarToggle;
  final List<TrackEntry> tracks;
  final AnalysisToolbarDataSource analysisDataSource;
  final bool viewModeEnabled;
  final bool nativePlaybackAvailable;
  final bool localFilePlaybackAvailable;
  final bool networkMediaAvailable;
  final bool sshRemoteMediaAvailable;
  final bool nativeFilePickerAvailable;
  final String? addMediaDisabledTooltip;
  final String? analysisDisabledTooltip;
  final bool canAddTrack;
  final bool canOpenLocalMedia;
  final bool canOpenNetworkMedia;
  final bool canOpenSshMedia;
  final bool canOpenMediaInfo;
  final bool canOpenProfiler;
  final bool canRunAnalysis;
  final bool analysisEnabled;
  final bool analysisWorkspaceActive;
  final bool mediaInfoActive;
  final bool profilerActive;
  final bool marksSidebarActive;

  const AppToolBar({
    super.key,
    required this.viewMode,
    required this.onViewModeChanged,
    required this.onOpenFile,
    required this.onOpenNetworkMedia,
    required this.onOpenSshRemoteMedia,
    required this.onMediaInfo,
    required this.onAnalysis,
    required this.onProfiler,
    required this.onSettings,
    required this.onMarksSidebarToggle,
    required this.tracks,
    required this.analysisDataSource,
    this.viewModeEnabled = false,
    this.nativePlaybackAvailable = true,
    this.localFilePlaybackAvailable = true,
    this.networkMediaAvailable = true,
    this.sshRemoteMediaAvailable = true,
    this.nativeFilePickerAvailable = true,
    this.addMediaDisabledTooltip,
    this.analysisDisabledTooltip,
    this.canAddTrack = true,
    this.canOpenLocalMedia = true,
    this.canOpenNetworkMedia = true,
    this.canOpenSshMedia = true,
    this.canOpenMediaInfo = true,
    this.canOpenProfiler = true,
    this.canRunAnalysis = true,
    this.analysisEnabled = false,
    this.analysisWorkspaceActive = false,
    this.mediaInfoActive = false,
    this.profilerActive = false,
    this.marksSidebarActive = false,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 40,
      padding: const EdgeInsets.all(4),
      child: Row(
        children: [
          _ToolbarToggleButton(
            active: marksSidebarActive,
            onPressed: onMarksSidebarToggle,
            customIcon: const _LeftSidebarToggleIcon(),
            tooltip: AppLocalizations.of(context)!.mainWindowLeftPanelToggle,
          ),
          const SizedBox(width: 4),
          // View mode selector (240x32)
          Opacity(
            opacity: viewModeEnabled ? 1.0 : 0.5,
            child: IgnorePointer(
              ignoring: !viewModeEnabled,
              child: ViewModeSelector(
                currentMode: viewMode,
                onChanged: onViewModeChanged,
              ),
            ),
          ),
          const Spacer(),
          _AddMediaButton(
            localFileEnabled:
                canAddTrack &&
                canOpenLocalMedia &&
                localFilePlaybackAvailable &&
                nativeFilePickerAvailable,
            networkMediaEnabled:
                canAddTrack && canOpenNetworkMedia && networkMediaAvailable,
            sshRemoteMediaEnabled:
                canAddTrack && canOpenSshMedia && sshRemoteMediaAvailable,
            disabledTooltip:
                addMediaDisabledTooltip ??
                'Playback is not available on this platform yet.',
            onOpenFile: onOpenFile,
            onOpenNetworkMedia: onOpenNetworkMedia,
            onOpenSshRemoteMedia: onOpenSshRemoteMedia,
          ),
          const SizedBox(width: 4),
          _ToolbarToggleButton(
            active: mediaInfoActive,
            enabled: canOpenMediaInfo && tracks.isNotEmpty,
            onPressed: onMediaInfo,
            icon: Icons.info_outline,
            tooltip: AppLocalizations.of(context)!.mediaInfo,
          ),
          const SizedBox(width: 4),
          // Profiler button
          _ToolbarToggleButton(
            active: profilerActive,
            enabled: canOpenProfiler && tracks.isNotEmpty,
            onPressed: onProfiler,
            icon: Icons.speed,
            tooltip: AppLocalizations.of(context)!.performanceMonitor,
          ),
          const SizedBox(width: 4),
          // Analysis button
          _AnalysisButton(
            enabled: canRunAnalysis && analysisEnabled,
            active: analysisWorkspaceActive,
            tracks: tracks,
            dataSource: analysisDataSource,
            disabledTooltip: analysisDisabledTooltip,
            onPressed: onAnalysis,
          ),
          const SizedBox(width: 4),
          // Settings button
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onSettings,
              icon: const Icon(Icons.settings, size: 18),
              tooltip: AppLocalizations.of(context)!.settings,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
        ],
      ),
    );
  }
}

class _AddMediaButton extends StatelessWidget {
  final bool localFileEnabled;
  final bool networkMediaEnabled;
  final bool sshRemoteMediaEnabled;
  final String disabledTooltip;
  final Future<void> Function() onOpenFile;
  final Future<void> Function(String url) onOpenNetworkMedia;
  final Future<void> Function(String remotePath) onOpenSshRemoteMedia;

  const _AddMediaButton({
    required this.localFileEnabled,
    required this.networkMediaEnabled,
    required this.sshRemoteMediaEnabled,
    required this.disabledTooltip,
    required this.onOpenFile,
    required this.onOpenNetworkMedia,
    required this.onOpenSshRemoteMedia,
  });

  static const _allChoices = [
    _AddMediaChoice.localFile,
    _AddMediaChoice.networkStream,
    _AddMediaChoice.sshRemoteFile,
  ];

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    final choices = _allChoices.where(_isEnabled).toList(growable: false);
    final anyEnabled = choices.isNotEmpty;
    final foreground = anyEnabled
        ? colorScheme.onPrimary
        : colorScheme.onSurface.withValues(alpha: 0.38);
    final background = anyEnabled
        ? colorScheme.primary
        : colorScheme.surfaceContainerHighest;
    return SizedBox(
      height: 32,
      child: Tooltip(
        message: anyEnabled ? l.openLocalFile : disabledTooltip,
        child: Material(
          color: background,
          borderRadius: BorderRadius.circular(18),
          clipBehavior: Clip.antiAlias,
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              InkWell(
                onTap: anyEnabled
                    ? () => fireAndLog(
                        'open first enabled media source',
                        _openFirstEnabled(context),
                      )
                    : null,
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(14, 0, 10, 0),
                  child: SizedBox(
                    height: 32,
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.add, size: 18, color: foreground),
                        const SizedBox(width: 8),
                        Text(
                          l.addMedia,
                          style: theme.textTheme.labelLarge?.copyWith(
                            color: foreground,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
              SizedBox(
                height: 18,
                width: 1,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: foreground.withValues(alpha: 0.28),
                  ),
                ),
              ),
              Tooltip(
                message: anyEnabled ? l.addMediaOptions : disabledTooltip,
                child: choices.isEmpty
                    ? SizedBox(
                        width: 36,
                        height: 32,
                        child: Icon(
                          Icons.arrow_drop_down,
                          size: 18,
                          color: foreground,
                        ),
                      )
                    : AppMenuCombo<_AddMediaChoice>(
                        width: 36,
                        height: 32,
                        value: choices.first,
                        items: choices,
                        buttonLabel: '',
                        labelFor: (choice) => _labelFor(l, choice),
                        iconFor: _iconFor,
                        onChanged: (choice) {
                          switch (choice) {
                            case _AddMediaChoice.localFile:
                              fireAndLog(
                                'open local media',
                                _openLocalFile(context),
                              );
                            case _AddMediaChoice.networkStream:
                              fireAndLog(
                                'open network media dialog',
                                _openNetworkDialog(context),
                              );
                            case _AddMediaChoice.sshRemoteFile:
                              fireAndLog(
                                'open SSH media dialog',
                                _openSshDialog(context),
                              );
                          }
                        },
                        menuTextStyle: theme.textTheme.bodySmall,
                        foregroundColor: foreground,
                        backgroundColor: Colors.transparent,
                        borderRadius: const BorderRadius.horizontal(
                          right: Radius.circular(18),
                        ),
                        buttonPadding: const EdgeInsets.only(left: 6, right: 8),
                        itemPadding: const EdgeInsets.symmetric(horizontal: 12),
                        showSelectedCheck: false,
                        notifyOnReselect: true,
                        maxMenuWidth: 240,
                      ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  bool _isEnabled(_AddMediaChoice choice) {
    return switch (choice) {
      _AddMediaChoice.localFile => localFileEnabled,
      _AddMediaChoice.networkStream => networkMediaEnabled,
      _AddMediaChoice.sshRemoteFile => sshRemoteMediaEnabled,
    };
  }

  Future<void> _openFirstEnabled(BuildContext context) {
    if (localFileEnabled) return _openLocalFile(context);
    if (networkMediaEnabled) {
      return _openNetworkDialog(context);
    }
    if (sshRemoteMediaEnabled) {
      return _openSshDialog(context);
    }
    return Future<void>.value();
  }

  String _labelFor(AppLocalizations l, _AddMediaChoice choice) {
    return switch (choice) {
      _AddMediaChoice.localFile => l.openLocalFile,
      _AddMediaChoice.networkStream => l.openNetworkStream,
      _AddMediaChoice.sshRemoteFile => l.openSshRemoteFile,
    };
  }

  IconData _iconFor(_AddMediaChoice choice) {
    return switch (choice) {
      _AddMediaChoice.localFile => Icons.video_file_outlined,
      _AddMediaChoice.networkStream => Icons.public,
      _AddMediaChoice.sshRemoteFile => Icons.dns_outlined,
    };
  }

  Future<void> _openLocalFile(BuildContext context) async {
    final feedback = AppFeedbackScope.read(context);
    try {
      await onOpenFile();
    } on Object catch (e) {
      feedback.showError(e.toString());
    } finally {
      await _restoreGlobalShortcutFocus();
    }
  }

  Future<void> _openNetworkDialog(BuildContext context) async {
    try {
      final url = await OpenNetworkStreamDialog.show(context);
      if (url == null || url.isEmpty) return;
      await onOpenNetworkMedia(url);
    } finally {
      await _restoreGlobalShortcutFocus();
    }
  }

  Future<void> _openSshDialog(BuildContext context) async {
    final feedback = AppFeedbackScope.read(context);
    try {
      final remotePath = await OpenSshRemoteFileDialog.show(context);
      if (remotePath == null || remotePath.isEmpty) return;
      await onOpenSshRemoteMedia(remotePath);
    } on Object catch (e) {
      feedback.showError(e.toString());
    } finally {
      await _restoreGlobalShortcutFocus();
    }
  }

  Future<void> _restoreGlobalShortcutFocus() async {
    FocusManager.instance.primaryFocus?.unfocus(
      disposition: UnfocusDisposition.scope,
    );
    await WidgetsBinding.instance.endOfFrame;
    FocusManager.instance.primaryFocus?.unfocus(
      disposition: UnfocusDisposition.scope,
    );
  }
}

enum _AddMediaChoice { localFile, networkStream, sshRemoteFile }

class _ToolbarToggleButton extends StatelessWidget {
  final bool active;
  final bool enabled;
  final VoidCallback onPressed;
  final IconData? icon;
  final Widget? customIcon;
  final String tooltip;

  const _ToolbarToggleButton({
    required this.active,
    this.enabled = true,
    required this.onPressed,
    this.icon,
    this.customIcon,
    required this.tooltip,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final colorScheme = theme.colorScheme;
    return IconButton(
      onPressed: enabled ? onPressed : null,
      icon: customIcon ?? Icon(icon, size: 18),
      tooltip: tooltip,
      padding: EdgeInsets.zero,
      style: ButtonStyle(
        fixedSize: const WidgetStatePropertyAll(Size.square(32)),
        minimumSize: const WidgetStatePropertyAll(Size.square(32)),
        maximumSize: const WidgetStatePropertyAll(Size.square(32)),
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
        shape: const WidgetStatePropertyAll(CircleBorder()),
        overlayColor: const WidgetStatePropertyAll(Colors.transparent),
        backgroundColor: WidgetStateProperty.resolveWith((states) {
          if (states.contains(WidgetState.disabled)) {
            return Colors.transparent;
          }
          if (states.contains(WidgetState.pressed)) {
            return colorScheme.primary.withValues(alpha: 0.22);
          }
          if (active ||
              states.contains(WidgetState.hovered) ||
              states.contains(WidgetState.focused)) {
            return colorScheme.primary.withValues(alpha: 0.16);
          }
          return Colors.transparent;
        }),
        foregroundColor: WidgetStateProperty.resolveWith((states) {
          if (states.contains(WidgetState.disabled)) {
            return colorScheme.onSurface.withValues(alpha: 0.38);
          }
          if (active) return colorScheme.primary;
          return colorScheme.onSurfaceVariant;
        }),
      ),
    );
  }
}

class _LeftSidebarToggleIcon extends StatelessWidget {
  const _LeftSidebarToggleIcon();

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _LeftSidebarToggleIconPainter(
        color: IconTheme.of(context).color ?? Colors.black,
      ),
      size: const Size(18, 18),
    );
  }
}

class _LeftSidebarToggleIconPainter extends CustomPainter {
  final Color color;

  const _LeftSidebarToggleIconPainter({required this.color});

  @override
  void paint(Canvas canvas, Size size) {
    final stroke = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.6
      ..strokeJoin = StrokeJoin.round;
    final fill = Paint()
      ..color = color.withValues(alpha: 0.16)
      ..style = PaintingStyle.fill;

    final outer = RRect.fromRectAndRadius(
      Rect.fromLTWH(2.0, 3.0, size.width - 4.0, size.height - 6.0),
      const Radius.circular(2.5),
    );
    canvas.drawRRect(outer, stroke);

    final sideRect = Rect.fromLTWH(3.4, 4.8, 3.6, size.height - 9.6);
    canvas.drawRect(sideRect, fill);
    canvas.drawLine(
      const Offset(8.2, 4.2),
      Offset(8.2, size.height - 4.2),
      stroke,
    );

    final arrow = Path()
      ..moveTo(13.0, size.height / 2)
      ..lineTo(9.0, size.height / 2)
      ..moveTo(10.6, size.height / 2 - 1.8)
      ..lineTo(9.2, size.height / 2)
      ..lineTo(10.6, size.height / 2 + 1.8);
    canvas.drawPath(arrow, stroke..strokeCap = StrokeCap.round);
  }

  @override
  bool shouldRepaint(covariant _LeftSidebarToggleIconPainter oldDelegate) {
    return oldDelegate.color != color;
  }
}

class _AnalysisButton extends StatefulWidget {
  final bool enabled;
  final bool active;
  final List<TrackEntry> tracks;
  final AnalysisToolbarDataSource dataSource;
  final String? disabledTooltip;
  final Future<void> Function() onPressed;

  const _AnalysisButton({
    required this.enabled,
    required this.active,
    required this.tracks,
    required this.dataSource,
    this.disabledTooltip,
    required this.onPressed,
  });

  @override
  State<_AnalysisButton> createState() => _AnalysisButtonState();
}

class _AnalysisButtonState extends State<_AnalysisButton>
    with SingleTickerProviderStateMixin {
  final LayerLink _layerLink = LayerLink();
  OverlayEntry? _overlayEntry;
  Timer? _hideTimer;
  bool _hoveringButton = false;
  bool _hoveringPanel = false;
  String? _lastButtonSignature;
  late final AnimationController _panelAnimationController;
  late final Animation<double> _panelOpacity;

  @override
  void initState() {
    super.initState();
    widget.dataSource.addListener(_handleDataSourceChanged);
    _lastButtonSignature = _buttonSignature();
    _panelAnimationController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 130),
      reverseDuration: const Duration(milliseconds: 110),
    );
    _panelOpacity = CurvedAnimation(
      parent: _panelAnimationController,
      curve: Curves.easeOutCubic,
      reverseCurve: Curves.easeInCubic,
    );
  }

  @override
  void didUpdateWidget(covariant _AnalysisButton oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.dataSource != widget.dataSource) {
      oldWidget.dataSource.removeListener(_handleDataSourceChanged);
      widget.dataSource.addListener(_handleDataSourceChanged);
      _lastButtonSignature = _buttonSignature();
    }
    if (_overlayEntry != null && oldWidget.tracks != widget.tracks) {
      _markOverlayNeedsBuildAfterFrame();
    }
  }

  @override
  void dispose() {
    _hideTimer?.cancel();
    widget.dataSource.removeListener(_handleDataSourceChanged);
    _removePanel();
    _panelAnimationController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final tooltip = widget.active
        ? '${MaterialLocalizations.of(context).closeButtonTooltip} '
              '${l.deckAnalysisTab}'
        : widget.enabled
        ? l.analysisClickToAnalyze
        : widget.disabledTooltip ?? l.analysisClickToAnalyze;
    final colors = Theme.of(context).colorScheme;
    return MouseRegion(
      onEnter: (_) {
        _hoveringButton = true;
        _showPanel();
      },
      onExit: (_) {
        _hoveringButton = false;
        _scheduleHidePanel();
      },
      child: CompositedTransformTarget(
        link: _layerLink,
        child: Tooltip(
          message: tooltip,
          child: Semantics(
            selected: widget.active,
            child: SizedBox(
              width: 32,
              height: 32,
              child: IconButton(
                onPressed: !widget.enabled || (_isWorking && !widget.active)
                    ? null
                    : () => fireAndLog(
                        'toggle analysis workspace',
                        _handlePressed(),
                      ),
                style: IconButton.styleFrom(
                  foregroundColor: widget.active
                      ? colors.onPrimaryContainer
                      : null,
                  backgroundColor: widget.active
                      ? colors.primaryContainer
                      : Colors.transparent,
                ),
                icon: _isWorking && !widget.active
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : Icon(
                        _isError
                            ? Icons.error_outline
                            : Icons.analytics_outlined,
                        size: 18,
                        color: _isError ? colors.error : null,
                      ),
                padding: EdgeInsets.zero,
                constraints: const BoxConstraints.tightFor(
                  width: 32,
                  height: 32,
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }

  bool get _isWorking =>
      widget.dataSource.state == AnalysisState.computingHash ||
      widget.dataSource.state == AnalysisState.generating;

  bool get _isError => widget.dataSource.state == AnalysisState.error;

  void _handleDataSourceChanged() {
    if (!mounted) return;
    final signature = _buttonSignature();
    if (signature == _lastButtonSignature) return;
    _lastButtonSignature = signature;
    setState(() {});
  }

  String _buttonSignature() {
    final error = widget.dataSource.error;
    return [
      widget.dataSource.state.name,
      error?.key.name ?? '',
      error?.args.join('|') ?? '',
    ].join(';');
  }

  Future<void> _handlePressed() async {
    _showPanel();
    await widget.onPressed();
    if (!mounted) return;
    final error = widget.dataSource.error;
    if (error == null) return;
    if (error.key != AnalysisErrorKey.cacheLimitExceeded &&
        error.key != AnalysisErrorKey.cacheWriteIncomplete) {
      return;
    }
    AppFeedbackScope.read(context).showError(_errorText(context, error));
  }

  void _showPanel() {
    _hideTimer?.cancel();
    if (_overlayEntry != null) {
      _overlayEntry!.markNeedsBuild();
      _panelAnimationController.forward();
      return;
    }
    final overlay = Overlay.of(context);
    _overlayEntry = OverlayEntry(
      builder: (context) => CompositedTransformFollower(
        link: _layerLink,
        targetAnchor: Alignment.bottomRight,
        followerAnchor: Alignment.topRight,
        offset: const Offset(0, 4),
        showWhenUnlinked: false,
        child: FadeTransition(
          opacity: _panelOpacity,
          child: UnconstrainedBox(
            alignment: Alignment.topRight,
            child: MouseRegion(
              onEnter: (_) {
                _hoveringPanel = true;
                _hideTimer?.cancel();
              },
              onExit: (_) {
                _hoveringPanel = false;
                _scheduleHidePanel();
              },
              child: SizedBox(
                width: _analysisPanelWidth,
                child: _AnalysisHoverPanel(
                  tracks: widget.tracks,
                  dataSource: widget.dataSource,
                ),
              ),
            ),
          ),
        ),
      ),
    );
    overlay.insert(_overlayEntry!);
    _panelAnimationController.forward(from: 0);
  }

  void _markOverlayNeedsBuildAfterFrame() {
    final entry = _overlayEntry;
    if (entry == null) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || _overlayEntry != entry) return;
      entry.markNeedsBuild();
    });
  }

  void _scheduleHidePanel() {
    _hideTimer?.cancel();
    _hideTimer = Timer(const Duration(milliseconds: 180), () {
      if (_hoveringButton || _hoveringPanel) return;
      fireAndLog('hide analysis hover panel', _fadeOutPanel());
    });
  }

  Future<void> _fadeOutPanel() async {
    await _panelAnimationController.reverse();
    if (!mounted) return;
    if (_hoveringButton || _hoveringPanel) {
      await _panelAnimationController.forward();
      return;
    }
    _removePanel();
  }

  void _removePanel() {
    _overlayEntry?.remove();
    _overlayEntry = null;
    if (_panelAnimationController.isAnimating ||
        _panelAnimationController.value != 0) {
      _panelAnimationController.value = 0;
    }
  }

  String _errorText(BuildContext context, AnalysisError e) {
    final l = AppLocalizations.of(context)!;
    return switch (e.key) {
      AnalysisErrorKey.hashFailed => l.analysisErrorHashFailed(
        e.args.firstOrNull ?? '',
      ),
      AnalysisErrorKey.unsupported => l.analysisErrorUnsupported(
        e.args.firstOrNull ?? '',
      ),
      AnalysisErrorKey.loadFailed => l.analysisErrorLoadFailed(
        e.args.firstOrNull ?? '',
      ),
      AnalysisErrorKey.cacheLimitExceeded => l.analysisErrorCacheLimitExceeded(
        e.args.isNotEmpty ? e.args[0] : '',
        e.args.length > 1 ? e.args[1] : '',
      ),
      AnalysisErrorKey.cacheWriteIncomplete =>
        l.analysisErrorCacheWriteIncomplete(
          e.args.isNotEmpty ? e.args[0] : '',
          e.args.length > 1 ? e.args[1] : '',
          e.args.length > 2 ? e.args[2] : '',
        ),
    };
  }
}

class _AnalysisHoverPanel extends StatefulWidget {
  final List<TrackEntry> tracks;
  final AnalysisToolbarDataSource dataSource;

  const _AnalysisHoverPanel({required this.tracks, required this.dataSource});

  @override
  State<_AnalysisHoverPanel> createState() => _AnalysisHoverPanelState();
}

class _AnalysisHoverPanelState extends State<_AnalysisHoverPanel> {
  Timer? _refreshTimer;
  AnalysisCacheSnapshot? _snapshot;
  Map<String, int> _bytesByHash = const {};
  bool _refreshing = false;

  @override
  void initState() {
    super.initState();
    widget.dataSource.addListener(_refresh);
    fireAndLog('refresh analysis hover panel', _refresh());
    _refreshTimer = Timer.periodic(
      const Duration(milliseconds: 700),
      (_) => fireAndLog('refresh analysis hover panel', _refresh()),
    );
  }

  @override
  void didUpdateWidget(covariant _AnalysisHoverPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.tracks != widget.tracks) {
      fireAndLog('refresh analysis hover panel', _refresh());
    }
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    widget.dataSource.removeListener(_refresh);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final snapshot = _snapshot;

    return Material(
      elevation: 12,
      color: theme.colorScheme.surface,
      borderRadius: BorderRadius.circular(8),
      clipBehavior: Clip.antiAlias,
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxHeight: _analysisPanelMaxHeight),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              height: _analysisPanelHeaderHeight,
              child: Row(
                children: [
                  const SizedBox(width: 10),
                  Icon(
                    Icons.analytics_outlined,
                    size: 16,
                    color: theme.colorScheme.primary,
                  ),
                  const SizedBox(width: 6),
                  Expanded(
                    child: Text(
                      l.analysisCachePanelTitle,
                      style: theme.textTheme.labelLarge,
                    ),
                  ),
                  if (snapshot != null)
                    Padding(
                      padding: const EdgeInsets.only(right: 10),
                      child: Text(
                        snapshot.hasLimit
                            ? l.cacheUsageWithLimit(
                                widget.dataSource.formatBytes(
                                  snapshot.totalBytes,
                                ),
                                widget.dataSource.formatBytes(
                                  snapshot.maxBytes,
                                ),
                              )
                            : l.cacheUsageUnlimited(
                                widget.dataSource.formatBytes(
                                  snapshot.totalBytes,
                                ),
                              ),
                        style: theme.textTheme.labelMedium?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant,
                        ),
                      ),
                    ),
                ],
              ),
            ),
            const Divider(height: 1),
            if (widget.tracks.isEmpty)
              Padding(
                padding: const EdgeInsets.all(12),
                child: Text(
                  l.analysisCachePanelEmpty,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              )
            else
              Flexible(
                child: ListView.separated(
                  shrinkWrap: true,
                  padding: EdgeInsets.zero,
                  itemCount: widget.tracks.length,
                  separatorBuilder: (_, _) => const Divider(height: 1),
                  itemBuilder: (context, index) {
                    return SizedBox(
                      height: _analysisPanelRowHeight,
                      child: _AnalysisTrackCacheTile(
                        track: widget.tracks[index],
                        snapshot: snapshot,
                        bytesByHash: _bytesByHash,
                        dataSource: widget.dataSource,
                      ),
                    );
                  },
                ),
              ),
          ],
        ),
      ),
    );
  }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      final snapshot = await widget.dataSource.snapshot();
      final hashes = <String>{};
      for (final track in widget.tracks) {
        final statusHash = widget.dataSource.statusForPath(track.path)?.hash;
        if (statusHash != null) hashes.add(statusHash);
        for (final entry in snapshot.entries) {
          if (entry.videoPath == track.path) hashes.add(entry.hash);
        }
      }
      final bytesByHash = await widget.dataSource.currentBytesByHash(hashes);
      if (!mounted) return;
      setState(() {
        _snapshot = snapshot;
        _bytesByHash = bytesByHash;
      });
    } finally {
      _refreshing = false;
    }
  }
}

class _AnalysisTrackCacheTile extends StatelessWidget {
  final TrackEntry track;
  final AnalysisCacheSnapshot? snapshot;
  final Map<String, int> bytesByHash;
  final AnalysisToolbarDataSource dataSource;

  const _AnalysisTrackCacheTile({
    required this.track,
    required this.snapshot,
    required this.bytesByHash,
    required this.dataSource,
  });

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final status = dataSource.statusForPath(track.path);
    final cacheEntry = _cacheEntry(status);
    final hash = status?.hash ?? cacheEntry?.hash;
    final cacheBytes = hash == null
        ? cacheEntry?.analysisBytes ?? 0
        : bytesByHash[hash] ?? cacheEntry?.analysisBytes ?? 0;
    final working = status?.isWorking ?? false;
    final failed = status?.isError ?? false;
    final cached =
        (cacheEntry?.complete ?? false) || (status?.isCached ?? false);
    final color = failed
        ? theme.colorScheme.error
        : cached
        ? Colors.green
        : theme.colorScheme.onSurfaceVariant;

    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      child: Row(
        children: [
          SizedBox(
            width: 20,
            height: 20,
            child: working
                ? const Padding(
                    padding: EdgeInsets.all(3),
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : Icon(
                    failed
                        ? Icons.error_outline
                        : cached
                        ? Icons.check_circle_outline
                        : Icons.storage_outlined,
                    size: 16,
                    color: color,
                  ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  track.fileName,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.bodySmall?.copyWith(
                    color: failed ? theme.colorScheme.error : null,
                  ),
                ),
                Text(
                  _statusText(l, status, cached, cacheBytes),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: failed
                        ? theme.colorScheme.error
                        : theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(width: 8),
          Text(
            dataSource.formatBytes(cacheBytes),
            style: theme.textTheme.labelSmall?.copyWith(color: color),
          ),
        ],
      ),
    );
  }

  AnalysisCacheEntryStats? _cacheEntry(AnalysisTrackGenerationStatus? status) {
    final entries = snapshot?.entries ?? const <AnalysisCacheEntryStats>[];
    for (final entry in entries) {
      if (status?.hash != null && entry.hash == status!.hash) return entry;
      if (entry.videoPath == track.path) return entry;
    }
    return null;
  }

  String _statusText(
    AppLocalizations l,
    AnalysisTrackGenerationStatus? status,
    bool cached,
    int cacheBytes,
  ) {
    if (status?.isError ?? false) return l.analysisCacheStatusFailed;
    if (status?.status == AnalysisTrackStatus.computingHash) {
      return l.analysisCacheStatusChecking;
    }
    if (status?.status == AnalysisTrackStatus.generating) {
      return l.analysisCacheStatusGenerating(
        ((status!.progress.clamp(0.0, 1.0)) * 100).toStringAsFixed(0),
        dataSource.formatBytes(cacheBytes),
      );
    }
    if (status?.status == AnalysisTrackStatus.loading) {
      return l.analysisCacheStatusLoading;
    }
    if (cached) return l.analysisCacheStatusCached;
    return l.analysisCacheStatusMissing;
  }
}
