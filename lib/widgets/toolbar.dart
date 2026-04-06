import 'package:flutter/material.dart';
import '../l10n/app_localizations.dart';
import 'segmented_widget.dart';

/// Top toolbar matching PySide6 ToolBar (40px height, margins: 4).
class AppToolBar extends StatelessWidget {
  final int viewMode; // 0=sideBySide, 1=splitScreen
  final ValueChanged<int> onViewModeChanged;
  final VoidCallback onAddMedia;
  final VoidCallback onNewWindow;
  final VoidCallback onSettings;
  final VoidCallback onDebugMemory;
  final bool viewModeEnabled;

  const AppToolBar({
    super.key,
    required this.viewMode,
    required this.onViewModeChanged,
    required this.onAddMedia,
    required this.onNewWindow,
    required this.onSettings,
    required this.onDebugMemory,
    this.viewModeEnabled = false,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 40,
      padding: const EdgeInsets.all(4),
      child: Row(
        children: [
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
          // Add Media button
          SizedBox(
            height: 32,
            child: FilledButton.icon(
              onPressed: onAddMedia,
              icon: const Icon(Icons.add, size: 16),
              label: Text(AppLocalizations.of(context)!.addMedia),
              style: FilledButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 12),
                visualDensity: VisualDensity.compact,
              ),
            ),
          ),
          const SizedBox(width: 4),
          // New Window button
          SizedBox(
            height: 32,
            child: OutlinedButton(
              onPressed: onNewWindow,
              style: OutlinedButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 12),
                visualDensity: VisualDensity.compact,
              ),
              child: Text(AppLocalizations.of(context)!.newWindow),
            ),
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
          const SizedBox(width: 4),
          // Debug / Memory monitor button
          SizedBox(
            width: 32,
            height: 32,
            child: IconButton(
              onPressed: onDebugMemory,
              icon: const Icon(Icons.speed, size: 18),
              tooltip: AppLocalizations.of(context)!.performanceMonitor,
              padding: EdgeInsets.zero,
              constraints: const BoxConstraints.tightFor(width: 32, height: 32),
            ),
          ),
        ],
      ),
    );
  }
}
