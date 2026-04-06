import 'package:flutter/material.dart';

/// Settings window (secondary window, NavigationRail: Shortcuts | About).
class SettingsApp extends StatelessWidget {
  const SettingsApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player - Settings',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0078D4),
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
    return Scaffold(
      body: Row(
        children: [
          NavigationRail(
            selectedIndex: _selectedIndex,
            onDestinationSelected: (index) {
              setState(() => _selectedIndex = index);
            },
            labelType: NavigationRailLabelType.all,
            destinations: const [
              NavigationRailDestination(
                icon: Icon(Icons.keyboard),
                label: Text('Shortcuts'),
              ),
              NavigationRailDestination(
                icon: Icon(Icons.info_outline),
                label: Text('About'),
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
    final actions = <Map<String, String>>[
      {'action': 'Toggle Play/Pause', 'key': 'Space'},
      {'action': 'Step Forward', 'key': 'Arrow Right'},
      {'action': 'Step Backward', 'key': 'Arrow Left'},
      {'action': 'Open File', 'key': 'O'},
      {'action': 'Toggle Layout Mode', 'key': 'M'},
      {'action': 'Seek Forward (+1s)', 'key': 'Shift + Arrow Right'},
      {'action': 'Seek Backward (-1s)', 'key': 'Shift + Arrow Left'},
    ];

    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Keyboard Shortcuts', style: theme.textTheme.titleMedium),
          const SizedBox(height: 16),
          Expanded(
            child: DataTable(
              headingTextStyle: theme.textTheme.labelSmall,
              dataTextStyle: theme.textTheme.bodySmall,
              columns: const [
                DataColumn(label: Text('Action')),
                DataColumn(label: Text('Shortcut')),
              ],
              rows: actions.map((a) => DataRow(cells: [
                    DataCell(Text(a['action']!)),
                    DataCell(Container(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                      decoration: BoxDecoration(
                        color: theme.colorScheme.surfaceContainerHighest,
                        borderRadius: BorderRadius.circular(4),
                      ),
                      child: Text(a['key']!,
                          style: theme.textTheme.bodySmall?.copyWith(
                                fontFamily: 'monospace',
                              )),
                    )),
                  ])).toList(),
            ),
          ),
        ],
      ),
    );
  }
}

class _AboutPage extends StatelessWidget {
  const _AboutPage();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Void Player', style: theme.textTheme.headlineMedium),
          const SizedBox(height: 4),
          Text('Version 1.0.0',
              style: theme.textTheme.bodyMedium?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  )),
          const SizedBox(height: 16),
          Text('A multi-track video comparison player.',
              style: theme.textTheme.bodyMedium),
          const SizedBox(height: 24),
          Text('Dependencies', style: theme.textTheme.titleSmall),
          const SizedBox(height: 8),
          _depItem('Flutter', 'BSD-3-Clause'),
          _depItem('FFmpeg', 'LGPL-2.1+'),
          _depItem('Direct3D 11', 'MIT'),
          _depItem('flutter_acrylic', 'MIT'),
          _depItem('desktop_multi_window', 'MIT'),
          const Spacer(),
          Text('License: GPLv3',
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
