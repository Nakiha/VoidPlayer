import '../analysis/ui/analysis_ui_selection.dart';
import '../marks/quick_mark.dart';
import '../video_renderer_controller.dart';

sealed class MainWindowSelection {
  const MainWindowSelection();

  bool get isEmpty => this is MainWindowNoSelection;
}

final class MainWindowNoSelection extends MainWindowSelection {
  const MainWindowNoSelection();
}

final class MainWindowQuickMarkSelection extends MainWindowSelection {
  final QuickMark mark;

  const MainWindowQuickMarkSelection(this.mark);
}

final class MainWindowAnalysisSelection extends MainWindowSelection {
  final AnalysisUiSelection selection;

  const MainWindowAnalysisSelection(this.selection);
}

final class MainWindowTrackSelection extends MainWindowSelection {
  final TrackInfo track;

  const MainWindowTrackSelection(this.track);
}
