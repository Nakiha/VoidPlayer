import 'package:flutter/material.dart';

import '../../widgets/app_menu_combo.dart';

class SettingsPageStyle {
  const SettingsPageStyle._();

  static const comboWidth = 260.0;
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

class SettingsComboRow<T> extends StatelessWidget {
  final String label;
  final IconData icon;
  final T value;
  final List<T> items;
  final String Function(T value) labelFor;
  final ValueChanged<T> onChanged;

  const SettingsComboRow({
    super.key,
    required this.label,
    required this.icon,
    required this.value,
    required this.items,
    required this.labelFor,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return ConstrainedBox(
      constraints: const BoxConstraints(minHeight: 36),
      child: Row(
        children: [
          Icon(icon, size: 20, color: theme.colorScheme.primary),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              label,
              style: SettingsPageStyle.body(context),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
          ),
          const SizedBox(width: 16),
          SizedBox(
            width: SettingsPageStyle.comboWidth,
            child: SettingsMenuCombo<T>(
              value: value,
              items: items,
              labelFor: labelFor,
              onChanged: onChanged,
            ),
          ),
        ],
      ),
    );
  }
}

class SettingsMenuCombo<T> extends StatelessWidget {
  final T value;
  final List<T> items;
  final String Function(T value) labelFor;
  final ValueChanged<T> onChanged;

  const SettingsMenuCombo({
    super.key,
    required this.value,
    required this.items,
    required this.labelFor,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return AppMenuCombo<T>(
      height: 36,
      value: value,
      items: items,
      labelFor: labelFor,
      onChanged: onChanged,
      buttonPadding: const EdgeInsets.symmetric(horizontal: 10),
      border: Border.all(color: theme.colorScheme.outlineVariant),
      borderRadius: BorderRadius.circular(6),
      textStyle: SettingsPageStyle.body(context),
      menuTextStyle: SettingsPageStyle.body(context),
      iconSize: 20,
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
