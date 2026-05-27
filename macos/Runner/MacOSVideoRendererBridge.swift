import Cocoa
import FlutterMacOS

final class MacOSVideoRendererBridge: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static weak var activeInstance: MacOSVideoRendererBridge?

  private var methodChannel: FlutterMethodChannel?
  private var eventChannel: FlutterEventChannel?
  private let lifecycle: MacOSPlayerLifecycleController
  private let tracks = MacOSVideoTrackController()
  private let presentation = MacOSPresentationController()
  private let presentationState = MacOSFramePresentationState()
  private let nativeEvents = MacOSNativeEventState()
  private let playback = MacOSPlaybackController()
  private let transport = MacOSTransportController()

  init(textureRegistry: FlutterTextureRegistry) {
    self.lifecycle = MacOSPlayerLifecycleController(textureRegistry: textureRegistry)
    super.init()
  }

  private var texture: MacOSVideoTexture? {
    lifecycle.texture
  }

  private var nativeTexture: MacOSFlutterTextureBridge? {
    lifecycle.nativeTexture
  }

  private var textureId: Int64? {
    lifecycle.textureId
  }

  private var backendName: String {
    lifecycle.backendName
  }

  private var nativePlayer: MacOSNativePlayerSession? {
    lifecycle.nativePlayer
  }

  static func register(with engine: FlutterEngine) {
    let bridge = MacOSVideoRendererBridge(textureRegistry: engine)
    activeInstance = bridge
    let messenger = engine.binaryMessenger
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    channel.setMethodCallHandler(bridge.handle)
    bridge.methodChannel = channel

    let events = FlutterEventChannel(name: eventsChannelName, binaryMessenger: messenger)
    events.setStreamHandler(bridge)
    bridge.eventChannel = events
  }

  static func destroyActivePlayerForWindowClose() {
    activeInstance?.destroyPlayerForWindowClose()
  }

  private func configureNativeLogging(arguments: Any?) {
    guard let args = arguments as? [String: Any],
          let logsDir = args["logsDir"] as? String,
          !logsDir.isEmpty else {
      return
    }
    logsDir.withCString { logsDirPointer in
      VPMacOSInstallCrashHandler(logsDirPointer)
    }
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging":
      configureNativeLogging(arguments: call.arguments)
      result(nil)
    case "setViewportBackgroundColor":
      result(nil)
    case "setTrackOffset":
      transport.setTrackOffset(arguments: call.arguments, player: nativePlayer)
      result(nil)
    case "setLoopRange":
      transport.setLoopRange(arguments: call.arguments, player: nativePlayer)
      result(nil)
    case "setAudibleTrack":
      transport.setAudibleTrack(arguments: call.arguments, player: nativePlayer)
      result(nil)
    case "setSpeed":
      transport.setSpeed(arguments: call.arguments, player: nativePlayer)
      result(nil)
    case "createPlayer":
      result(createPlayer(arguments: call.arguments))
    case "destroyPlayer":
      destroyPlayer()
      result(nil)
    case "addTrack":
      result(addTrack(arguments: call.arguments))
    case "removeTrack":
      removeTrack(arguments: call.arguments)
      result(nil)
    case "resize":
      presentation.resize(arguments: call.arguments, context: presentationContext())
      result(nil)
    case "play":
      playback.play(
        player: nativePlayer,
        texture: nativeTexture,
        textureRegistered: textureId != nil,
        maxTrackSlots: tracks.activeSlotCapacity(),
        userData: Unmanaged.passUnretained(self).toOpaque(),
        presentationState: presentationState
      )
      result(nil)
    case "pause":
      playback.pause(player: nativePlayer)
      result(nil)
    case "seek":
      let targetPtsUs = MacOSFlutterArguments.intArg(call.arguments, "ptsUs") ?? 0
      let requestId = MacOSFlutterArguments.intArg(call.arguments, "requestId")
      let resumeAfterSeek = playback.currentIsPlaying(player: nativePlayer)
      if let error = transport.seekAndRefresh(
        targetPtsUs: targetPtsUs,
        requestId: requestId,
        resumeAfterSeek: resumeAfterSeek,
        context: transportContext()
      ) {
        result(error)
        return
      }
      result(nil)
    case "stepForward":
      if let error = transport.stepAndRefresh(forward: true, context: transportContext()) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      if let error = transport.stepAndRefresh(forward: false, context: transportContext()) {
        result(error)
        return
      }
      result(nil)
    case "currentPts":
      result(transport.currentPts(player: nativePlayer, presentationState: presentationState))
    case "duration":
      result(tracks.isEmpty ? 0 : tracks.currentDurationUs)
    case "currentPresentedFrame":
      result(
        textureId == nil
          ? nil
          : presentationState.currentPresentedFrameMap()
      )
    case "isPlaying":
      result(playback.currentIsPlaying(player: nativePlayer))
    case "getLayout":
      result(presentation.layout)
    case "applyLayout":
      presentation.applyLayout(arguments: call.arguments, context: presentationContext())
      result(nil)
    case "getTracks":
      result(tracks.tracks)
    case "pickFiles":
      MacOSFilePicker.pickFiles(arguments: call.arguments, result: result)
    case "getDiagnostics":
      result(MacOSVideoRendererDiagnostics.map(
        backendName: backendName,
        player: nativePlayer,
        textureId: textureId,
        textureStats: texture?.diagnostics(),
        textureDimensions: texture?.dimensions(),
        trackCount: tracks.count,
        presentationTargetInstalled: playback.targetInstalled,
        nativeEventDiagnostics: nativeEvents.diagnosticMap(),
        presentationDiagnostics: presentationState.diagnosticMap()
      ))
    case "captureViewport":
      result(MacOSViewportCapture.capture(texture: texture))
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func createPlayer(arguments: Any?) -> Any {
    lifecycle.create(
      arguments: arguments,
      playback: playback,
      tracks: tracks,
      presentationState: presentationState,
      markFrameAvailable: { [weak self] in
        self?.markFrameAvailable()
      }
    )
  }

  private func destroyPlayer() {
    lifecycle.destroy(playback: playback, tracks: tracks, presentationState: presentationState)
    presentation.resetLayout()
  }

  private func destroyPlayerForWindowClose() {
    if textureId == nil && nativePlayer == nil {
      return
    }
    NSLog("VoidPlayer macOS native player teardown before window close")
    destroyPlayer()
  }

  private func addTrack(arguments: Any?) -> Any {
    guard textureId != nil else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "createPlayer must be called before addTrack",
        details: nil
      )
    }

    let addResult = tracks.addTrack(
      arguments: arguments,
      backendName: backendName,
      nativePlayer: nativePlayer,
      textureDimensions: texture?.dimensions()
    )
    if addResult.refreshCurrentFrame {
      presentation.refreshCurrentFrame(context: presentationContext())
    }
    if addResult.markFrameAvailable {
      markFrameAvailable()
    }
    return addResult.payload
  }

  private func removeTrack(arguments: Any?) {
    let removeResult = tracks.removeTrack(
      arguments: arguments,
      backendName: backendName,
      nativePlayer: nativePlayer
    )
    if removeResult.destroyPlayer {
      destroyPlayer()
    } else if removeResult.refreshCurrentFrame {
      presentation.refreshCurrentFrame(context: presentationContext())
    }
  }

  private func markFrameAvailable() {
    lifecycle.markFrameAvailable()
  }

  private func activeDurationUs() -> Int {
    let durationUs = tracks.currentDurationUs
    return durationUs > 0 ? durationUs : MacOSVideoTrackPayload.syntheticDurationUs
  }

  private func emitSeekPreviewPresented(requestId: Int?, targetPtsUs: Int) {
    nativeEvents.emitSeekPreviewPresented(
      requestId: requestId,
      targetPtsUs: targetPtsUs,
      presentationState: presentationState
    )
  }

  private func presentationContext() -> MacOSPresentationContext {
    MacOSPresentationContext(
      nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
      player: nativePlayer,
      texture: texture,
      nativeTexture: nativeTexture,
      maxTrackSlots: tracks.activeSlotCapacity(),
      playback: playback,
      presentationState: presentationState,
      markFrameAvailable: { [weak self] in
        self?.markFrameAvailable()
      }
    )
  }

  private func transportContext() -> MacOSTransportContext {
    MacOSTransportContext(
      nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
      player: nativePlayer,
      texture: nativeTexture,
      textureRegistered: textureId != nil,
      playback: playback,
      presentationState: presentationState,
      activeDurationUs: activeDurationUs(),
      maxTrackSlots: tracks.activeSlotCapacity(),
      userData: Unmanaged.passUnretained(self).toOpaque(),
      markFrameAvailable: { [weak self] in
        self?.markFrameAvailable()
      },
      emitSeekPreviewPresented: { [weak self] requestId, targetPtsUs in
        self?.emitSeekPreviewPresented(requestId: requestId, targetPtsUs: targetPtsUs)
      }
    )
  }

  func scheduleNativeFrameCopyFromCallback() {
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.playback.handleFrameCallback(
        player: self.nativePlayer,
        texture: self.nativeTexture,
        maxTrackSlots: self.tracks.activeSlotCapacity(),
        nativeBackendActive: self.backendName == MacOSVideoTrackPayload.nativeFormatName,
        presentationState: self.presentationState,
        markFrameAvailable: self.markFrameAvailable
      )
    }
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    nativeEvents.onListen(events)
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    nativeEvents.onCancel()
    return nil
  }

}
