import '../../track_manager.dart';
import 'main_window_state.dart';

class MainWindowTimelineMetrics {
  final MainWindowStateStore stateStore;
  final TrackManager trackManager;

  const MainWindowTimelineMetrics({
    required this.stateStore,
    required this.trackManager,
  });

  int get effectiveDurationUs {
    final state = stateStore.value;
    int maxEffective = state.durationUs;
    for (final entry in trackManager.entries) {
      final offsetUs = state.syncOffsets[entry.fileId] ?? 0;
      final effective = entry.info.durationUs + offsetUs;
      if (effective > maxEffective) maxEffective = effective;
    }
    return maxEffective;
  }
}
