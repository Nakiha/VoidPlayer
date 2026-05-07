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
    if (trackManager.isEmpty) {
      return state.durationUs;
    }

    var maxEffective = 0;
    for (final entry in trackManager.entries) {
      final offsetUs = state.syncOffsets[entry.fileId] ?? 0;
      final effective =
          (entry.info.startTimeUs + entry.info.durationUs + offsetUs)
              .clamp(0, 1 << 62)
              .toInt();
      if (effective > maxEffective) maxEffective = effective;
    }

    return maxEffective > 0 ? maxEffective : state.durationUs;
  }
}
