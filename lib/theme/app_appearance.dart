import 'package:flutter/material.dart';

import '../config/app_config.dart';

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
    required this.systemAccentColor,
    required AppThemePreference themePreference,
    required AppAccentPreference accentPreference,
    required Color customAccentColor,
  }) : _themePreference = themePreference,
       _accentPreference = accentPreference,
       _customAccentColor = customAccentColor;

  factory AppAppearanceController.load({required Color systemAccentColor}) {
    final config = AppConfig.instance;
    return AppAppearanceController._(
      systemAccentColor: systemAccentColor,
      themePreference: AppThemePreference.fromStorage(
        config.themeModePreference,
      ),
      accentPreference: AppAccentPreference.fromStorage(
        config.accentColorPreference,
      ),
      customAccentColor: Color(config.customAccentColorValue),
    );
  }

  Color systemAccentColor;
  AppThemePreference _themePreference;
  AppAccentPreference _accentPreference;
  Color _customAccentColor;

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
    AppConfig.instance.themeModePreference = preference.storageValue;
    notifyListeners();
    await AppConfig.instance.save();
  }

  Future<void> setAccentPreference(AppAccentPreference preference) async {
    if (_accentPreference == preference) return;
    _accentPreference = preference;
    AppConfig.instance.accentColorPreference = preference.storageValue;
    notifyListeners();
    await AppConfig.instance.save();
  }

  Future<void> setCustomAccentColor(Color color) async {
    if (_customAccentColor == color) return;
    _customAccentColor = color;
    AppConfig.instance.customAccentColorValue = color.toARGB32();
    notifyListeners();
    await AppConfig.instance.save();
  }
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
