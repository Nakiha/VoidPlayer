import '../agent/agent_protocol_server.dart';
import '../track_manager.dart';
import 'main_window_media.dart';
import 'main_window_playback.dart';
import 'main_window_quick_marks.dart';
import 'main_window_state.dart';

/// Maps agent protocol methods onto main window coordinators. Keep this the
/// only place that defines what a connected agent is allowed to do.
class MainWindowAgentHandler implements AgentRequestHandler {
  final MainWindowStateStore stateStore;
  final TrackManager trackManager;
  final MainWindowMediaCoordinator mediaCoordinator;
  final MainWindowPlaybackCoordinator playbackCoordinator;
  final MainWindowQuickMarkCoordinator quickMarkCoordinator;

  MainWindowAgentHandler({
    required this.stateStore,
    required this.trackManager,
    required this.mediaCoordinator,
    required this.playbackCoordinator,
    required this.quickMarkCoordinator,
  });

  @override
  Future<Map<String, Object?>> handleRequest(
    String method,
    Map<String, Object?> params,
  ) async {
    switch (method) {
      case 'getSession':
        return _getSession();
      case 'addMedia':
        final path = _requireString(params, 'path');
        await mediaCoordinator.loadMediaPaths([path]);
        return {'trackCount': trackManager.count};
      case 'getMarks':
        return quickMarkCoordinator.buildMarksExportDocument();
      case 'exportMarks':
        final path = _requireString(params, 'path');
        await quickMarkCoordinator.exportMarksToFile(path);
        return {'path': path};
      case 'play':
        await playbackCoordinator.play();
        return const {};
      case 'pause':
        await playbackCoordinator.pause();
        return const {};
      case 'seekTo':
        playbackCoordinator.seekTo(_requireInt(params, 'ptsUs'));
        return const {};
      case 'setMediaSourceId':
        final slotIndex = _requireInt(params, 'slotIndex');
        final sourceId = _requireString(params, 'sourceId');
        await quickMarkCoordinator.declareSourceIdForSlot(slotIndex, sourceId);
        return const {};
      default:
        throw AgentRequestException('unknownMethod', 'unknown method $method');
    }
  }

  Future<Map<String, Object?>> _getSession() async {
    final media = await quickMarkCoordinator.buildExportMedia();
    final state = stateStore.value;
    return {
      'media': [for (final entry in media) entry.toJson()],
      'playback': {
        'isPlaying': state.isPlaying,
        'currentPtsUs': state.currentPtsUs,
        'durationUs': state.durationUs,
      },
    };
  }

  static String _requireString(Map<String, Object?> params, String key) {
    final value = params[key];
    if (value is String && value.trim().isNotEmpty) return value;
    throw AgentRequestException(
      'badRequest',
      'param "$key" must be a non-empty string',
    );
  }

  static int _requireInt(Map<String, Object?> params, String key) {
    final value = params[key];
    if (value is int) return value;
    throw AgentRequestException('badRequest', 'param "$key" must be an int');
  }
}
