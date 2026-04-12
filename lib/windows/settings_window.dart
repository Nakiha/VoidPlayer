import 'package:flutter/material.dart';
import '../actions/player_action.dart';
import '../l10n/action_labels.dart';
import '../l10n/app_localizations.dart';

/// Settings window (secondary window, NavigationRail: Shortcuts | About).
class SettingsApp extends StatelessWidget {
  final Color accentColor;
  const SettingsApp({super.key, required this.accentColor});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Settings',
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.dark,
        ),
      ),
      home: const SettingsPage(),
    );
  }
}

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  int _selectedIndex = 0;

  @override
  Widget build(BuildContext context) {
    final l = AppLocalizations.of(context)!;
    return Scaffold(
      body: Row(
        children: [
          NavigationRail(
            selectedIndex: _selectedIndex,
            onDestinationSelected: (index) {
              setState(() => _selectedIndex = index);
            },
            labelType: NavigationRailLabelType.all,
            destinations: [
              NavigationRailDestination(
                icon: const Icon(Icons.keyboard),
                label: Text(l.shortcuts),
              ),
              NavigationRailDestination(
                icon: const Icon(Icons.info_outline),
                label: Text(l.about),
              ),
            ],
          ),
          const VerticalDivider(thickness: 1, width: 1),
          Expanded(
            child: IndexedStack(
              index: _selectedIndex,
              children: const [
                _ShortcutsPage(),
                _AboutPage(),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _ShortcutsPage extends StatelessWidget {
  const _ShortcutsPage();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
          child: Text(l.keyboardShortcuts, style: theme.textTheme.titleMedium),
        ),
        const SizedBox(height: 8),
        Expanded(
          child: SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(8, 0, 16, 16),
            child: Table(
              columnWidths: const {
                0: FlexColumnWidth(),
                1: IntrinsicColumnWidth(),
              },
              defaultVerticalAlignment: TableCellVerticalAlignment.middle,
              children: [
                // Header
                TableRow(
                  children: [
                    Padding(
                      padding: const EdgeInsets.all(8),
                      child: Text(l.action,
                          style: theme.textTheme.labelSmall),
                    ),
                    Padding(
                      padding: const EdgeInsets.all(8),
                      child: Text(l.shortcut,
                          style: theme.textTheme.labelSmall),
                    ),
                  ],
                ),
                // Divider
                TableRow(children: [
                  Divider(height: 1, color: theme.dividerColor),
                  Divider(height: 1, color: theme.dividerColor),
                ]),
                // Data rows
                ...PlayerAction.shortcutEntries.map((e) => TableRow(
                  children: [
                    Padding(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 8, vertical: 4),
                      child: Text(resolveActionLabel(e.labelKey, l),
                          style: theme.textTheme.bodySmall),
                    ),
                    Padding(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 8, vertical: 4),
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 8, vertical: 2),
                        decoration: BoxDecoration(
                          color: theme.colorScheme.surfaceContainerHighest,
                          borderRadius: BorderRadius.circular(4),
                        ),
                        child: Text(e.shortcutLabel,
                            style: theme.textTheme.bodySmall?.copyWith(
                                  fontFamily: 'monospace',
                                )),
                      ),
                    ),
                  ],
                )),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class _AboutPage extends StatelessWidget {
  const _AboutPage();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final l = AppLocalizations.of(context)!;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(l.appTitle, style: theme.textTheme.headlineMedium),
          const SizedBox(height: 4),
          Text(l.versionLabel,
              style: theme.textTheme.bodyMedium?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  )),
          const SizedBox(height: 16),
          Text(l.appDescription,
              style: theme.textTheme.bodyMedium),
          const SizedBox(height: 24),
          Text(l.dependencies, style: theme.textTheme.titleSmall),
          const SizedBox(height: 8),
          _depItem('Flutter', 'BSD-3-Clause'),
          _depItem('FFmpeg', 'LGPL-2.1+'),
          _depItem('Direct3D 11', 'MIT'),
          _depItem('flutter_acrylic', 'MIT'),
          _depItem('desktop_multi_window', 'MIT'),
          const Spacer(),
          Text(l.license,
              style: theme.textTheme.bodySmall?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  )),
        ],
      ),
    );
  }

  Widget _depItem(String name, String license) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        children: [
          SizedBox(
            width: 180,
            child: Text(name),
          ),
          Text(license,
              style: TextStyle(
                color: Colors.grey[400],
                fontSize: 12,
              )),
        ],
      ),
    );
  }
}
