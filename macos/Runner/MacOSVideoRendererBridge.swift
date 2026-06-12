import Cocoa
import FlutterMacOS
import Metal

final class MacOSVideoRendererBridge: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static weak var activeInstance: MacOSVideoRendererBridge?

  private var methodChannel: FlutterMethodChannel?
  private var eventChannel: FlutterEventChannel?
  private weak var flutterEngine: FlutterEngine?
  private weak var contentView: NSView?
  private var nativeCompositor: MacOSNativeCompositorView?
  private var lastNativeCompositorFailure = "not initialized"
  private let lifecycle: MacOSPlayerLifecycleController
  private let tracks = MacOSVideoTrackController()
  private let presentation = MacOSPresentationController()
  private let presentationState = MacOSFramePresentationState()
  private let nativeEvents = MacOSNativeEventState()
  private let playback = MacOSPlaybackController()
  private let transport = MacOSTransportController()
  private let frameCallbackProfiler = MacOSFrameCallbackProfiler()
  private let frameAvailableRate = MacOSRateWindow()
  private let coalescedFrameCallbackDelayMs = 8
  private var frameAvailableCount = 0
  private var profilerSummaryTimer: DispatchSourceTimer?

  init(engine: FlutterEngine, contentView: NSView) {
    self.flutterEngine = engine
    self.contentView = contentView
    self.lifecycle = MacOSPlayerLifecycleController(textureRegistry: engine)
    super.init()
    ensureNativeCompositorMatchesCurrentConfiguration()
  }

  deinit {
    profilerSummaryTimer?.cancel()
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

  static func register(with engine: FlutterEngine, contentView: NSView) {
    let bridge = MacOSVideoRendererBridge(engine: engine, contentView: contentView)
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
    let logFileName = args["logFileName"] as? String ?? "native_main.log"
    let level = args["logLevel"] as? String ?? "info"
    logsDir.withCString { logsDirPointer in
      logFileName.withCString { logFileNamePointer in
        level.withCString { levelPointer in
          VPMacOSConfigureLogging(logsDirPointer, logFileNamePointer, levelPointer)
        }
      }
    }
    logsDir.withCString { logsDirPointer in
      VPMacOSInstallCrashHandler(logsDirPointer)
    }
    startProfilerSummaryTimerIfNeeded()
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging":
      configureNativeLogging(arguments: call.arguments)
      result(nil)
    case "setViewportBackgroundColor":
      if let color = MacOSFlutterArguments.intArg(call.arguments, "color") {
        nativePlayer?.setBackgroundColor(color)
        if backendName == MacOSVideoTrackPayload.nativeFormatName {
          presentation.refreshCurrentFrame(context: presentationContext())
        }
      }
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
    case "setNativeCompositorViewportRect":
      setNativeCompositorViewportRect(arguments: call.arguments)
      result(nil)
    case "setNativeCompositorViewportTransform":
      setNativeCompositorViewportTransform(arguments: call.arguments)
      result(nil)
    case "prepareNativeCompositorSourceCache":
      prepareNativeCompositorSourceCache(arguments: call.arguments)
      result(nil)
    case "clearNativeCompositorSourceCache":
      clearNativeCompositorSourceCache(arguments: call.arguments)
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
      presentation.cancelPendingLayoutRefreshes()
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
      presentation.cancelPendingLayoutRefreshes()
      if let error = transport.stepAndRefresh(forward: true, context: transportContext()) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      presentation.cancelPendingLayoutRefreshes()
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
      result(currentPresentedFrame(arguments: call.arguments))
    case "isPlaying":
      result(playback.currentIsPlaying(player: nativePlayer))
    case "getPlaybackSnapshot":
      result(playbackSnapshot(arguments: call.arguments))
    case "getLayout":
      result(presentation.layout)
    case "applyLayout":
      presentation.applyLayout(arguments: call.arguments, context: presentationContext()) { outcome in
        MacOSProfilerLog.traceEvent(String(
          format: "VoidPlayer viewport trace swift event=apply-layout-result outcome=%@",
          outcome
        ))
        result(nil)
      }
    case "getTracks":
      result(tracks.tracks)
    case "pickFiles":
      MacOSFilePicker.pickFiles(arguments: call.arguments, result: result)
    case "activateSecurityScopedBookmarks":
      MacOSFilePicker.activateSecurityScopedBookmarks(arguments: call.arguments, result: result)
    case "getDiagnostics":
      var diagnostics = MacOSVideoRendererDiagnostics.map(
        backendName: backendName,
        player: nativePlayer,
        textureId: textureId,
        textureStats: texture?.diagnostics(),
        textureDimensions: texture?.dimensions(),
        trackCount: tracks.count,
        isPlaying: playback.currentIsPlaying(player: nativePlayer),
        presentationTargetInstalled: playback.targetInstalled,
        nativeEventDiagnostics: nativeEvents.diagnosticMap(),
        frameCallbackDiagnostics: frameCallbackDiagnostics(),
        viewportDiagnostics: presentation.diagnosticMap(),
        presentationDiagnostics: presentationState.diagnosticMap()
      )
      diagnostics.merge(MacOSPresentationConfiguration.current.diagnostics) { _, next in next }
      if let nativeCompositor {
        diagnostics.merge(nativeCompositor.diagnostics()) { _, next in next }
      }
      result(diagnostics)
    case "debugFlutterSurfaceInfo":
      result(debugFlutterSurfaceInfo())
    case "debugNativeCompositor", "debugNativeCompositorSpike":
      if let nativeCompositor {
        var diagnostics = nativeCompositor.diagnostics()
        if let state = nativePlayer?.rendererOwnedPresentationState() {
          diagnostics.merge(Self.rendererOwnedColorDiagnostics(from: state)) { _, next in next }
        }
        result(diagnostics)
      } else {
        var diagnostics = MacOSPresentationConfiguration.current.diagnostics
        diagnostics.merge([
          "nativeCompositorEnabled": false,
          "nativeCompositorSpikeEnabled": false,
          "nativeCompositorLastFailure": "native compositor presentation mode is not enabled",
        ]) { _, next in next }
        result(diagnostics)
      }
    case "captureViewport":
      result(MacOSViewportCapture.capture(texture: texture))
    case "captureViewportRegion":
      result(MacOSViewportCapture.captureRegion(texture: texture, arguments: call.arguments))
    case "captureWindow":
      result(MacOSViewportCapture.captureWindow(arguments: call.arguments))
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private static func rendererOwnedColorDiagnostics(from state: [String: Any]) -> [String: Any] {
    [
      "rendererOwnedLastFrameColorRangeCode": state["lastFrameColorRangeCode"] ?? 0,
      "rendererOwnedLastFrameColorRange": state["lastFrameColorRange"] ?? "unknown",
      "rendererOwnedLastFrameColorMatrixCode": state["lastFrameColorMatrixCode"] ?? 0,
      "rendererOwnedLastFrameColorMatrix": state["lastFrameColorMatrix"] ?? "unknown",
      "rendererOwnedLastFrameColorTransferCode": state["lastFrameColorTransferCode"] ?? 0,
      "rendererOwnedLastFrameColorTransfer": state["lastFrameColorTransfer"] ?? "unknown",
      "rendererOwnedLastFrameColorPrimariesCode": state["lastFrameColorPrimariesCode"] ?? 0,
      "rendererOwnedLastFrameColorPrimaries": state["lastFrameColorPrimaries"] ?? "unknown",
    ]
  }

  private func debugFlutterSurfaceInfo() -> Any {
    guard let engine = flutterEngine else {
      return FlutterError(code: "NO_ENGINE", message: "Flutter engine is unavailable", details: nil)
    }

    let infos = engine.voidPlayerHDRCurrentFlutterSurfaceInfos()
    guard let info = infos.first else {
      NSLog("VoidPlayer HDR compositor: no Flutter front surface is currently available")
      return FlutterError(code: "NO_FLUTTER_SURFACE", message: "No Flutter front surface", details: nil)
    }

    let texture = info["texture"] as? MTLTexture
    let ioSurface = info["ioSurface"]
    var payload = info
    payload.removeValue(forKey: "texture")
    payload.removeValue(forKey: "ioSurface")
    payload["nativeTextureObjectAvailable"] = texture != nil
    payload["nativeIOSurfaceObjectAvailable"] = ioSurface != nil

    NSLog(
      "VoidPlayer HDR compositor: stole Flutter texture available=%@ ioSurface=%@ pointer=%@ format=%@ size=%@x%@ wideGamut=%@",
      texture != nil ? "true" : "false",
      ioSurface != nil ? "true" : "false",
      String(describing: payload["texturePointer"] ?? "nil"),
      String(describing: payload["texturePixelFormat"] ?? "unknown"),
      String(describing: payload["textureWidth"] ?? "0"),
      String(describing: payload["textureHeight"] ?? "0"),
      String(describing: payload["wideGamut"] ?? "false")
    )
    return payload
  }

  private func setNativeCompositorViewportRect(arguments: Any?) {
    guard let nativeCompositor else { return }
    nativeCompositor.setViewportRect(
      left: MacOSFlutterArguments.intArg(arguments, "left") ?? 0,
      top: MacOSFlutterArguments.intArg(arguments, "top") ?? 0,
      width: MacOSFlutterArguments.intArg(arguments, "width") ?? 0,
      height: MacOSFlutterArguments.intArg(arguments, "height") ?? 0,
      surfaceWidth: MacOSFlutterArguments.intArg(arguments, "surfaceWidth") ?? 0,
      surfaceHeight: MacOSFlutterArguments.intArg(arguments, "surfaceHeight") ?? 0
    )
  }

  private func setNativeCompositorViewportTransform(arguments: Any?) {
    guard let nativeCompositor else { return }
    nativeCompositor.setViewportTransform(
      enabled: MacOSFlutterArguments.boolArg(arguments, "enabled") ?? false,
      scaleX: MacOSFlutterArguments.doubleArg(arguments, "scaleX") ?? 1.0,
      scaleY: MacOSFlutterArguments.doubleArg(arguments, "scaleY") ?? 1.0,
      translateX: MacOSFlutterArguments.doubleArg(arguments, "translateX") ?? 0.0,
      translateY: MacOSFlutterArguments.doubleArg(arguments, "translateY") ?? 0.0,
      mode: MacOSFlutterArguments.intArg(arguments, "mode") ?? 0,
      splitPos: MacOSFlutterArguments.doubleArg(arguments, "splitPos") ?? 0.5,
      activeTrackCount: MacOSFlutterArguments.intArg(arguments, "activeTrackCount") ?? 1
    )
  }

  private func prepareNativeCompositorSourceCache(arguments: Any?) {
    guard let nativeCompositor else { return }
    guard let player = nativePlayer else {
      nativeCompositor.setSourceCache(textures: [], order: [], error: "native player unavailable")
      return
    }
    let sourceSlots = MacOSFlutterArguments.intListArg(arguments, "sourceSlots")
    let sourceOrder = MacOSFlutterArguments.intListArg(arguments, "sourceOrder")
    let displayOffsetX = MacOSFlutterArguments.doubleListArg(arguments, "displayOffsetX")
    let displayOffsetY = MacOSFlutterArguments.doubleListArg(arguments, "displayOffsetY")
    let invDisplaySizeX = MacOSFlutterArguments.doubleListArg(arguments, "invDisplaySizeX")
    let invDisplaySizeY = MacOSFlutterArguments.doubleListArg(arguments, "invDisplaySizeY")
    let viewOffsetUvX = MacOSFlutterArguments.doubleListArg(arguments, "viewOffsetUvX")
    let viewOffsetUvY = MacOSFlutterArguments.doubleListArg(arguments, "viewOffsetUvY")
    let trackPayloads = tracks.tracks
    if trackPayloads.isEmpty || sourceSlots.isEmpty {
      nativeCompositor.setSourceCache(textures: [], order: sourceOrder, error: "no source tracks")
      return
    }

    let outputPixelFormat = MacOSPresentationConfiguration.current.edrOutputEnabled
      ? kCVPixelFormatType_64RGBAHalf
      : kCVPixelFormatType_32BGRA
    let maxBytes = 384 * 1024 * 1024
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    struct PendingSource {
      let texture: MacOSNativeCompositorSourceTexture
      var target: VPMacOSNativeSourceFrameBakeTarget
    }

    var pending: [PendingSource] = []
    var totalBytes = 0
    for payload in trackPayloads {
      guard let slot = payload["slot"] as? Int,
            sourceSlots.contains(slot),
            let fileId = payload["fileId"] as? Int,
            let width = payload["width"] as? Int,
            let height = payload["height"] as? Int,
            width > 0,
            height > 0 else {
        continue
      }
      let estimatedBytes = width * height * (outputPixelFormat == kCVPixelFormatType_64RGBAHalf ? 8 : 4)
      if totalBytes + estimatedBytes > maxBytes {
        nativeCompositor.setSourceCache(
          textures: [],
          order: sourceOrder,
          error: "source cache memory cap exceeded"
        )
        return
      }
      var pixelBuffer: CVPixelBuffer?
      let status = CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        outputPixelFormat,
        attributes,
        &pixelBuffer
      )
      guard status == kCVReturnSuccess, let pixelBuffer else {
        nativeCompositor.setSourceCache(
          textures: [],
          order: sourceOrder,
          error: "failed to allocate source cache pixel buffer"
        )
        return
      }
      totalBytes += CVPixelBufferGetBytesPerRow(pixelBuffer) *
        CVPixelBufferGetHeight(pixelBuffer)
      let projectionIndex = slot
      let texture = MacOSNativeCompositorSourceTexture(
        pixelBuffer: pixelBuffer,
        sourceSlot: slot,
        fileId: fileId,
        width: width,
        height: height,
        displayOffsetX: Float(doubleAt(displayOffsetX, projectionIndex)),
        displayOffsetY: Float(doubleAt(displayOffsetY, projectionIndex)),
        invDisplaySizeX: Float(doubleAt(invDisplaySizeX, projectionIndex)),
        invDisplaySizeY: Float(doubleAt(invDisplaySizeY, projectionIndex)),
        viewOffsetUvX: Float(doubleAt(viewOffsetUvX, projectionIndex)),
        viewOffsetUvY: Float(doubleAt(viewOffsetUvY, projectionIndex))
      )
      var target = VPMacOSNativeSourceFrameBakeTarget()
      target.pixel_buffer = UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque())
      target.source_slot = Int32(slot)
      target.source_file_id = Int32(fileId)
      target.width = Int32(width)
      target.height = Int32(height)
      target.drawn = 0
      VPMacOSNativeFrameInfoInit(&target.frame_info)
      pending.append(PendingSource(texture: texture, target: target))
    }

    guard !pending.isEmpty else {
      nativeCompositor.setSourceCache(textures: [], order: sourceOrder, error: "no source cache targets")
      return
    }
    let maxWidth = pending.map { $0.texture.width }.max() ?? 1
    let maxHeight = pending.map { $0.texture.height }.max() ?? 1
    let bakeTarget = MacOSNativeMetalPresentationTarget(width: maxWidth, height: maxHeight)
    var nativeTargets = pending.map { $0.target }
    let result = nativeTargets.withUnsafeMutableBufferPointer { buffer in
      bakeTarget.bakeCurrentFrameSources(player: player, targets: buffer)
    }
    if result.drawnCount <= 0 {
      nativeCompositor.setSourceCache(textures: [], order: sourceOrder, error: result.error)
      return
    }
    var bakedTextures: [MacOSNativeCompositorSourceTexture] = []
    for index in pending.indices {
      if nativeTargets[index].drawn != 0 {
        bakedTextures.append(pending[index].texture)
      }
    }
    nativeCompositor.setSourceCache(textures: bakedTextures, order: sourceOrder)
  }

  private func clearNativeCompositorSourceCache(arguments: Any?) {
    nativeCompositor?.clearSourceCache(
      reason: MacOSFlutterArguments.stringArg(arguments, "reason") ?? "clear requested"
    )
  }

  private func doubleAt(_ values: [Double], _ index: Int) -> Double {
    guard values.indices.contains(index) else { return 0.0 }
    return values[index]
  }

  private func currentPresentedFrame(arguments: Any?) -> Any? {
    guard textureId != nil else { return nil }
    let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") ?? -1
    if fileId >= 0, let player = nativePlayer {
      for track in player.trackDiagnostics() {
        if (track["fileId"] as? Int) == fileId {
          let frame: [String: Any] = [
            "ptsUs": track["currentPtsUs"] as? Int64 ?? -1,
            "dtsUs": track["currentDtsUs"] as? Int64 ?? Int64.min,
            "analysisFrameIndex": track["analysisFrameIndex"] as? Int ?? -1,
            "frameIdentityMode": track["frameIdentityMode"] as? Int ?? 0,
            "sourcePacketIndex": track["sourcePacketIndex"] as? Int ?? -1,
            "sourcePacketSize": track["sourcePacketSize"] as? Int ?? 0,
            "sourcePacketPos": track["sourcePacketPos"] as? Int64 ?? -1,
            "sourcePacketPtsUs": track["sourcePacketPtsUs"] as? Int64 ?? Int64.min,
            "sourcePacketDtsUs": track["sourcePacketDtsUs"] as? Int64 ?? Int64.min,
          ]
          return frame
        }
      }
    }
    return presentationState.currentPresentedFrameMap()
  }

  private func playbackSnapshot(arguments: Any?) -> Any {
    let includePresentedFrames =
      MacOSFlutterArguments.boolArg(arguments, "includePresentedFrames") ?? false
    var snapshot: [String: Any] = [
      "currentPtsUs": transport.currentPts(player: nativePlayer, presentationState: presentationState),
      "durationUs": tracks.isEmpty ? 0 : tracks.currentDurationUs,
      "isPlaying": playback.currentIsPlaying(player: nativePlayer),
    ]
    if includePresentedFrames {
      snapshot["presentedFrames"] = currentPresentedFrames()
    }
    return snapshot
  }

  private func currentPresentedFrames() -> [[String: Any]] {
    guard textureId != nil, let player = nativePlayer else { return [] }
    return player.trackDiagnostics().compactMap { track in
      guard let fileId = track["fileId"] as? Int else { return nil }
      return [
        "fileId": fileId,
        "ptsUs": track["currentPtsUs"] as? Int64 ?? -1,
        "dtsUs": track["currentDtsUs"] as? Int64 ?? Int64.min,
        "analysisFrameIndex": track["analysisFrameIndex"] as? Int ?? -1,
        "frameIdentityMode": track["frameIdentityMode"] as? Int ?? 0,
        "sourcePacketIndex": track["sourcePacketIndex"] as? Int ?? -1,
        "sourcePacketSize": track["sourcePacketSize"] as? Int ?? 0,
        "sourcePacketPos": track["sourcePacketPos"] as? Int64 ?? -1,
        "sourcePacketPtsUs": track["sourcePacketPtsUs"] as? Int64 ?? Int64.min,
        "sourcePacketDtsUs": track["sourcePacketDtsUs"] as? Int64 ?? Int64.min,
      ]
    }
  }

  private func createPlayer(arguments: Any?) -> Any {
    let result = lifecycle.create(
      arguments: arguments,
      playback: playback,
      tracks: tracks,
      presentationState: presentationState,
      markFrameAvailable: { [weak self] in
        self?.markFrameAvailable()
      }
    )
    ensureNativeCompositorMatchesCurrentConfiguration()
    nativeCompositor?.setVideoTexture(texture)
    return result
  }

  private func destroyPlayer() {
    presentation.resetLayout()
    lifecycle.destroy(playback: playback, tracks: tracks, presentationState: presentationState)
    MacOSPresentationConfiguration.resetForNoMedia()
    nativeCompositor?.setVideoTexture(nil)
    ensureNativeCompositorMatchesCurrentConfiguration()
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
    refreshPresentationPolicyForCurrentTracks()
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
      refreshPresentationPolicyForCurrentTracks()
      presentation.refreshCurrentFrame(context: presentationContext())
    }
  }

  private func markFrameAvailable() {
    frameAvailableCount += 1
    frameAvailableRate.record()
    lifecycle.markFrameAvailable()
    nativeCompositor?.setVideoTexture(texture)
  }

  private func ensureNativeCompositorMatchesCurrentConfiguration() {
    let configuration = MacOSPresentationConfiguration.current
    guard configuration.nativeCompositorEnabled,
          textureId != nil,
          texture != nil else {
      nativeCompositor?.detach()
      nativeCompositor = nil
      lastNativeCompositorFailure = configuration.nativeCompositorEnabled
        ? ""
        : "native compositor presentation mode is not enabled"
      emitNativeCompositorState()
      return
    }
    let currentMode = nativeCompositor?.diagnostics()["macOSPresentationMode"] as? String
    let currentReason =
      nativeCompositor?.diagnostics()["macOSPresentationReason"] as? String
    if currentMode == configuration.mode.rawValue &&
        currentReason == configuration.reason {
      emitNativeCompositorState()
      return
    }
    nativeCompositor?.detach()
    nativeCompositor = nil
    guard let engine = flutterEngine,
          let contentView else {
      lastNativeCompositorFailure = "Flutter engine or content view is unavailable"
      emitNativeCompositorState()
      return
    }
    guard let compositor = MacOSNativeCompositorView(engine: engine) else {
      lastNativeCompositorFailure = "native compositor initialization failed"
      emitNativeCompositorState()
      return
    }
    nativeCompositor = compositor
    compositor.attach(to: contentView)
    lastNativeCompositorFailure = ""
    nativeCompositor?.setVideoTexture(texture)
    emitNativeCompositorState()
  }

  private func refreshPresentationPolicyForCurrentTracks() {
    guard backendName == MacOSVideoTrackPayload.nativeFormatName else { return }
    let nextConfiguration = MacOSPresentationConfiguration.resolve(
      hasHDRTrack: tracks.hasHDRTrack
    )
    let previousConfiguration = MacOSPresentationConfiguration.current
    guard nextConfiguration.mode != previousConfiguration.mode ||
            nextConfiguration.reason != previousConfiguration.reason else {
      return
    }
    MacOSPresentationConfiguration.updateCurrent(nextConfiguration)
    NSLog(
      "VoidPlayer macOS presentation policy: request=%@ mode=%@ reason=%@ hdrTrack=%@",
      nextConfiguration.request,
      nextConfiguration.mode.rawValue,
      nextConfiguration.reason,
      tracks.hasHDRTrack ? "true" : "false"
    )
    if let nativeTexture {
      let pixelFormatChanged = nativeTexture.setRendererTargetPixelFormat(
        nextConfiguration.rendererTargetPixelFormat,
        player: nativePlayer
      )
      if pixelFormatChanged {
        playback.setTargetInstalled(false)
      }
    }
    ensureNativeCompositorMatchesCurrentConfiguration()
  }

  private func emitNativeCompositorState() {
    let configuration = MacOSPresentationConfiguration.current
    nativeEvents.emitNativeCompositorState(
      active: nativeCompositor != nil,
      requested: configuration.nativeCompositorEnabled,
      edrEnabled: configuration.edrOutputEnabled,
      mode: configuration.mode.rawValue,
      reason: configuration.reason,
      failure: nativeCompositor == nil ? lastNativeCompositorFailure : ""
    )
  }

  private func frameCallbackDiagnostics() -> [String: Any] {
    var diagnostics = frameCallbackProfiler.diagnosticMap()
    let frameAvailableHz = frameAvailableRate.rateHz()
    diagnostics["frameAvailableCount"] = frameAvailableCount
    diagnostics["frameAvailableHz"] = frameAvailableHz
    diagnostics["frameAvailableHzX1000"] = Int(frameAvailableHz * 1000.0)
    return diagnostics
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
      userData: Unmanaged.passUnretained(self).toOpaque(),
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

  func scheduleNativeFrameCopyFromCallback(
    callbackGeneration: UInt64,
    callbackContext: MacOSNativeFrameCallbackContext
  ) {
    let cachedPlaying = playback.isPlaying
    let enqueueNs = DispatchTime.now().uptimeNanoseconds
    guard frameCallbackProfiler.tryEnqueue(enqueueNs: enqueueNs) else {
      return
    }
    let schedule = { [weak self, weak callbackContext] in
      guard let self else { return }
      guard callbackContext?.isCurrent(callbackGeneration) == true else {
        _ = self.frameCallbackProfiler.finishProcessing(
          endNs: DispatchTime.now().uptimeNanoseconds
        )
        return
      }
      self.processNativeFrameCallback(
        enqueueNs: enqueueNs,
        callbackGeneration: callbackGeneration,
        callbackContext: callbackContext
      )
    }
    if cachedPlaying {
      DispatchQueue.main.async {
        schedule()
      }
    } else {
      DispatchQueue.main.asyncAfter(
        deadline: .now() + .milliseconds(coalescedFrameCallbackDelayMs)
      ) {
        schedule()
      }
    }
  }

  private func processNativeFrameCallback(
    enqueueNs: UInt64,
    callbackGeneration: UInt64,
    callbackContext: MacOSNativeFrameCallbackContext?
  ) {
    guard callbackContext?.isCurrent(callbackGeneration) == true else {
      _ = frameCallbackProfiler.finishProcessing(
        endNs: DispatchTime.now().uptimeNanoseconds
      )
      return
    }
    let startNs = DispatchTime.now().uptimeNanoseconds
    let nativePlaying = playback.syncPlayingState(player: nativePlayer)
    frameCallbackProfiler.recordMainStart(enqueueNs: enqueueNs, startNs: startNs)
    let suppressLayoutPublication =
      backendName == MacOSVideoTrackPayload.nativeFormatName &&
      presentation.shouldSuppressNativeCallbackPublicationDuringLayout()
    if suppressLayoutPublication {
      presentation.recordLayoutCallbackPublicationSuppressed()
    } else {
      playback.handleFrameCallback(
        player: nativePlayer,
        texture: nativeTexture,
        maxTrackSlots: tracks.activeSlotCapacity(),
        nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
        presentationState: presentationState,
        markFrameAvailable: markFrameAvailable
      )
    }
    let endNs = DispatchTime.now().uptimeNanoseconds
    logFrameCallbackProfiler(
      enqueueNs: enqueueNs,
      startNs: startNs,
      endNs: endNs,
      suppressed: suppressLayoutPublication
    )
    if let nextEnqueueNs = frameCallbackProfiler.finishProcessing(endNs: endNs) {
      DispatchQueue.main.asyncAfter(
        deadline: .now() + .milliseconds(coalescedFrameCallbackDelayMs)
      ) { [weak self, weak callbackContext] in
        guard let self else { return }
        guard callbackContext?.isCurrent(callbackGeneration) == true else {
          return
        }
        if !nativePlaying {
          _ = self.playback.syncPlayingState(player: self.nativePlayer)
        }
        self.processNativeFrameCallback(
          enqueueNs: nextEnqueueNs,
          callbackGeneration: callbackGeneration,
          callbackContext: callbackContext
        )
      }
    }
  }

  private func logFrameCallbackProfiler(
    enqueueNs: UInt64,
    startNs: UInt64,
    endNs: UInt64,
    suppressed: Bool
  ) {
    let waitNs = startNs >= enqueueNs ? startNs - enqueueNs : 0
    let handleNs = endNs >= startNs ? endNs - startNs : 0
    MacOSProfilerLog.trace(String(
      format: "VoidPlayer viewport trace swift event=frame-callback waitMs=%.2f handleMs=%.2f playing=%d tracks=%d native=%d suppressed=%d",
      Double(waitNs) / 1_000_000.0,
      Double(handleNs) / 1_000_000.0,
      playback.isPlaying ? 1 : 0,
      tracks.activeSlotCapacity(),
      backendName == MacOSVideoTrackPayload.nativeFormatName ? 1 : 0,
      suppressed ? 1 : 0
    ))
    let slow = waitNs >= 12_000_000 || handleNs >= 8_000_000
    guard slow else { return }
    MacOSProfilerLog.log(String(
      format: "VoidPlayer macOS frame callback profiler waitMs=%.2f handleMs=%.2f playing=%d tracks=%d native=%d suppressed=%d",
      Double(waitNs) / 1_000_000.0,
      Double(handleNs) / 1_000_000.0,
      playback.isPlaying ? 1 : 0,
      tracks.activeSlotCapacity(),
      backendName == MacOSVideoTrackPayload.nativeFormatName ? 1 : 0,
      suppressed ? 1 : 0
    ))
  }

  private func startProfilerSummaryTimerIfNeeded() {
    guard MacOSProfilerLog.enabled, profilerSummaryTimer == nil else {
      return
    }
    let timer = DispatchSource.makeTimerSource(queue: .main)
    timer.schedule(deadline: .now() + .seconds(1), repeating: .seconds(1))
    timer.setEventHandler { [weak self] in
      self?.logProfilerSummary()
    }
    profilerSummaryTimer = timer
    timer.resume()
  }

  private func logProfilerSummary() {
    MacOSRendererProfilerSummary.log(
      isPlaying: playback.isPlaying,
      trackCount: tracks.count,
      viewport: presentation.diagnosticMap(),
      callbacks: frameCallbackDiagnostics(),
      presentationFrames: presentationState.diagnosticMap(),
      perf: nativePlayer?.performanceStats() ?? [:],
      scheduler: nativePlayer?.presentationSchedulerStats() ?? [:]
    )
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    nativeEvents.onListen(events)
    emitNativeCompositorState()
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    nativeEvents.onCancel()
    return nil
  }

}
