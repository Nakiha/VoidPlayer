import 'dart:async';

import 'package:flutter/material.dart';

enum AppFeedbackSeverity { info, success, warning, error }

class AppFeedbackMessage {
  final String text;
  final AppFeedbackSeverity severity;
  final String? actionLabel;
  final VoidCallback? onAction;
  final Duration duration;

  const AppFeedbackMessage({
    required this.text,
    this.severity = AppFeedbackSeverity.info,
    this.actionLabel,
    this.onAction,
    this.duration = const Duration(seconds: 4),
  });

  bool get isPersistent => duration == Duration.zero;
}

class AppFeedbackController extends ChangeNotifier {
  AppFeedbackMessage? _current;
  Timer? _dismissTimer;

  AppFeedbackMessage? get current => _current;

  void show(AppFeedbackMessage message) {
    if (_current?.text == message.text &&
        _current?.severity == message.severity) {
      _scheduleDismiss(message);
      return;
    }
    _current = message;
    notifyListeners();
    _scheduleDismiss(message);
  }

  void showError(String text, {String? actionLabel, VoidCallback? onAction}) {
    show(
      AppFeedbackMessage(
        text: text,
        severity: AppFeedbackSeverity.error,
        actionLabel: actionLabel,
        onAction: onAction,
        duration: const Duration(seconds: 6),
      ),
    );
  }

  void showInfo(String text) {
    show(AppFeedbackMessage(text: text));
  }

  void dismiss() {
    _dismissTimer?.cancel();
    _dismissTimer = null;
    if (_current == null) return;
    _current = null;
    notifyListeners();
  }

  void _scheduleDismiss(AppFeedbackMessage message) {
    _dismissTimer?.cancel();
    _dismissTimer = null;
    if (message.isPersistent) return;
    _dismissTimer = Timer(message.duration, () {
      if (!identical(_current, message)) return;
      dismiss();
    });
  }

  @override
  void dispose() {
    _dismissTimer?.cancel();
    super.dispose();
  }
}

class AppFeedbackScope extends InheritedWidget {
  final AppFeedbackController controller;

  const AppFeedbackScope({
    super.key,
    required this.controller,
    required super.child,
  });

  static AppFeedbackController of(BuildContext context) {
    final scope = context
        .dependOnInheritedWidgetOfExactType<AppFeedbackScope>();
    assert(scope != null, 'AppFeedbackScope was not found.');
    return scope!.controller;
  }

  static AppFeedbackController read(BuildContext context) {
    final element = context
        .getElementForInheritedWidgetOfExactType<AppFeedbackScope>();
    final scope = element?.widget as AppFeedbackScope?;
    assert(scope != null, 'AppFeedbackScope was not found.');
    return scope!.controller;
  }

  @override
  bool updateShouldNotify(AppFeedbackScope oldWidget) =>
      !identical(controller, oldWidget.controller);
}
