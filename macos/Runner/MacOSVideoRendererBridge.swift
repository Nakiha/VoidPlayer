import Cocoa
import FlutterMacOS

final class MacOSVideoRendererBridge: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static weak var activeInstance: MacOSVideoRendererBridge?

  private let textureRegistry: FlutterTextureRegistry
  private var methodChannel: FlutterMethodChannel?
  private var eventChannel: FlutterEventChannel?
  private var texture: MacOSFlutterTextureBridge?
  private var textureId: Int64?
  private let tracks = MacOSVideoTrackController()
  private var layout: [String: Any] = MacOSVideoTrackPayload.defaultLayout()
  private var backendName = "synthetic-texture"
  private var nativePlayer: MacOSNativePlayerSession?
  private let presentationState = MacOSFramePresentationState()
  private let nativeEvents = MacOSNativeEventState()
  private let playback = MacOSPlaybackController()
  private let transport = MacOSTransportController()

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
    super.init()
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
      resize(arguments: call.arguments)
      result(nil)
    case "play":
      playback.play(
        player: nativePlayer,
        texture: texture,
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
      result(layout)
    case "applyLayout":
      if let nextLayout = MacOSNativeLayoutBridge.apply(
        arguments: call.arguments,
        player: nativePlayer
      ) {
        layout = nextLayout
        refreshCurrentFrameAfterLayoutChange()
      }
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
    destroyPlayer()

    let startup: MacOSVideoRendererStartup
    do {
      startup = try MacOSVideoRendererStartupFactory.make(arguments: arguments)
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to open macOS native player",
        details: "\(error)"
      )
    }

    let registeredTextureId = textureRegistry.register(startup.texture)

    texture = startup.texture
    textureId = registeredTextureId
    backendName = startup.backendName
    nativePlayer = startup.nativePlayer
    playback.setTargetInstalled(startup.presentationTargetInstalled)
    tracks.replace(with: startup.tracks, fallbackDurationUs: startup.trackDurationUs)
    presentationState.seedPresentedFrame(
      ptsUs: startup.initialPresentedPtsUs,
      dtsUs: startup.initialPresentedDtsUs,
      durationUs: startup.trackDurationUs
    )
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": tracks.tracks,
    ]
  }

  private func destroyPlayer() {
    playback.stopFramePump(player: nativePlayer, clearPresentationTarget: true)
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    tracks.reset()
    presentationState.resetAll()
    backendName = "synthetic-texture"
    playback.reset()
    nativePlayer?.close()
    nativePlayer = nil
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
      refreshCurrentFrameAfterLayoutChange()
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
      refreshCurrentFrameAfterLayoutChange()
    }
  }

  private func resize(arguments: Any?) {
    let width = MacOSFlutterArguments.intArg(arguments, "width")
    let height = MacOSFlutterArguments.intArg(arguments, "height")
    if let width, let height {
      let nextWidth = max(16, width)
      let nextHeight = max(16, height)
      let currentDimensions = texture?.dimensions()
      let willChange = currentDimensions?.width != nextWidth ||
        currentDimensions?.height != nextHeight
      if backendName == MacOSVideoTrackPayload.nativeFormatName, willChange {
        nativePlayer?.clearMetalPresentationTarget()
      }
      _ = texture?.resize(width: nextWidth, height: nextHeight) ?? false
      if backendName == MacOSVideoTrackPayload.nativeFormatName {
        refreshCurrentFrameAfterLayoutChange()
        playback.reinstallPresentationTargetIfPlaying(
          player: nativePlayer,
          texture: texture,
          maxTrackSlots: tracks.activeSlotCapacity()
        )
      }
    }
    markFrameAvailable()
  }

  private func markFrameAvailable() {
    if let id = textureId {
      textureRegistry.textureFrameAvailable(id)
    }
  }

  private func activeDurationUs() -> Int {
    let durationUs = tracks.currentDurationUs
    return durationUs > 0 ? durationUs : MacOSVideoTrackPayload.syntheticDurationUs
  }

  private func refreshCurrentFrameAfterLayoutChange() {
    guard backendName == MacOSVideoTrackPayload.nativeFormatName,
          let nativePlayer,
          let texture else {
      markFrameAvailable()
      return
    }
    MacOSNativeFrameRefresh.refreshCurrentFrameAfterLayoutChange(
      player: nativePlayer,
      texture: texture,
      maxTrackSlots: tracks.activeSlotCapacity(),
      presentationState: presentationState,
      framePump: playback.framePumpForRefresh
    )
    markFrameAvailable()
  }

  private func emitSeekPreviewPresented(requestId: Int?, targetPtsUs: Int) {
    nativeEvents.emitSeekPreviewPresented(
      requestId: requestId,
      targetPtsUs: targetPtsUs,
      presentationState: presentationState
    )
  }

  private func transportContext() -> MacOSTransportContext {
    MacOSTransportContext(
      nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
      player: nativePlayer,
      texture: texture,
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
