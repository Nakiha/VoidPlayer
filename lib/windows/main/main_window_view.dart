import 'package:desktop_drop/desktop_drop.dart';
import 'package:flutter/material.dart';

import '../../platform/pointer_button_state_provider.dart';
import '../../utils/media_source.dart';
import 'main_window_scaffold.dart';
import 'main_window_view_model.dart';

class MainWindowView extends StatelessWidget {
  final MainWindowViewModel model;
  final MainWindowViewActions actions;
  final PointerButtonStateProvider pointerButtonStateProvider;

  const MainWindowView({
    super.key,
    required this.model,
    required this.actions,
    this.pointerButtonStateProvider = emptyPointerButtonStateProvider,
  });

  @override
  Widget build(BuildContext context) {
    return DropTarget(
      onDragEntered: (_) => actions.drop.dragEntered(),
      onDragExited: (_) => actions.drop.dragExited(),
      onDragDone: (details) {
        final sources = mediaSourcesFromDroppedValues(
          details.files.map((f) => f.path),
        );
        if (sources.isNotEmpty) actions.drop.filesDropped(sources);
      },
      child: RepaintBoundary(
        key: model.fullFrameCaptureKey,
        child: MainWindowScaffold(
          model: model,
          actions: actions,
          pointerButtonStateProvider: pointerButtonStateProvider,
        ),
      ),
    );
  }
}
