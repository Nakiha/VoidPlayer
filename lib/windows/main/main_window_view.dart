import 'package:desktop_drop/desktop_drop.dart';
import 'package:flutter/material.dart';

import 'main_window_scaffold.dart';
import 'main_window_view_model.dart';

class MainWindowView extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;

  const MainWindowView({super.key, required this.model, required this.actions});

  @override
  Widget build(BuildContext context) {
    return DropTarget(
      onDragEntered: (_) => actions.onDragEntered(),
      onDragExited: (_) => actions.onDragExited(),
      onDragDone: (details) {
        final paths = details.files
            .map((f) => f.path)
            .where((path) => path.isNotEmpty)
            .toList();
        if (paths.isNotEmpty) actions.onFilesDropped(paths);
      },
      child: MainWindowScaffold(model: model, actions: actions),
    );
  }
}
