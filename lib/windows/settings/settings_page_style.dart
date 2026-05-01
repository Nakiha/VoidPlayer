import 'package:flutter/material.dart';

class SettingsPageStyle {
  const SettingsPageStyle._();

  static const pagePadding = EdgeInsets.all(16);
  static const contentGap = SizedBox(height: 16);
  static const compactGap = SizedBox(height: 8);

  static TextStyle? title(BuildContext context) {
    return Theme.of(
      context,
    ).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w600);
  }

  static TextStyle? sectionTitle(BuildContext context) {
    return Theme.of(
      context,
    ).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600);
  }

  static TextStyle? body(BuildContext context) {
    return Theme.of(context).textTheme.bodyMedium;
  }

  static TextStyle? secondary(BuildContext context) {
    final theme = Theme.of(context);
    return theme.textTheme.bodyMedium?.copyWith(
      color: theme.colorScheme.onSurfaceVariant,
    );
  }

  static TextStyle? tableHeader(BuildContext context) {
    return Theme.of(
      context,
    ).textTheme.labelLarge?.copyWith(fontWeight: FontWeight.w600);
  }

  static TextStyle? shortcutKey(BuildContext context) {
    return Theme.of(context).textTheme.bodyMedium?.copyWith(
      fontFamily: 'Consolas',
      fontFamilyFallback: const ['Cascadia Mono', 'monospace'],
      fontWeight: FontWeight.w600,
    );
  }
}

class SettingsPageTitle extends StatelessWidget {
  final String text;
  final Widget? trailing;

  const SettingsPageTitle({super.key, required this.text, this.trailing});

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(child: Text(text, style: SettingsPageStyle.title(context))),
        ?trailing,
      ],
    );
  }
}
