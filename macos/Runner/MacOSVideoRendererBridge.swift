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
  private let trackStore = MacOSVideoTrackStore()
  private var layout: [String: Any] = MacOSVideoTrackPayload.defaultLayout()
  private var backendName = "synthetic-texture"
  private var nativePlayer: MacOSNativePlayerSession?
  private let presentationState = MacOSFramePresentationState()
  private let nativeEvents = MacOSNativeEventState()
  private let playback = MacOSPlaybackController()

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
      let fileId = MacOSFlutterArguments.intArg(call.arguments, "fileId") ?? -1
      let offsetUs = MacOSFlutterArguments.intArg(call.arguments, "offsetUs") ?? 0
      nativePlayer?.setTrackOffset(fileId: fileId, offsetUs: offsetUs)
      result(nil)
    case "setLoopRange":
      nativePlayer?.setLoopRange(
        enabled: MacOSFlutterArguments.boolArg(call.arguments, "enabled") ?? false,
        startUs: MacOSFlutterArguments.intArg(call.arguments, "startUs") ?? 0,
        endUs: MacOSFlutterArguments.intArg(call.arguments, "endUs") ?? 0
      )
      result(nil)
    case "setAudibleTrack":
      let fileId = MacOSFlutterArguments.intArg(call.arguments, "fileId") ?? -1
      nativePlayer?.setAudibleTrack(fileId)
      result(nil)
    case "setSpeed":
      let speed = max(0.01, MacOSFlutterArguments.doubleArg(call.arguments, "speed") ?? 1.0)
      nativePlayer?.setSpeed(speed)
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
        maxTrackSlots: activeTrackSlotCapacity(),
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
      if let error = seekAndRefresh(
        targetPtsUs: targetPtsUs,
        requestId: requestId,
        resumeAfterSeek: resumeAfterSeek
      ) {
        result(error)
        return
      }
      result(nil)
    case "stepForward":
      if let error = stepAndRefresh(forward: true) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      if let error = stepAndRefresh(forward: false) {
        result(error)
        return
      }
      result(nil)
    case "currentPts":
      presentationState.setCurrentPts(nativePlayer?.currentPtsUs() ?? presentationState.currentPtsUs)
      result(presentationState.currentPtsUs)
    case "duration":
      result(trackStore.isEmpty ? 0 : trackStore.currentDurationUs)
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
      result(trackStore.tracks)
    case "pickFiles":
      MacOSFilePicker.pickFiles(arguments: call.arguments, result: result)
    case "getDiagnostics":
      result(MacOSVideoRendererDiagnostics.map(
        backendName: backendName,
        player: nativePlayer,
        textureId: textureId,
        textureStats: texture?.diagnostics(),
        textureDimensions: texture?.dimensions(),
        trackCount: trackStore.count,
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
    trackStore.replace(with: startup.tracks, fallbackDurationUs: startup.trackDurationUs)
    presentationState.seedPresentedFrame(
      ptsUs: startup.initialPresentedPtsUs,
      dtsUs: startup.initialPresentedDtsUs,
      durationUs: startup.trackDurationUs
    )
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": trackStore.tracks,
    ]
  }

  private func nativeTrackMap(path: String, metadata: MacOSNativeTrackMetadata) -> [String: Any] {
    MacOSVideoTrackPayload.nativeTrack(
      path: path,
      metadata: metadata,
      decoderName: nativePlayer?.decoderName() ?? "decode_thread_software"
    )
  }

  private func destroyPlayer() {
    playback.stopFramePump(player: nativePlayer, clearPresentationTarget: true)
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    trackStore.reset()
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

    let fileId = trackStore.nextFileId()
    let slot = trackStore.count
    let path = MacOSFlutterArguments.stringArg(arguments, "path") ?? "macos-synthetic-\(fileId)"
    if backendName == MacOSVideoTrackPayload.nativeFormatName {
      do {
        guard let session = nativePlayer else {
          throw MacOSNativePlayerError.failed("macOS native player is unavailable")
        }
        let metadata = try session.addTrack(path: path, fileId: fileId)
        let track = nativeTrackMap(path: path, metadata: metadata)
        trackStore.append(track)
        refreshCurrentFrameAfterLayoutChange()
        return track
      } catch {
        return FlutterError(
          code: "DECODE_FAILED",
          message: "Failed to add macOS native track",
          details: "\(error)"
        )
      }
    }

    let size = texture?.dimensions() ?? (width: 1920, height: 1080)
    let track = MacOSVideoTrackPayload.syntheticTrack(
      fileId: fileId,
      slot: slot,
      path: path,
      width: size.width,
      height: size.height,
      durationUs: trackStore.currentDurationUs
    )
    trackStore.append(track)
    markFrameAvailable()
    return track
  }

  private func removeTrack(arguments: Any?) {
    guard let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") else { return }
    if backendName == MacOSVideoTrackPayload.nativeFormatName {
      if fileId == 0 {
        destroyPlayer()
        return
      }
      nativePlayer?.removeTrack(fileId: fileId)
    }
    trackStore.remove(
      fileId: fileId,
      compactSlots: backendName != MacOSVideoTrackPayload.nativeFormatName
    )
    if trackStore.isEmpty {
      destroyPlayer()
    } else {
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
          maxTrackSlots: activeTrackSlotCapacity()
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
    let durationUs = trackStore.currentDurationUs
    return durationUs > 0 ? durationUs : MacOSVideoTrackPayload.syntheticDurationUs
  }

  private func activeTrackSlotCapacity() -> Int {
    trackStore.activeSlotCapacity()
  }

  private func refreshDecodedFrameIfNeeded(targetPtsUs: Int) -> FlutterError? {
    guard backendName == MacOSVideoTrackPayload.nativeFormatName,
          let nativePlayer,
          let texture else {
      return nil
    }

    return MacOSNativeFrameRefresh.seekAndRefresh(
      player: nativePlayer,
      texture: texture,
      targetPtsUs: targetPtsUs,
      maxTrackSlots: activeTrackSlotCapacity(),
      presentationState: presentationState,
      framePump: playback.framePumpForRefresh
    )
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
      maxTrackSlots: activeTrackSlotCapacity(),
      presentationState: presentationState,
      framePump: playback.framePumpForRefresh
    )
    markFrameAvailable()
  }

  private func seekAndRefresh(
    targetPtsUs: Int,
    requestId: Int?,
    resumeAfterSeek: Bool
  ) -> FlutterError? {
    playback.stopForBlockingCommand(player: nativePlayer, pausePlayer: true)
    let settledPtsUs = max(0, min(activeDurationUs(), targetPtsUs))
    presentationState.setCurrentPts(settledPtsUs)
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: settledPtsUs) {
      return error
    }
    markFrameAvailable()
    emitSeekPreviewPresented(requestId: requestId, targetPtsUs: settledPtsUs)
    if resumeAfterSeek {
      playback.resumeIfNeeded(
        true,
        player: nativePlayer,
        texture: texture,
        textureRegistered: textureId != nil,
        maxTrackSlots: activeTrackSlotCapacity(),
        userData: Unmanaged.passUnretained(self).toOpaque(),
        presentationState: presentationState
      )
    }
    return nil
  }

  private func stepAndRefresh(forward: Bool) -> FlutterError? {
    playback.stopForBlockingCommand(player: nativePlayer, pausePlayer: false)
    guard let nativePlayer,
          let texture else {
      return nil
    }
    if let error = MacOSNativeFrameRefresh.stepAndRefresh(
      player: nativePlayer,
      texture: texture,
      forward: forward,
      maxTrackSlots: activeTrackSlotCapacity(),
      presentationState: presentationState,
      framePump: playback.framePumpForRefresh
    ) {
      return error
    }
    markFrameAvailable()
    return nil
  }

  private func emitSeekPreviewPresented(requestId: Int?, targetPtsUs: Int) {
    nativeEvents.emitSeekPreviewPresented(
      requestId: requestId,
      targetPtsUs: targetPtsUs,
      presentationState: presentationState
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
