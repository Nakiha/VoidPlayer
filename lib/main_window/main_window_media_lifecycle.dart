import '../track_manager.dart';
import 'main_window_playback.dart';
import 'main_window_state.dart';

class MainWindowMediaLifecycle {
  final MainWindowStateStore stateStore;
  final TrackManager trackManager;
  final MainWindowPlaybackCoordinator playbackCoordinator;
  final void Function(bool fullScreen, {required String reason})
  requestFullScreen;

  const MainWindowMediaLifecycle({
    required this.stateStore,
    required this.trackManager,
    required this.playbackCoordinator,
    required this.requestFullScreen,
  });

  void applyStartupLoopRangeIfReady() {
    playbackCoordinator.applyStartupLoopRangeIfReady();
  }

  void seekTo(int ptsUs) {
    playbackCoordinator.seekTo(ptsUs);
  }

  void resetAfterLastTrackRemoved() {
    if (stateStore.value.fullScreen) {
      requestFullScreen(
        false,
        reason: 'exit full screen after last track removed',
      );
    }
    trackManager.clear();
    stateStore.resetAfterLastTrackRemoved();
    playbackCoordinator.invalidateLoopRangeSync();
  }

  void preparePlayerDestroyAfterLastTrackRemoved() {
    playbackCoordinator.cancelLoopBoundaryTimer();
  }
}
