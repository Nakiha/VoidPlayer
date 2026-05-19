import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    MacOSVideoRendererStub.register(with: flutterViewController.engine.binaryMessenger)
    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

private final class MacOSVideoRendererStub: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"

  static func register(with messenger: FlutterBinaryMessenger) {
    let stub = MacOSVideoRendererStub()
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    channel.setMethodCallHandler(stub.handle)

    let events = FlutterEventChannel(name: eventsChannelName, binaryMessenger: messenger)
    events.setStreamHandler(stub)
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging",
         "destroyPlayer",
         "play",
         "pause",
         "seek",
         "setSpeed",
         "setLoopRange",
         "setAudibleTrack",
         "resize",
         "setViewportBackgroundColor",
         "stepForward",
         "stepBackward",
         "applyLayout",
         "removeTrack",
         "setTrackOffset":
      result(nil)
    case "createPlayer", "addTrack":
      result(unsupportedPlayerError(call.method))
    case "currentPts", "duration":
      result(0)
    case "currentPresentedFrame":
      result(nil)
    case "isPlaying":
      result(false)
    case "getLayout":
      result(defaultLayout())
    case "getTracks", "pickFiles":
      result([])
    case "getDiagnostics":
      result([
        "platform": "macos",
        "backend": "stub",
        "available": false,
        "reason": "macOS native playback is not implemented yet",
      ])
    case "captureViewport":
      result(FlutterError(
        code: "NO_PLAYER",
        message: "macOS native playback is not implemented yet",
        details: nil
      ))
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func unsupportedPlayerError(_ method: String) -> FlutterError {
    return FlutterError(
      code: "UNSUPPORTED_PLATFORM",
      message: "\(method) is not available on macOS yet",
      details: "VoidPlayer macOS native playback is still in the platform-boundary phase."
    )
  }

  private func defaultLayout() -> [String: Any] {
    return [
      "mode": 0,
      "splitPos": 0.5,
      "zoomRatio": 1.0,
      "viewOffsetX": 0.0,
      "viewOffsetY": 0.0,
      "pixelSizeMode": 0,
      "order": [0, 1, 2, 3],
    ]
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    return nil
  }
}
