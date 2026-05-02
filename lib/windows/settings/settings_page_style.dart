import 'package:flutter/material.dart';

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
    return MenuAnchor(
      alignmentOffset: const Offset(0, 4),
      style: MenuStyle(
        backgroundColor: WidgetStatePropertyAll(
          theme.colorScheme.surfaceContainerHigh,
        ),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
        ),
        padding: const WidgetStatePropertyAll(
          EdgeInsets.symmetric(vertical: 4),
        ),
      ),
      menuChildren: [
        for (final item in items)
          MenuItemButton(
            leadingIcon: item == value
                ? Icon(Icons.check, size: 16, color: theme.colorScheme.primary)
                : const SizedBox(width: 16),
            requestFocusOnHover: false,
            style: ButtonStyle(
              padding: WidgetStatePropertyAll(
                EdgeInsets.only(left: item == value ? 8.0 : 12.0, right: 16),
              ),
            ),
            onPressed: () => onChanged(item),
            child: SizedBox(
              width: 160,
              child: Text(
                labelFor(item),
                style: SettingsPageStyle.body(context)?.copyWith(
                  color: item == value ? theme.colorScheme.primary : null,
                ),
              ),
            ),
          ),
      ],
      builder: (context, controller, child) {
        return SizedBox(
          height: 36,
          child: Material(
            color: Colors.transparent,
            child: InkWell(
              onTap: () {
                if (controller.isOpen) {
                  controller.close();
                } else {
                  controller.open();
                }
              },
              borderRadius: BorderRadius.circular(6),
              child: DecoratedBox(
                decoration: BoxDecoration(
                  border: Border.all(color: theme.colorScheme.outlineVariant),
                  borderRadius: BorderRadius.circular(6),
                ),
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 10),
                  child: Row(
                    children: [
                      Expanded(
                        child: Text(
                          labelFor(value),
                          style: SettingsPageStyle.body(context),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                      Icon(
                        Icons.arrow_drop_down,
                        size: 20,
                        color: theme.iconTheme.color,
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        );
      },
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
