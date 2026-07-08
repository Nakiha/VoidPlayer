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
  private let sourceProvider = MacOSNativeCompositorSourceProvider()
  private var viewportBackgroundColor: UInt32?
  private var lastNativeCompositorFailure = "not initialized"
  private let lifecycle: MacOSPlayerLifecycleController
  private let tracks = MacOSVideoTrackController()
  private let presentation = MacOSPresentationController()
  private let presentationState = MacOSFramePresentationState()
  private let nativeEvents = MacOSNativeEventState()
  private let playback = MacOSPlaybackController()
  private let transport = MacOSTransportController()
  private let frameCallbackProfiler = MacOSFrameCallbackProfiler()
  private let compositorLatencyProfiler = MacOSCompositorLatencyProfiler()
  private let frameAvailableRate = MacOSRateWindow()
  private let sourceProjectionMethodReceiveRate = MacOSRateWindow()
  private let coalescedFrameCallbackDelayMs = 8
  private var frameAvailableCount = 0
  private var sourceProjectionMethodReceiveCount = 0
  private var inlineDirtyFrameCallbackDrainCount = 0
  private var flutterTextureFrameAvailableSkippedWhilePlayingCount = 0
  private var compositorVideoTextureRefreshCount = 0
  private var compositorVideoTextureRefreshSkippedWhilePlayingCount = 0
  private var playbackSpeed = 1.0
  private var profilerSummaryTimer: DispatchSourceTimer?
  private var screenChangeObserver: NSObjectProtocol?

  init(engine: FlutterEngine, contentView: NSView) {
    self.flutterEngine = engine
    self.contentView = contentView
    self.lifecycle = MacOSPlayerLifecycleController(textureRegistry: engine)
    super.init()
    installScreenChangeObserver()
    ensureNativeCompositorMatchesCurrentConfiguration()
  }

  deinit {
    profilerSummaryTimer?.cancel()
    if let observer = screenChangeObserver {
      NotificationCenter.default.removeObserver(observer)
    }
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

  private func isNativeBackendActive() -> Bool {
    backendName == MacOSVideoTrackPayload.nativeFormatName
  }

  private var nativePlayer: MacOSNativePlayerSession? {
    lifecycle.nativePlayer
  }

  private var presentationScreen: NSScreen? {
    contentView?.window?.screen ?? NSScreen.main
  }

  private func installScreenChangeObserver() {
    screenChangeObserver = NotificationCenter.default.addObserver(
      forName: NSWindow.didChangeScreenNotification,
      object: nil,
      queue: .main
    ) { [weak self] notification in
      guard let self,
            let window = self.contentView?.window,
            notification.object as? NSWindow === window else {
        return
      }
      self.refreshPresentationPolicyForCurrentTracks()
    }
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
      if let color = MacOSFlutterArguments.uint32Arg(call.arguments, "color") {
        viewportBackgroundColor = color
        nativePlayer?.setBackgroundColor(color)
        nativeCompositor?.setViewportBackgroundColor(color)
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
      let requestedSpeed = MacOSFlutterArguments.doubleArg(call.arguments, "speed") ?? playbackSpeed
      playbackSpeed = max(0.01, requestedSpeed)
      transport.setSpeed(arguments: call.arguments, player: nativePlayer)
      emitPlaybackClock(force: true)
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
    case "prewarmNativePresentationTargetSize":
      prewarmNativePresentationTargetSize(arguments: call.arguments)
      result(nil)
    case "setNativeCompositorViewportRect":
      setNativeCompositorViewportRect(arguments: call.arguments)
      result(nil)
    case "requestNativeCompositorFlutterFrame":
      result(nil)
    case "ackNativeCompositorFlutterState":
      result(nil)
    case "setNativeCompositorViewportTransform":
      setNativeCompositorViewportTransform(arguments: call.arguments)
      result(nil)
    case "prepareNativeCompositorSourceCache":
      prepareNativeCompositorSourceCache(arguments: call.arguments)
      result(nil)
    case "setNativeAnalysisOverlay":
      setNativeAnalysisOverlay(arguments: call.arguments)
      result(nil)
    case "clearNativeCompositorSourceCache":
      clearNativeCompositorSourceCache(arguments: call.arguments)
      result(nil)
    case "play":
      let useNativeCompositorSourceProvider = nativeCompositorSourceProviderReady()
      setNativeCompositorSourceProviderActive(useNativeCompositorSourceProvider)
      playback.play(
        player: nativePlayer,
        texture: useNativeCompositorSourceProvider ? nil : nativeTexture,
        textureRegistered: textureId != nil,
        maxTrackSlots: tracks.activeSlotCapacity(),
        userData: Unmanaged.passUnretained(self).toOpaque(),
        presentationState: presentationState,
        requiresPresentationTarget: !useNativeCompositorSourceProvider
      )
      primeNativeCompositorPlaybackSource(reason: "play")
      emitPlaybackClock(force: true)
      result(nil)
    case "pause":
      playback.pause(player: nativePlayer)
      setNativeCompositorSourceProviderActive(nativeCompositorSourceProviderReady())
      emitPlaybackClock(force: true)
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
      if resumeAfterSeek && nativeCompositorSourceProviderReady() {
        setNativeCompositorSourceProviderActive(true)
        primeNativeCompositorPlaybackSource(reason: "seek resume")
      }
      emitPlaybackClock(force: true)
      result(nil)
    case "stepForward":
      presentation.cancelPendingLayoutRefreshes()
      if let error = transport.stepAndRefresh(forward: true, context: transportContext()) {
        result(error)
        return
      }
      emitPlaybackClock(force: true)
      result(nil)
    case "stepBackward":
      presentation.cancelPendingLayoutRefreshes()
      if let error = transport.stepAndRefresh(forward: false, context: transportContext()) {
        result(error)
        return
      }
      emitPlaybackClock(force: true)
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
    case "resetNativePerfCounters":
      nativePlayer?.resetRendererOwnedPresentationStats()
      presentationState.resetFrameCounters()
      result(nil)
    case "pickFiles":
      MacOSFilePicker.pickFiles(
        arguments: call.arguments,
        parentWindow: contentView?.window,
        result: result
      )
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
        nativeCompositorSourceProviderActive: nativeCompositorSourceProviderReady(),
        nativeEventDiagnostics: nativeEvents.diagnosticMap(),
        frameCallbackDiagnostics: frameCallbackDiagnostics(),
        viewportDiagnostics: presentation.diagnosticMap(),
        presentationDiagnostics: presentationState.diagnosticMap(),
        trackPayloads: tracks.tracks
      )
      diagnostics.merge(MacOSPresentationConfiguration.current.diagnostics) { _, next in next }
      if let nativeCompositor {
        diagnostics.merge(nativeCompositor.diagnostics()) { _, next in next }
      }
      diagnostics.merge(sourceProvider.diagnostics(nativeBackendActive: isNativeBackendActive())) {
        _, next in next
      }
      diagnostics["nativeCompositorSourceProjectionMethodReceiveCount"] =
        sourceProjectionMethodReceiveCount
      diagnostics["nativeCompositorSourceProjectionMethodReceiveHz"] =
        sourceProjectionMethodReceiveRate.rateHz()
      diagnostics["nativeCompositorSourceProjectionMethodReceiveHzX1000"] =
        Int(sourceProjectionMethodReceiveRate.rateHz() * 1000.0)
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
    let trace = compositorLatencyProfiler.receive(
      route: "viewport-rect",
      arguments: arguments
    )
    nativePlayer?.noteViewportCompositorActivity()
    nativeCompositor.setViewportRect(
      left: MacOSFlutterArguments.intArg(arguments, "left") ?? 0,
      top: MacOSFlutterArguments.intArg(arguments, "top") ?? 0,
      width: MacOSFlutterArguments.intArg(arguments, "width") ?? 0,
      height: MacOSFlutterArguments.intArg(arguments, "height") ?? 0,
      surfaceWidth: MacOSFlutterArguments.intArg(arguments, "surfaceWidth") ?? 0,
      surfaceHeight: MacOSFlutterArguments.intArg(arguments, "surfaceHeight") ?? 0,
      trace: trace
    )
  }

  private func prewarmNativePresentationTargetSize(arguments: Any?) {
    guard let texture else { return }
    texture.prewarmRendererTarget(
      width: MacOSFlutterArguments.intArg(arguments, "width") ?? 0,
      height: MacOSFlutterArguments.intArg(arguments, "height") ?? 0
    )
  }

  private func setNativeCompositorViewportTransform(arguments: Any?) {
    _ = compositorLatencyProfiler.receive(route: "viewport-transform-noop", arguments: arguments)
    // Kept as a no-op compatibility endpoint while Dart/native converge on
    // full-layout source projection.
  }

  private func prepareNativeCompositorSourceCache(arguments: Any?) {
    guard let nativeCompositor else { return }
    sourceProjectionMethodReceiveCount += 1
    sourceProjectionMethodReceiveRate.record()
    let trace = compositorLatencyProfiler.receive(
      route: "source-projection",
      arguments: arguments
    )
    let sourceOrder = MacOSFlutterArguments.intListArg(arguments, "sourceOrder")
    guard let player = nativePlayer else {
      clearNativeCompositorSourceProvider(reason: "native player unavailable")
      nativeCompositor.setSourceBuffers(
        textures: [],
        overlay: .empty,
        error: "native player unavailable"
      )
      return
    }
    let sourceSlots = MacOSFlutterArguments.intListArg(arguments, "sourceSlots")
    let displayOffsetX = MacOSFlutterArguments.doubleListArg(arguments, "displayOffsetX")
    let displayOffsetY = MacOSFlutterArguments.doubleListArg(arguments, "displayOffsetY")
    let invDisplaySizeX = MacOSFlutterArguments.doubleListArg(arguments, "invDisplaySizeX")
    let invDisplaySizeY = MacOSFlutterArguments.doubleListArg(arguments, "invDisplaySizeY")
    let viewOffsetUvX = MacOSFlutterArguments.doubleListArg(arguments, "viewOffsetUvX")
    let viewOffsetUvY = MacOSFlutterArguments.doubleListArg(arguments, "viewOffsetUvY")
    let projection = MacOSNativeCompositorSourceProjection(
      mode: MacOSFlutterArguments.intArg(arguments, "mode") ?? 0,
      splitPos: MacOSFlutterArguments.doubleArg(arguments, "splitPos") ?? 0.5,
      activeTrackCount: MacOSFlutterArguments.intArg(arguments, "activeTrackCount") ?? 1,
      order: sourceOrder,
      displayOffsetX: displayOffsetX,
      displayOffsetY: displayOffsetY,
      invDisplaySizeX: invDisplaySizeX,
      invDisplaySizeY: invDisplaySizeY,
      viewOffsetUvX: viewOffsetUvX,
      viewOffsetUvY: viewOffsetUvY,
      trace: trace
    )
    player.noteViewportCompositorActivity()
    if tracks.tracks.isEmpty || sourceSlots.isEmpty {
      clearNativeCompositorSourceProvider(reason: "no source tracks")
      nativeCompositor.setSourceBuffers(
        textures: [],
        overlay: .empty,
        error: "no source tracks"
      )
      return
    }

    let descriptors = nativeCompositorSourceDescriptors(
      sourceSlots: sourceSlots,
      displayOffsetX: displayOffsetX,
      displayOffsetY: displayOffsetY,
      invDisplaySizeX: invDisplaySizeX,
      invDisplaySizeY: invDisplaySizeY,
      viewOffsetUvX: viewOffsetUvX,
      viewOffsetUvY: viewOffsetUvY
    )
    if descriptors.isEmpty {
      clearNativeCompositorSourceProvider(reason: "no matching source tracks")
      nativeCompositor.setSourceBuffers(
        textures: [],
        overlay: .empty,
        error: "no matching source tracks"
      )
      return
    }
    let subscribed = sourceProvider.subscribe(
      player: player,
      descriptors: descriptors,
      order: sourceOrder,
      projection: projection,
      isPlaying: playback.currentIsPlaying(player: player),
      edrOutputEnabled: MacOSPresentationConfiguration.current.edrOutputEnabled,
      presentationState: presentationState,
      reason: "source projection"
    )
    if subscribed {
      setNativeCompositorSourceProviderActive(true)
    }
    if playback.currentIsPlaying(player: player),
       !playback.sourceProviderFramePumpActive {
      primeNativeCompositorPlaybackSource(reason: "source cache subscribed")
    }
  }

  private func setNativeAnalysisOverlay(arguments: Any?) {
    VPMacOSNativeAnalysisOverlayClearTracks()
    var trackCount = 0
    var loadedTrackCount = 0
    if let tracks = (arguments as? [String: Any])?["tracks"] as? [Any] {
      for entry in tracks {
        guard let map = entry as? [String: Any],
              let fileId = MacOSFlutterArguments.intValue(map["fileId"]),
              let analysisPath = map["analysisPath"] as? String,
              !analysisPath.isEmpty else {
          continue
        }
        analysisPath.withCString { pathPointer in
          if VPMacOSNativeAnalysisOverlaySetTrack(Int32(fileId), pathPointer) != 0 {
            loadedTrackCount += 1
          }
        }
        trackCount += 1
      }
    } else if let tracks = (arguments as? [AnyHashable: Any])?["tracks"] as? [Any] {
      for entry in tracks {
        guard let map = entry as? [AnyHashable: Any],
              let fileId = MacOSFlutterArguments.intValue(map["fileId"]),
              let analysisPath = map["analysisPath"] as? String,
              !analysisPath.isEmpty else {
          continue
        }
        analysisPath.withCString { pathPointer in
          if VPMacOSNativeAnalysisOverlaySetTrack(Int32(fileId), pathPointer) != 0 {
            loadedTrackCount += 1
          }
        }
        trackCount += 1
      }
    }
    let showCuGrid = MacOSFlutterArguments.boolArg(arguments, "showCuGrid") == true
    let showQpHeatmap = MacOSFlutterArguments.boolArg(arguments, "showQpHeatmap") == true
    let showBitCost = MacOSFlutterArguments.boolArg(arguments, "showCuBitCostHeatmap") == true
    VPMacOSNativeAnalysisOverlaySetState(
      showCuGrid ? 1 : 0,
      MacOSFlutterArguments.boolArg(arguments, "showPredMode") == true ? 1 : 0,
      showQpHeatmap ? 1 : 0,
      MacOSFlutterArguments.boolArg(arguments, "showPredLines") == true ? 1 : 0,
      showBitCost ? 1 : 0,
      Int32(MacOSFlutterArguments.intArg(arguments, "opacityPermille") ?? 550),
      Int32(MacOSFlutterArguments.intArg(arguments, "mode") ?? 0),
      Int32(MacOSFlutterArguments.intArg(arguments, "trackFileId") ?? -1)
    )
    if MacOSProfilerLog.enabled {
      String(
        format:
          "NativeAnalysisOverlaySync tracks=%d loaded=%d cu=%@ qp=%@ bitCost=%@ mode=%d opacity=%d",
        trackCount,
        loadedTrackCount,
        showCuGrid ? "true" : "false",
        showQpHeatmap ? "true" : "false",
        showBitCost ? "true" : "false",
        MacOSFlutterArguments.intArg(arguments, "mode") ?? 0,
        MacOSFlutterArguments.intArg(arguments, "opacityPermille") ?? 550
      ).withCString { pointer in
        VPMacOSLogProfilerSummary(pointer)
      }
    }
    if let player = nativePlayer {
      nativeCompositor?.setOverlayPrimitives(player.currentOverlayPrimitives())
    }
  }

  private func clearNativeCompositorSourceCache(arguments: Any?) {
    _ = compositorLatencyProfiler.receive(route: "source-clear", arguments: arguments)
    clearNativeCompositorSourceProvider(
      reason: MacOSFlutterArguments.stringArg(arguments, "reason") ?? "clear requested"
    )
  }

  private func nativeCompositorSourceDescriptors(
    sourceSlots: [Int],
    displayOffsetX: [Double],
    displayOffsetY: [Double],
    invDisplaySizeX: [Double],
    invDisplaySizeY: [Double],
    viewOffsetUvX: [Double],
    viewOffsetUvY: [Double]
  ) -> [MacOSCompositorSourceTrackDescriptor] {
    tracks.tracks.compactMap { payload in
      guard let slot = payload["slot"] as? Int,
            sourceSlots.contains(slot),
            let fileId = payload["fileId"] as? Int,
            let width = payload["width"] as? Int,
            let height = payload["height"] as? Int,
            width > 0,
            height > 0 else {
        return nil
      }
      return MacOSCompositorSourceTrackDescriptor(
        slot: slot,
        fileId: fileId,
        width: width,
        height: height,
        displayOffsetX: Float(doubleAt(displayOffsetX, slot)),
        displayOffsetY: Float(doubleAt(displayOffsetY, slot)),
        invDisplaySizeX: Float(doubleAt(invDisplaySizeX, slot)),
        invDisplaySizeY: Float(doubleAt(invDisplaySizeY, slot)),
        viewOffsetUvX: Float(doubleAt(viewOffsetUvX, slot)),
        viewOffsetUvY: Float(doubleAt(viewOffsetUvY, slot))
      )
    }
  }

  private func doubleAt(_ values: [Double], _ index: Int) -> Double {
    guard values.indices.contains(index) else { return 0.0 }
    return values[index]
  }

  private func currentPresentedFrame(arguments: Any?) -> Any? {
    guard textureId != nil else { return nil }
    let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") ?? -1
    if fileId >= 0, let player = nativePlayer {
      let tracks = player.trackDiagnostics()
      if shouldUseRendererOwnedPresentedFrame(player: player),
         tracks.count == 1,
         (tracks[0]["fileId"] as? Int) == fileId,
         let frame = player.lastRendererOwnedFrameInfo() {
        let map = frame.presentedFrameMap(fileId: fileId)
        tracePresentedFrameSource(
          route: "currentPresentedFrame",
          source: "renderer-owned-paused",
          fileId: fileId,
          map: map
        )
        return map
      }
      for track in tracks {
        if (track["fileId"] as? Int) == fileId {
          let map = presentedFrameMap(fromTrackDiagnostic: track, fileId: fileId)
          tracePresentedFrameSource(
            route: "currentPresentedFrame",
            source: "track-diagnostics",
            fileId: fileId,
            map: map
          )
          return map
        }
      }
    }
    let map = presentationState.currentPresentedFrameMap()
    tracePresentedFrameSource(
      route: "currentPresentedFrame",
      source: "presentation-state-fallback",
      fileId: fileId,
      map: map
    )
    return map
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
    let tracks = player.trackDiagnostics()
    if shouldUseRendererOwnedPresentedFrame(player: player),
       tracks.count == 1,
       let fileId = tracks[0]["fileId"] as? Int,
       let frame = player.lastRendererOwnedFrameInfo() {
      let map = frame.presentedFrameMap(fileId: fileId)
      tracePresentedFrameSource(
        route: "presentedFrames",
        source: "renderer-owned-paused",
        fileId: fileId,
        map: map
      )
      return [map]
    }
    return tracks.compactMap { track in
      guard let fileId = track["fileId"] as? Int else { return nil }
      let map = presentedFrameMap(fromTrackDiagnostic: track, fileId: fileId)
      tracePresentedFrameSource(
        route: "presentedFrames",
        source: "track-diagnostics",
        fileId: fileId,
        map: map
      )
      return map
    }
  }

  private func presentedFrameMap(
    fromTrackDiagnostic track: [String: Any],
    fileId: Int
  ) -> [String: Any] {
    [
      "fileId": fileId,
      "ptsUs": track["currentPtsUs"] as? Int64 ?? -1,
      "dtsUs": track["currentDtsUs"] as? Int64 ?? Int64.min,
      "durationUs": 0,
      "analysisFrameIndex": track["analysisFrameIndex"] as? Int ?? -1,
      "frameIdentityMode": track["frameIdentityMode"] as? Int ?? 0,
      "sourcePacketIndex": track["sourcePacketIndex"] as? Int ?? -1,
      "sourcePacketSize": track["sourcePacketSize"] as? Int ?? 0,
      "sourcePacketPos": track["sourcePacketPos"] as? Int64 ?? -1,
      "sourcePacketPtsUs": track["sourcePacketPtsUs"] as? Int64 ?? Int64.min,
      "sourcePacketDtsUs": track["sourcePacketDtsUs"] as? Int64 ?? Int64.min,
    ]
  }

  private func shouldUseRendererOwnedPresentedFrame(
    player: MacOSNativePlayerSession
  ) -> Bool {
    player.rendererOwnedPresentationActive() &&
      !playback.currentIsPlaying(player: player)
  }

  private func tracePresentedFrameSource(
    route: String,
    source: String,
    fileId: Int,
    map: [String: Any]
  ) {
    guard ProcessInfo.processInfo.environment["VOIDPLAYER_QUICK_MARK_TRACE"] == "1" else {
      return
    }
    let pts = map["ptsUs"] ?? "nil"
    let dts = map["dtsUs"] ?? "nil"
    let duration = map["durationUs"] ?? "nil"
    let analysis = map["analysisFrameIndex"] ?? "nil"
    let packetIndex = map["sourcePacketIndex"] ?? "nil"
    let packetPos = map["sourcePacketPos"] ?? "nil"
    NSLog(
      "VoidPlayer quickmark timing route=\(route) source=\(source) fileId=\(fileId) pts=\(pts) dts=\(dts) duration=\(duration) analysis=\(analysis) packetIndex=\(packetIndex) packetPos=\(packetPos)"
    )
  }

  private func createPlayer(arguments: Any?) -> Any {
    if let color = MacOSFlutterArguments.uint32Arg(arguments, "color") {
      viewportBackgroundColor = color
    }
    playbackSpeed = 1.0
    let result = lifecycle.create(
      arguments: arguments,
      playback: playback,
      tracks: tracks,
      presentationState: presentationState,
      markFrameAvailable: { [weak self] in
        self?.markFrameAvailable()
      }
    )
    refreshPresentationPolicyForCurrentTracks()
    ensureNativeCompositorMatchesCurrentConfiguration()
    if let viewportBackgroundColor {
      nativeCompositor?.setViewportBackgroundColor(viewportBackgroundColor)
    }
    nativeCompositor?.setVideoTexture(texture)
    emitPlaybackClock(force: true)
    return result
  }

  private func destroyPlayer() {
    playbackSpeed = 1.0
    texture?.clearStableDisplaySnapshot()
    presentation.resetLayout()
    lifecycle.destroy(playback: playback, tracks: tracks, presentationState: presentationState)
    MacOSPresentationConfiguration.resetForNoMedia()
    nativeCompositor?.setVideoTexture(nil)
    ensureNativeCompositorMatchesCurrentConfiguration()
  }

  private func emitPlaybackClock(force: Bool = false) {
    nativeEvents.emitPlaybackClock(
      currentPtsUs: transport.currentPts(player: nativePlayer, presentationState: presentationState),
      durationUs: tracks.isEmpty ? 0 : tracks.currentDurationUs,
      isPlaying: playback.currentIsPlaying(player: nativePlayer),
      playbackSpeed: playbackSpeed,
      force: force
    )
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

    let sourceProviderWasReady = nativeCompositorSourceProviderReady()
    let addResult = tracks.addTrack(
      arguments: arguments,
      backendName: backendName,
      nativePlayer: nativePlayer,
      textureDimensions: texture?.dimensions()
    )
    if addResult.payload is FlutterError {
      return addResult.payload
    }
    if !sourceProviderWasReady {
      clearNativeCompositorSourceProvider(reason: "track topology changed")
    }
    refreshPresentationPolicyForCurrentTracks()
    if addResult.refreshCurrentFrame {
      if !sourceProviderWasReady {
        presentation.refreshCurrentFrame(context: presentationContext())
      }
    }
    if addResult.markFrameAvailable {
      markFrameAvailable()
    }
    return addResult.payload
  }

  private func removeTrack(arguments: Any?) {
    let sourceProviderWasReady = nativeCompositorSourceProviderReady()
    let removeResult = tracks.removeTrack(
      arguments: arguments,
      backendName: backendName,
      nativePlayer: nativePlayer
    )
    if !sourceProviderWasReady {
      clearNativeCompositorSourceProvider(reason: "track topology changed")
    }
    if removeResult.destroyPlayer {
      destroyPlayer()
    } else if removeResult.refreshCurrentFrame {
      refreshPresentationPolicyForCurrentTracks()
      if !sourceProviderWasReady {
        presentation.refreshCurrentFrame(context: presentationContext())
      }
    }
  }

  private func markFrameAvailable(refreshSourceRing: Bool = true) {
    frameAvailableCount += 1
    frameAvailableRate.record()
    let nativeCompositorOwnsSurface =
      nativeCompositor != nil &&
      backendName == MacOSVideoTrackPayload.nativeFormatName &&
      (playback.isPlaying || nativeCompositorSourceProviderReady())
    nativeCompositor?.requestVideoReadyFrame(reason: "native-frame-available")
    if nativeCompositorOwnsSurface {
      flutterTextureFrameAvailableSkippedWhilePlayingCount += 1
      compositorVideoTextureRefreshSkippedWhilePlayingCount += 1
    } else {
      lifecycle.markFrameAvailable()
      compositorVideoTextureRefreshCount += 1
      nativeCompositor?.setVideoTexture(texture)
    }
    // Keep the live source ring mirroring the just-presented frame so a
    // playing-state pan reveals slid-to pixels immediately. No-op unless an
    // interaction has subscribed; coalesced on the ring's own queue.
    if refreshSourceRing, let player = nativePlayer {
      sourceProvider.requestRefresh(player: player)
    }
  }

  private func ensureNativeCompositorMatchesCurrentConfiguration() {
    let configuration = MacOSPresentationConfiguration.current
    guard configuration.nativeCompositorEnabled,
          textureId != nil,
          texture != nil else {
      clearNativeCompositorSourceProvider(reason: "compositor disabled")
      sourceProvider.detach(reason: "compositor disabled")
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
    clearNativeCompositorSourceProvider(reason: "compositor reconfigured")
    sourceProvider.detach(reason: "compositor reconfigured")
    nativeCompositor?.detach()
    nativeCompositor = nil
    guard let engine = flutterEngine,
          let contentView else {
      lastNativeCompositorFailure = "Flutter engine or content view is unavailable"
      emitNativeCompositorState()
      return
    }
    guard let compositor = MacOSNativeCompositorView(
      engine: engine,
      latencyProfiler: compositorLatencyProfiler
    ) else {
      lastNativeCompositorFailure = "native compositor initialization failed"
      emitNativeCompositorState()
      return
    }
    nativeCompositor = compositor
    sourceProvider.attach(compositor: compositor)
    if let viewportBackgroundColor {
      compositor.setViewportBackgroundColor(viewportBackgroundColor)
    }
    compositor.attach(to: contentView)
    lastNativeCompositorFailure = ""
    nativeCompositor?.setVideoTexture(texture)
    emitNativeCompositorState()
  }

  private func refreshPresentationPolicyForCurrentTracks() {
    guard backendName == MacOSVideoTrackPayload.nativeFormatName else { return }
    let nextConfiguration = MacOSPresentationConfiguration.resolve(
      hasHDRTrack: tracks.hasHDRTrack,
      screen: presentationScreen
    )
    let previousConfiguration = MacOSPresentationConfiguration.current
    guard nextConfiguration.mode != previousConfiguration.mode ||
            nextConfiguration.reason != previousConfiguration.reason ||
            nextConfiguration.displayEDRHeadroomX1000 !=
              previousConfiguration.displayEDRHeadroomX1000 else {
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
    diagnostics["macosFrameCallbackInlineDirtyDrainCount"] =
      inlineDirtyFrameCallbackDrainCount
    diagnostics["flutterTextureFrameAvailableSkippedWhilePlayingCount"] =
      flutterTextureFrameAvailableSkippedWhilePlayingCount
    diagnostics["compositorVideoTextureRefreshCount"] = compositorVideoTextureRefreshCount
    diagnostics["compositorVideoTextureRefreshSkippedWhilePlayingCount"] =
      compositorVideoTextureRefreshSkippedWhilePlayingCount
    return diagnostics
  }

  private func activeDurationUs() -> Int {
    let durationUs = tracks.currentDurationUs
    if durationUs > 0 {
      return durationUs
    }
    return backendName == MacOSVideoTrackPayload.nativeFormatName
      ? 0
      : MacOSVideoTrackPayload.syntheticDurationUs
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
      nativeCompositorSourceProjectionActive: shouldUseNativeCompositorSourceProjectionLayout(),
      nativeCompositorSourceProviderActive: nativeCompositorSourceProviderReady(),
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

  private func shouldUseNativeCompositorSourceProjectionLayout() -> Bool {
    sourceProvider.shouldUseProjectionLayout(
      nativeBackendActive: isNativeBackendActive(),
      isPlaying: playback.isPlaying
    )
  }

  private func nativeCompositorSourceProviderReady() -> Bool {
    sourceProvider.ready(nativeBackendActive: isNativeBackendActive())
  }

  private func setNativeCompositorSourceProviderActive(_ active: Bool) {
    guard !active || nativeCompositorSourceProviderReady() else {
      return
    }
    nativePlayer?.setViewportCompositorActive(active)
  }

  private func clearNativeCompositorSourceProvider(reason: String) {
    setNativeCompositorSourceProviderActive(false)
    sourceProvider.clear(reason: reason)
  }

  private func primeNativeCompositorPlaybackSource(reason: String) {
    guard let player = nativePlayer,
          nativeCompositorSourceProviderReady(),
          playback.currentIsPlaying(player: player) else {
      return
    }
    if !playback.sourceProviderFramePumpActive {
      _ = playback.ensurePresentationPump(
        player: player,
        texture: nil,
        maxTrackSlots: tracks.activeSlotCapacity(),
        userData: Unmanaged.passUnretained(self).toOpaque(),
        presentationState: presentationState,
        requiresPresentationTarget: false
      )
    }
    setNativeCompositorSourceProviderActive(true)
    player.noteViewportCompositorActivity()
    sourceProvider.requestRefresh(player: player)
    if MacOSProfilerLog.enabled {
      NSLog("VoidPlayer WGPU source provider primed reason=\(reason)")
    }
  }

  private func publishNativeCompositorSourceProviderReadyFrame(
    timeoutMs: Int,
    reason: String
  ) -> String? {
    guard let player = nativePlayer,
          nativeCompositorSourceProviderReady() else {
      return "source provider is not ready"
    }
    setNativeCompositorSourceProviderActive(true)
    return sourceProvider.publishReadyFrame(
      player: player,
      timeoutMs: timeoutMs,
      reason: reason
    )
  }

  private func sourceProviderExpectedFileIdsForPts(_ ptsUs: Int) -> [Int] {
    if nativeCompositorSourceProviderReady(),
       !sourceProvider.configuredExpectedFileIds.isEmpty {
      return sourceProvider.configuredExpectedFileIds
    }
    let player = nativePlayer
    return tracks.tracks.compactMap { track in
      guard let fileId = MacOSFlutterArguments.intValue(track["fileId"]) else {
        return nil
      }
      let durationUs = MacOSFlutterArguments.intValue(track["durationUs"]) ?? 0
      if durationUs <= 0 {
        return fileId
      }
      let offsetUs = player?.trackOffsetUs(fileId: fileId) ?? 0
      let localPtsUs = ptsUs - offsetUs
      return (localPtsUs >= 0 && localPtsUs < durationUs) ? fileId : nil
    }
  }

  private func transportContext() -> MacOSTransportContext {
    let sourceProviderReady = nativeCompositorSourceProviderReady()
    return MacOSTransportContext(
      nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
      player: nativePlayer,
      texture: nativeTexture,
      textureRegistered: textureId != nil,
      playback: playback,
      presentationState: presentationState,
      activeDurationUs: activeDurationUs(),
      maxTrackSlots: tracks.activeSlotCapacity(),
      userData: Unmanaged.passUnretained(self).toOpaque(),
      requiresPresentationTarget: !sourceProviderReady,
      setSourceProviderActive: { [weak self] active in
        guard !active || sourceProviderReady else {
          return
        }
        self?.setNativeCompositorSourceProviderActive(active)
      },
      sourceProviderExpectedFileIdsForPts: { [weak self] ptsUs in
        self?.sourceProviderExpectedFileIdsForPts(ptsUs) ?? []
      },
      publishSourceProviderReadyFrame: { [weak self] timeoutMs, reason in
        self?.publishNativeCompositorSourceProviderReadyFrame(
          timeoutMs: timeoutMs,
          reason: reason
        )
      },
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
    let sourceRingRefreshRequested = requestSourceRingRefreshFromNativeCallback(
      cachedPlaying: cachedPlaying
    )
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
        callbackContext: callbackContext,
        cachedPlaying: cachedPlaying,
        sourceRingRefreshRequested: sourceRingRefreshRequested,
        immediateDepth: 0
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
    callbackContext: MacOSNativeFrameCallbackContext?,
    cachedPlaying: Bool,
    sourceRingRefreshRequested: Bool,
    immediateDepth: Int
  ) {
    guard callbackContext?.isCurrent(callbackGeneration) == true else {
      _ = frameCallbackProfiler.finishProcessing(
        endNs: DispatchTime.now().uptimeNanoseconds
      )
      return
    }
    let startNs = DispatchTime.now().uptimeNanoseconds
    let sourceTick = shouldHandleFrameCallbackAsNativeCompositorSourceTick(
      cachedPlaying: cachedPlaying
    )
    let nativePlaying = sourceTick
      ? true
      : playback.syncPlayingState(player: nativePlayer)
    if nativePlaying && !sourceTick {
      emitPlaybackClock()
    }
    frameCallbackProfiler.recordMainStart(enqueueNs: enqueueNs, startNs: startNs)
    if !sourceTick, let generation = currentRendererOwnedTargetGeneration() {
      frameCallbackProfiler.recordTargetGeneration(generation, nowNs: startNs)
    }
    let suppressLayoutPublication =
      backendName == MacOSVideoTrackPayload.nativeFormatName &&
      presentation.shouldSuppressNativeCallbackPublicationDuringLayout()
    if suppressLayoutPublication {
      presentation.recordLayoutCallbackPublicationSuppressed()
    } else if sourceTick {
      presentationState.recordCallback()
      markFrameAvailable(refreshSourceRing: !sourceRingRefreshRequested)
      transport.resolvePendingSeekPreviewIfPresented(
        presentationState: presentationState,
        emitSeekPreviewPresented: { [weak self] requestId, targetPtsUs in
          self?.emitSeekPreviewPresented(requestId: requestId, targetPtsUs: targetPtsUs)
        }
      )
    } else {
      playback.handleFrameCallback(
        player: nativePlayer,
        texture: nativeTexture,
        maxTrackSlots: tracks.activeSlotCapacity(),
        nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
        presentationState: presentationState,
        markFrameAvailable: {
          self.markFrameAvailable(refreshSourceRing: !sourceRingRefreshRequested)
          self.transport.resolvePendingSeekPreviewIfPresented(
            presentationState: self.presentationState,
            emitSeekPreviewPresented: { [weak self] requestId, targetPtsUs in
              self?.emitSeekPreviewPresented(requestId: requestId, targetPtsUs: targetPtsUs)
            }
          )
        }
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
      let processDirtyCallback = { [weak self, weak callbackContext] in
        guard let self else { return }
        guard callbackContext?.isCurrent(callbackGeneration) == true else {
          _ = self.frameCallbackProfiler.finishProcessing(
            endNs: DispatchTime.now().uptimeNanoseconds
          )
          return
        }
        if !nativePlaying && !sourceTick {
          _ = self.playback.syncPlayingState(player: self.nativePlayer)
        }
        self.processNativeFrameCallback(
          enqueueNs: nextEnqueueNs,
          callbackGeneration: callbackGeneration,
          callbackContext: callbackContext,
          cachedPlaying: nativePlaying,
          sourceRingRefreshRequested: sourceRingRefreshRequested,
          immediateDepth: immediateDepth + 1
        )
      }
      if nativePlaying && immediateDepth < 2 {
        inlineDirtyFrameCallbackDrainCount += 1
        processDirtyCallback()
      } else if nativePlaying {
        DispatchQueue.main.async {
          processDirtyCallback()
        }
      } else {
        DispatchQueue.main.asyncAfter(
          deadline: .now() + .milliseconds(coalescedFrameCallbackDelayMs)
        ) {
          processDirtyCallback()
        }
      }
    }
  }

  private func shouldHandleFrameCallbackAsNativeCompositorSourceTick(
    cachedPlaying _: Bool
  ) -> Bool {
    backendName == MacOSVideoTrackPayload.nativeFormatName &&
      nativeCompositor != nil &&
      nativeCompositorSourceProviderReady()
  }

  private func requestSourceRingRefreshFromNativeCallback(cachedPlaying: Bool) -> Bool {
    guard cachedPlaying,
          let player = nativePlayer else {
      return false
    }
    return sourceProvider.requestRefreshIfConfigured(player: player)
  }

  private func currentRendererOwnedTargetGeneration() -> Int64? {
    guard let generation =
      nativePlayer?.rendererOwnedPresentationState()["targetGeneration"] else {
      return nil
    }
    switch generation {
    case let value as Int64:
      return value
    case let value as Int:
      return Int64(value)
    case let value as UInt64:
      return Int64(min(value, UInt64(Int64.max)))
    default:
      return nil
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
    var compositorDiagnostics: [String: Any] = [:]
    if let nativeCompositor {
      compositorDiagnostics = nativeCompositor.diagnostics()
    } else {
      compositorDiagnostics = compositorLatencyProfiler.diagnosticMap()
    }
    let textureStats = texture?.diagnostics()
    let textureDiagnostics: [String: Any] = [
      "pixelBufferRebuildCount": textureStats?.rebuildCount ?? 0,
      "pixelBufferAllocationCount": textureStats?.allocationCount ?? 0,
      "pixelBufferRebuildReuseCount": textureStats?.rebuildReuseCount ?? 0,
      "pixelBufferRebuildLastAllocatedCount": textureStats?.rebuildLastAllocatedCount ?? 0,
      "pixelBufferRebuildLastReusedCount": textureStats?.rebuildLastReusedCount ?? 0,
      "pixelBufferRebuildLastDurationMs": textureStats?.rebuildLastDurationMs ?? 0.0,
      "retiredPixelBufferCount": textureStats?.retiredPixelBufferCount ?? 0,
      "pixelBufferPrewarmRequestCount": textureStats?.prewarmRequestCount ?? 0,
      "pixelBufferPrewarmHitCount": textureStats?.prewarmHitCount ?? 0,
      "pixelBufferPrewarmReadyCount": textureStats?.prewarmReadyCount ?? 0,
      "pixelBufferPrewarmDroppedCount": textureStats?.prewarmDroppedCount ?? 0,
    ]
    MacOSRendererProfilerSummary.log(
      isPlaying: playback.isPlaying,
      trackCount: tracks.count,
      viewport: presentation.diagnosticMap(),
      callbacks: frameCallbackDiagnostics(),
      presentationFrames: presentationState.diagnosticMap(),
      perf: nativePlayer?.performanceStats() ?? [:],
      texture: textureDiagnostics,
      scheduler: nativePlayer?.presentationSchedulerStats() ?? [:],
      compositor: compositorDiagnostics,
      sourceRing: sourceProvider.diagnostics(nativeBackendActive: isNativeBackendActive())
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
