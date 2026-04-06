import 'package:flutter/services.dart';
import '../video_renderer_controller.dart';

/// Inter-window communication channel.
/// Main window sets up a handler; secondary windows call through static methods.
class PlayerChannel {
  static const _channelName = 'player_channel';

  // --- Main window side: set up handler ---

  /// Set up the method call handler in the main window.
  /// Secondary windows invoke methods to query player state.
  static void setupMainHandler(VideoRendererController controller) {
    const channel = MethodChannel(_channelName);
    channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'isPlaying':
          return await controller.isPlaying();
        case 'currentPts':
          return await controller.currentPts();
        case 'duration':
          return await controller.duration();
        case 'getLayout':
          final layout = await controller.getLayout();
          return layout.toMap();
        default:
          throw MissingPluginException('Not implemented: ${call.method}');
      }
    });
  }
}
