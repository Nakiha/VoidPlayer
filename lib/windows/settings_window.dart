import 'package:flutter/material.dart';
import '../actions/player_action.dart';
import '../l10n/action_labels.dart';
import '../l10n/app_localizations.dart';
import 'settings/appearance_settings_page.dart';
import 'settings/cache_settings_page.dart';
import 'settings/preferences_settings_page.dart';
import 'settings/settings_page_style.dart';

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
    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
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
              icon: const Icon(Icons.palette_outlined),
              label: Text(l.appearance),
            ),
            NavigationRailDestination(
              icon: const Icon(Icons.tune),
              label: Text(l.preferences),
            ),
            NavigationRailDestination(
              icon: const Icon(Icons.storage),
              label: Text(l.cache),
            ),
            NavigationRailDestination(
              icon: const Icon(Icons.info_outline),
              label: Text(l.about),
            ),
          ],
        ),
        const VerticalDivider(thickness: 1, width: 1),
        Expanded(child: _buildSelectedPage()),
      ],
    );
  }

  Widget _buildSelectedPage() {
    return switch (_selectedIndex) {
      0 => const _ShortcutsPage(),
      1 => const AppearanceSettingsPage(),
      2 => const PreferencesSettingsPage(),
      3 => const CacheSettingsPage(),
      _ => const _AboutPage(),
    };
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
          child: SettingsPageTitle(text: l.keyboardShortcuts),
        ),
        SettingsPageStyle.compactGap,
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
                      child: Text(
                        l.action,
                        style: SettingsPageStyle.tableHeader(context),
                      ),
                    ),
                    Padding(
                      padding: const EdgeInsets.all(8),
                      child: Text(
                        l.shortcut,
                        style: SettingsPageStyle.tableHeader(context),
                      ),
                    ),
                  ],
                ),
                // Divider
                TableRow(
                  children: [
                    Divider(height: 1, color: theme.dividerColor),
                    Divider(height: 1, color: theme.dividerColor),
                  ],
                ),
                // Data rows
                ...PlayerAction.shortcutEntries.map(
                  (e) => TableRow(
                    children: [
                      Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 8,
                          vertical: 6,
                        ),
                        child: Text(
                          resolveActionLabel(e.labelKey, l),
                          style: SettingsPageStyle.body(context),
                        ),
                      ),
                      Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 8,
                          vertical: 6,
                        ),
                        child: Container(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 10,
                            vertical: 4,
                          ),
                          decoration: BoxDecoration(
                            color: theme.colorScheme.surfaceContainerHighest,
                            borderRadius: BorderRadius.circular(4),
                          ),
                          child: Text(
                            e.shortcutLabel,
                            style: SettingsPageStyle.shortcutKey(context),
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
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
    final l = AppLocalizations.of(context)!;
    return Padding(
      padding: SettingsPageStyle.pagePadding,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SettingsPageTitle(text: l.appTitle),
          const SizedBox(height: 4),
          Text(l.versionLabel, style: SettingsPageStyle.secondary(context)),
          SettingsPageStyle.contentGap,
          Text(l.appDescription, style: SettingsPageStyle.body(context)),
          const SizedBox(height: 24),
          Text(l.dependencies, style: SettingsPageStyle.sectionTitle(context)),
          SettingsPageStyle.compactGap,
          _depItem('Flutter', 'BSD-3-Clause'),
          _depItem('FFmpeg', 'LGPL-2.1+'),
          _depItem('Direct3D 11', 'MIT'),
          _depItem('flutter_acrylic', 'MIT'),
          _depItem('window_manager', 'MIT'),
          const Spacer(),
          Text(l.license, style: SettingsPageStyle.secondary(context)),
        ],
      ),
    );
  }

  Widget _depItem(String name, String license) {
    return Builder(
      builder: (context) {
        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 3),
          child: Row(
            children: [
              SizedBox(
                width: 180,
                child: Text(name, style: SettingsPageStyle.body(context)),
              ),
              Text(license, style: SettingsPageStyle.secondary(context)),
            ],
          ),
        );
      },
    );
  }
}
