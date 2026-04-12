import 'app_localizations.dart';

/// Resolves a [labelKey] (defined in `PlayerAction.shortcutEntries`) to its
/// localized string using the generated [AppLocalizations].
///
/// Flutter's generated l10n uses compile-time getters, so a manual mapping is
/// required.  When a new action label is added to the `.arb` files, add a
/// corresponding `case` here — the settings window will then pick it up
/// automatically.
String resolveActionLabel(String labelKey, AppLocalizations l) {
  return switch (labelKey) {
    'actionTogglePlay' => l.actionTogglePlay,
    'actionStepForward' => l.actionStepForward,
    'actionStepBackward' => l.actionStepBackward,
    'actionOpenFile' => l.actionOpenFile,
    'actionToggleLayout' => l.actionToggleLayout,
    'actionSeekForward' => l.actionSeekForward,
    'actionSeekBackward' => l.actionSeekBackward,
    'actionOpenAnalysis' => l.actionOpenAnalysis,
    _ => labelKey,
  };
}
