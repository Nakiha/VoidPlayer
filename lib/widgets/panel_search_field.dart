import 'package:flutter/material.dart';

InputDecoration panelSearchInputDecoration(
  BuildContext context, {
  required String hintText,
  Widget? suffixIcon,
}) {
  final theme = Theme.of(context);
  final colors = theme.colorScheme;
  final border = OutlineInputBorder(
    borderRadius: BorderRadius.circular(8),
    borderSide: BorderSide(color: colors.outlineVariant),
  );
  return InputDecoration(
    hintText: hintText,
    hintStyle: theme.textTheme.bodySmall?.copyWith(
      color: colors.onSurfaceVariant,
    ),
    prefixIcon: const Icon(Icons.search, size: 15),
    prefixIconConstraints: const BoxConstraints.tightFor(width: 28, height: 28),
    suffixIcon: suffixIcon,
    isDense: true,
    filled: true,
    fillColor: colors.surfaceContainerLow,
    contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 7),
    border: border,
    enabledBorder: border,
  );
}
