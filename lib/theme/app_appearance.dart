import 'dart:async';

import 'package:flutter/material.dart';

import '../config/app_settings_repository.dart';

enum AppThemePreference {
  system('system'),
  light('light'),
  dark('dark');

  const AppThemePreference(this.storageValue);

  final String storageValue;

  ThemeMode get themeMode {
    return switch (this) {
      AppThemePreference.light => ThemeMode.light,
      AppThemePreference.dark => ThemeMode.dark,
      AppThemePreference.system => ThemeMode.system,
    };
  }

  static AppThemePreference fromStorage(String value) {
    return AppThemePreference.values.firstWhere(
      (preference) => preference.storageValue == value,
      orElse: () => AppThemePreference.system,
    );
  }
}

enum AppAccentPreference {
  system('system'),
  custom('custom');

  const AppAccentPreference(this.storageValue);

  final String storageValue;

  static AppAccentPreference fromStorage(String value) {
    return value == custom.storageValue ? custom : system;
  }
}

class AppAppearanceController extends ChangeNotifier {
  AppAppearanceController._({
    required AppSettingsRepository settings,
    required this.systemAccentColor,
    required AppThemePreference themePreference,
    required AppAccentPreference accentPreference,
    required Color customAccentColor,
  }) : _settings = settings,
       _themePreference = themePreference,
       _accentPreference = accentPreference,
       _customAccentColor = customAccentColor;

  factory AppAppearanceController.load({
    required AppSettingsRepository settings,
    required Color systemAccentColor,
  }) {
    return AppAppearanceController._(
      settings: settings,
      systemAccentColor: systemAccentColor,
      themePreference: AppThemePreference.fromStorage(
        settings.themeModePreference,
      ),
      accentPreference: AppAccentPreference.fromStorage(
        settings.accentColorPreference,
      ),
      customAccentColor: Color(settings.customAccentColorValue),
    );
  }

  final AppSettingsRepository _settings;
  Color systemAccentColor;
  AppThemePreference _themePreference;
  AppAccentPreference _accentPreference;
  Color _customAccentColor;
  Timer? _customAccentSaveTimer;

  AppThemePreference get themePreference => _themePreference;
  AppAccentPreference get accentPreference => _accentPreference;
  Color get customAccentColor => _customAccentColor;
  ThemeMode get themeMode => _themePreference.themeMode;
  Color get accentColor => _accentPreference == AppAccentPreference.system
      ? systemAccentColor
      : _customAccentColor;

  Future<void> setThemePreference(AppThemePreference preference) async {
    if (_themePreference == preference) return;
    _themePreference = preference;
    _settings.themeModePreference = preference.storageValue;
    notifyListeners();
    await _settings.save();
  }

  Future<void> setAccentPreference(AppAccentPreference preference) async {
    if (_accentPreference == preference) return;
    _accentPreference = preference;
    _settings.accentColorPreference = preference.storageValue;
    notifyListeners();
    await _settings.save();
  }

  Future<void> setCustomAccentColor(Color color) async {
    if (_customAccentColor == color) return;
    _customAccentColor = color;
    _settings.customAccentColorValue = color.toARGB32();
    notifyListeners();
    _customAccentSaveTimer?.cancel();
    _customAccentSaveTimer = Timer(const Duration(milliseconds: 400), () {
      _customAccentSaveTimer = null;
      unawaited(_settings.save());
    });
  }

  @override
  void dispose() {
    final hadPendingCustomAccentSave = _customAccentSaveTimer != null;
    _customAccentSaveTimer?.cancel();
    _customAccentSaveTimer = null;
    if (hadPendingCustomAccentSave) {
      unawaited(_settings.save());
    }
    super.dispose();
  }
}

class AppSettingsScope extends InheritedWidget {
  final AppSettingsRepository settings;

  const AppSettingsScope({
    super.key,
    required this.settings,
    required super.child,
  });

  static AppSettingsRepository of(BuildContext context) {
    final scope = context
        .dependOnInheritedWidgetOfExactType<AppSettingsScope>();
    assert(scope != null, 'AppSettingsScope was not found.');
    return scope!.settings;
  }

  static AppSettingsRepository read(BuildContext context) {
    final element = context
        .getElementForInheritedWidgetOfExactType<AppSettingsScope>();
    final scope = element?.widget as AppSettingsScope?;
    assert(scope != null, 'AppSettingsScope was not found.');
    return scope!.settings;
  }

  @override
  bool updateShouldNotify(AppSettingsScope oldWidget) =>
      !identical(settings, oldWidget.settings);
}

class AppAppearanceScope extends InheritedNotifier<AppAppearanceController> {
  const AppAppearanceScope({
    super.key,
    required AppAppearanceController controller,
    required super.child,
  }) : super(notifier: controller);

  static AppAppearanceController of(BuildContext context) {
    final scope = context
        .dependOnInheritedWidgetOfExactType<AppAppearanceScope>();
    assert(scope?.notifier != null, 'AppAppearanceScope was not found.');
    return scope!.notifier!;
  }
}
