import Cocoa
import FlutterMacOS
import Metal

private enum MacOSRendererOwnedCompositeSubmitOutcome {
  case submitted
  case coalesced
  case failed
}

enum MacOSFlutterSurfaceExporter {
  private static let currentSurfaceInfosSelector =
    NSSelectorFromString("voidPlayerHDRCurrentFlutterSurfaceInfos")

  static func isAvailable(engine: FlutterEngine?) -> Bool {
    engine?.responds(to: currentSurfaceInfosSelector) == true
  }

  static func currentSurfaceInfos(engine: FlutterEngine?) -> [[String: Any]] {
    guard let engine,
          engine.responds(to: currentSurfaceInfosSelector),
          let raw = engine.perform(currentSurfaceInfosSelector)?.takeUnretainedValue() else {
      return []
    }
    if let infos = raw as? [[String: Any]] {
      return infos
    }
    if let infos = raw as? [Any] {
      return infos.compactMap { $0 as? [String: Any] }
    }
    return []
  }
}

final class MacOSVideoRendererBridge: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static weak var activeInstance: MacOSVideoRendererBridge?
  private static let rendererOwnedProjectionOnlyBoostReasons: Set<String> = [
    "viewport-pan",
    "viewport-split",
    "viewport-zoom",
  ]
  private static let rendererOwnedFlutterSurfaceWarmGraceNs: UInt64 = 500_000_000
  private static let rendererOwnedFlutterSurfaceContinuousComposeIntervalNs: UInt64 =
    16_000_000
  private static let rendererOwnedFlutterSurfaceContinuousSampleIntervalNs: UInt64 =
    33_000_000
  private static let rendererOwnedCompositeRefreshMaxInFlight = 2

  private var methodChannel: FlutterMethodChannel?
  private var eventChannel: FlutterEventChannel?
  private weak var flutterEngine: FlutterEngine?
  private weak var contentView: NSView?
  private let rendererOwnedCompositeRefreshQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.renderer-owned-composite-refresh",
    qos: .userInteractive
  )
  private lazy var rendererOwnedCompositeDisplayLink = MacOSViewportDisplayLink { [weak self] in
    self?.processRendererOwnedCompositeRefreshTick()
  }
  private var rendererOwnedCompositeRefreshInFlight = false
  private var rendererOwnedCompositeRefreshInFlightCount = 0
  private var rendererOwnedCompositeRefreshPendingReason: String?
  private var rendererOwnedCompositeRefreshPendingGeneration: UInt64 = 0
  private var rendererOwnedCompositeRefreshSubmittedGeneration: UInt64 = 0
  private var rendererOwnedCompositeRefreshCompletedGeneration: UInt64 = 0
  private var rendererOwnedCompositeRefreshAsyncStartNsByGeneration: [UInt64: UInt64] = [:]
  private var rendererOwnedCompositeRefreshTimedOutGenerations = Set<UInt64>()
  private var rendererOwnedCompositeRefreshRequestCount = 0
  private var rendererOwnedCompositeRefreshSubmitCount = 0
  private var rendererOwnedCompositeRefreshAppliedCount = 0
  private var rendererOwnedCompositeRefreshCoalescedCount = 0
  private var rendererOwnedCompositeRefreshFailureCount = 0
  private let rendererOwnedCompositeRefreshRequestRate = MacOSRateWindow()
  private let rendererOwnedCompositeRefreshSubmitRate = MacOSRateWindow()
  private let rendererOwnedCompositeRefreshPresentRate = MacOSRateWindow()
  private let rendererOwnedCompositeRefreshDuration = MacOSDurationWindow()
  private var rendererOwnedCompositeRefreshLatestProjectionNs: UInt64 = 0
  private var rendererOwnedCompositeRefreshLastSummaryLogNs: UInt64 = 0
  private var rendererOwnedFlutterSurfaceDirty = true
  private var rendererOwnedFlutterSurfaceLastReason = "initial"
  private var rendererOwnedFlutterSurfaceLastGeneration: UInt64 = 0
  private var rendererOwnedFlutterSurfaceContentGeneration: UInt64 = 1
  private var rendererOwnedFlutterSurfaceLastSourceKey: UInt64 = 0
  private var rendererOwnedFlutterSurfaceWarmUntilNs: UInt64 = 0
  private var rendererOwnedFlutterSurfaceDirtyCount = 0
  private var rendererOwnedFlutterSurfaceSampleCount = 0
  private var rendererOwnedFlutterSurfacePublishCount = 0
  private var rendererOwnedFlutterSurfaceUnchangedCount = 0
  private var rendererOwnedFlutterSurfaceSourceChangeCount = 0
  private var rendererOwnedFlutterSurfaceAwaitFirstCount = 0
  private var rendererOwnedViewportAwaitFirstCount = 0
  private var rendererOwnedFlutterSurfaceWarmTickCount = 0
  private var rendererOwnedFlutterSurfaceWarmComposeCount = 0
  private var rendererOwnedFlutterSurfaceContinuousTickCount = 0
  private var rendererOwnedFlutterSurfaceContinuousComposeCount = 0
  private var rendererOwnedFlutterSurfaceLastContinuousComposeNs: UInt64 = 0
  private var rendererOwnedFlutterSurfaceLastContinuousSampleNs: UInt64 = 0
  private var rendererOwnedFlutterSurfaceLastPublishLogNs: UInt64 = 0
  private let rendererOwnedFlutterSurfaceSampleDuration = MacOSDurationWindow()
  private let rendererOwnedSourceCachePublishRate = MacOSRateWindow()
  private let rendererOwnedSourceProjectionRate = MacOSRateWindow()
  private var rendererOwnedSourceCacheLastPublishCount: Int64 = 0
  private var viewportBackgroundColor: UInt32?
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
  private let coalescedFrameCallbackDelayMs = 8
  private var frameAvailableCount = 0
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
    ensureRendererOwnedPresentationMatchesCurrentConfiguration()
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

  private var rendererTarget: MacOSRendererOwnedPresentationTarget? {
    lifecycle.rendererTarget
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
        if backendName == MacOSVideoTrackPayload.nativeFormatName {
          if rendererTarget?.rendererOwnedRunnerLayerActive == true {
            scheduleRendererOwnedCompositeRefresh(reason: "background-color")
          } else {
            presentation.refreshCurrentFrame(context: presentationContext())
          }
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
      scheduleRendererOwnedCompositeRefresh(reason: "resize")
      result(nil)
    case "prewarmNativePresentationTargetSize":
      prewarmNativePresentationTargetSize(arguments: call.arguments)
      result(nil)
    case "setRendererOwnedViewportRect":
      presentation.setRendererOwnedViewportRect(
        arguments: call.arguments,
        rendererTarget: rendererTarget
      )
      if rendererTarget?.rendererOwnedRunnerLayerActive == true {
        scheduleRendererOwnedCompositeRefresh(reason: "viewport-rect")
      } else if backendName == MacOSVideoTrackPayload.nativeFormatName,
         !playback.currentIsPlaying(player: nativePlayer) {
        if presentation.refreshCurrentFrame(context: presentationContext()) {
          scheduleRendererOwnedCompositeRefreshNextTick(
            reason: "viewport-rect-followup"
          )
        }
      } else {
        scheduleRendererOwnedCompositeRefresh(reason: "viewport-rect")
      }
      result(nil)
    case "requestRendererOwnedFlutterSurface":
      let reason = MacOSFlutterArguments.stringArg(call.arguments, "reason") ??
        "request-flutter-frame"
      markRendererOwnedFlutterSurfaceDirty(reason: "request-\(reason)")
      result(nil)
    case "boostRendererOwnedFlutterSurfaceInteraction":
      let reason = MacOSFlutterArguments.stringArg(call.arguments, "reason") ?? "boost"
      if Self.rendererOwnedProjectionOnlyBoostReasons.contains(reason) {
        scheduleRendererOwnedCompositeRefresh(reason: "boost-\(reason)")
      } else {
        markRendererOwnedFlutterSurfaceDirty(reason: "boost-\(reason)")
      }
      result(nil)
    case "ackRendererOwnedFlutterSurfaceState":
      markRendererOwnedFlutterSurfaceDirty(reason: "ack-flutter-state")
      result(nil)
    case "prepareRendererOwnedSourceProjection":
      prepareRendererOwnedSourceProjection(arguments: call.arguments)
      result(nil)
    case "setNativeAnalysisOverlay":
      setNativeAnalysisOverlay(arguments: call.arguments)
      result(nil)
    case "clearRendererOwnedSourceProjection":
      clearRendererOwnedSourceProjection(arguments: call.arguments)
      result(nil)
    case "play":
      playback.play(
        player: nativePlayer,
        rendererTarget: rendererTarget,
        textureRegistered: textureId != nil,
        maxTrackSlots: tracks.activeSlotCapacity(),
        userData: Unmanaged.passUnretained(self).toOpaque(),
        presentationState: presentationState
      )
      scheduleRendererOwnedCompositeRefresh(reason: "playback-flutter-surface")
      emitPlaybackClock(force: true)
      result(nil)
    case "pause":
      playback.pause(player: nativePlayer)
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
        nativeEventDiagnostics: nativeEvents.diagnosticMap(),
        frameCallbackDiagnostics: frameCallbackDiagnostics(),
        viewportDiagnostics: presentation.diagnosticMap(),
        presentationDiagnostics: presentationState.diagnosticMap(),
        trackPayloads: tracks.tracks
      )
      diagnostics.merge(MacOSPresentationConfiguration.current.diagnostics) { _, next in next }
      diagnostics.merge(rendererOwnedCompositorDiagnostics()) { _, next in next }
      diagnostics["flutterSurfaceExporterAvailable"] =
        MacOSFlutterSurfaceExporter.isAvailable(engine: flutterEngine)
      result(diagnostics)
    case "debugFlutterSurfaceInfo":
      result(debugFlutterSurfaceInfo())
    case "debugRendererOwnedPresentation":
      var diagnostics = MacOSPresentationConfiguration.current.diagnostics
      let rendererOwnedReady = rendererOwnedPresentationReady()
      diagnostics.merge([
        "rendererOwnedPresentationRequested":
          MacOSPresentationConfiguration.current.rendererOwnedPresentationEnabled,
        "rendererOwnedPresentationReady": rendererOwnedReady,
        "rendererOwnedPresentationLastFailure":
          rendererOwnedReady ? "" : "renderer-owned presentation is not ready",
      ]) { _, next in next }
      diagnostics.merge(rendererOwnedCompositorDiagnostics()) { _, next in next }
      if let state = nativePlayer?.rendererOwnedPresentationState() {
        diagnostics.merge(Self.rendererOwnedColorDiagnostics(from: state)) { _, next in next }
      }
      result(diagnostics)
    case "captureViewport":
      result(MacOSViewportCapture.capture(texture: texture))
    case "captureViewportRegion":
      result(MacOSViewportCapture.captureRegion(texture: texture, arguments: call.arguments))
    case "captureWindow":
      result(MacOSViewportCapture.captureWindow(arguments: call.arguments))
    case "captureWindowRegion":
      result(MacOSViewportCapture.captureWindowRegion(arguments: call.arguments))
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

  private func rendererOwnedCompositorDiagnostics() -> [String: Any] {
    let state = nativePlayer?.rendererOwnedPresentationState() ?? [:]
    let perfStats = nativePlayer?.performanceStats() ?? [:]
    let rendererOwnedActive = state["active"] as? Bool ?? false
    let backend = state["backendName"] as? String ?? "unknown"
    recordRendererOwnedSourceCachePublishCount(
      int64DiagnosticValue(state["sourceCachePublishCount"])
    )
    let sourceCacheHz = rendererOwnedSourceCachePublishRate.rateHz()
    let sourceCachePublishCount = int64DiagnosticValue(state["sourceCachePublishCount"])
    let sourceCacheTextureCount = int64DiagnosticValue(state["sourceCacheTextureCount"])
    let sourceProjectionHz = rendererOwnedSourceProjectionRate.rateHz()
    let compositeRequestHz = rendererOwnedCompositeRefreshRequestRate.rateHz()
    let compositeSubmitHz = rendererOwnedCompositeRefreshSubmitRate.rateHz()
    let compositePresentHz = rendererOwnedCompositeRefreshPresentRate.rateHz()
    let retainedComposeHitRatioX1000 = retainedComposeHitRatioX1000(
      state: state,
      perfStats: perfStats
    )
    let latestProjectionLagMs =
      rendererOwnedCompositeRefreshLatestProjectionNs > 0
        ? Double(
          DispatchTime.now().uptimeNanoseconds - rendererOwnedCompositeRefreshLatestProjectionNs
        ) / 1_000_000.0
        : 0.0
    let compositeClockDiagnostics = rendererOwnedCompositeDisplayLink.diagnosticMap()
    let configuration = MacOSPresentationConfiguration.current
    let mode = rendererOwnedActive
      ? (backend.lowercased().contains("wgpu") ? "renderer-owned-wgpu-metal" : "renderer-owned-metal")
      : "inactive"
    var diagnostics: [String: Any] = [
      "rendererOwnedPresentationActive": rendererOwnedActive,
      "rendererOwnedPresentationMode": mode,
      "rendererOwnedPresentationBackendName": backend,
      "rendererOwnedExternalFlutterSurfaceUpdateCount":
        state["externalFlutterSurfaceUpdateCount"] ?? 0,
      "rendererOwnedExternalFlutterSurfaceConsumeCount":
        state["externalFlutterSurfaceConsumeCount"] ?? 0,
      "rendererOwnedFlutterSurfaceDirty":
        rendererOwnedFlutterSurfaceDirty,
      "rendererOwnedFlutterSurfaceDirtyCount":
        rendererOwnedFlutterSurfaceDirtyCount,
      "rendererOwnedFlutterSurfaceSampleCount":
        rendererOwnedFlutterSurfaceSampleCount,
      "rendererOwnedFlutterSurfacePublishCount":
        rendererOwnedFlutterSurfacePublishCount,
      "rendererOwnedFlutterSurfaceUnchangedCount":
        rendererOwnedFlutterSurfaceUnchangedCount,
      "rendererOwnedFlutterSurfaceSampleP95Ms":
        rendererOwnedFlutterSurfaceSampleDuration.p95Ms(),
      "rendererOwnedFlutterSurfaceSampleP95MsX1000":
        Int(rendererOwnedFlutterSurfaceSampleDuration.p95Ms() * 1000.0),
      "rendererOwnedFlutterSurfaceLastReason":
        rendererOwnedFlutterSurfaceLastReason,
      "rendererOwnedFlutterSurfaceLastGeneration":
        rendererOwnedFlutterSurfaceLastGeneration,
      "rendererOwnedFlutterSurfaceContentGeneration":
        rendererOwnedFlutterSurfaceContentGeneration,
      "rendererOwnedFlutterSurfaceLastSourceKey":
        rendererOwnedFlutterSurfaceLastSourceKey,
      "rendererOwnedFlutterSurfaceSourceChangeCount":
        rendererOwnedFlutterSurfaceSourceChangeCount,
      "rendererOwnedFlutterSurfaceAwaitFirstCount":
        rendererOwnedFlutterSurfaceAwaitFirstCount,
      "rendererOwnedViewportAwaitFirstCount":
        rendererOwnedViewportAwaitFirstCount,
      "rendererOwnedFlutterSurfaceWarmActive":
        rendererOwnedFlutterSurfaceWarmActive(),
      "rendererOwnedFlutterSurfaceWarmTickCount":
        rendererOwnedFlutterSurfaceWarmTickCount,
      "rendererOwnedFlutterSurfaceWarmComposeCount":
        rendererOwnedFlutterSurfaceWarmComposeCount,
      "rendererOwnedFlutterSurfaceContinuousTickCount":
        rendererOwnedFlutterSurfaceContinuousTickCount,
      "rendererOwnedFlutterSurfaceContinuousComposeCount":
        rendererOwnedFlutterSurfaceContinuousComposeCount,
      "rendererOwnedSourceProjectionUpdateCount":
        state["sourceProjectionUpdateCount"] ?? 0,
      "rendererOwnedSourceProjectionConsumeCount":
        state["sourceProjectionConsumeCount"] ?? 0,
      "rendererOwnedCompositeRefreshInFlight":
        rendererOwnedCompositeRefreshInFlight,
      "rendererOwnedCompositeRefreshInFlightCount":
        rendererOwnedCompositeRefreshInFlightCount,
      "rendererOwnedCompositeRefreshMaxInFlight":
        Self.rendererOwnedCompositeRefreshMaxInFlight,
      "rendererOwnedCompositeRefreshPendingGeneration":
        rendererOwnedCompositeRefreshPendingGeneration,
      "rendererOwnedCompositeRefreshSubmittedGeneration":
        rendererOwnedCompositeRefreshSubmittedGeneration,
      "rendererOwnedCompositeRefreshCompletedGeneration":
        rendererOwnedCompositeRefreshCompletedGeneration,
      "rendererOwnedCompositeRefreshRequestCount":
        rendererOwnedCompositeRefreshRequestCount,
      "rendererOwnedCompositeRefreshSubmitCount":
        rendererOwnedCompositeRefreshSubmitCount,
      "rendererOwnedCompositeRefreshAppliedCount":
        rendererOwnedCompositeRefreshAppliedCount,
      "rendererOwnedCompositeRefreshCoalescedCount":
        rendererOwnedCompositeRefreshCoalescedCount,
      "rendererOwnedCompositeRefreshFailureCount":
        rendererOwnedCompositeRefreshFailureCount,
      "rendererOwnedCompositeRequestHz": compositeRequestHz,
      "rendererOwnedCompositeRequestHzX1000": Int(compositeRequestHz * 1000.0),
      "rendererOwnedCompositeSubmitHz": compositeSubmitHz,
      "rendererOwnedCompositeSubmitHzX1000": Int(compositeSubmitHz * 1000.0),
      "rendererOwnedPresentHz": compositePresentHz,
      "rendererOwnedPresentHzX1000": Int(compositePresentHz * 1000.0),
      "rendererOwnedRetainedComposeHitRatioX1000":
        retainedComposeHitRatioX1000,
      "rendererOwnedComposeSkippedInFlight":
        rendererOwnedCompositeRefreshCoalescedCount,
      "rendererOwnedLatestProjectionLagMs": latestProjectionLagMs,
      "rendererOwnedLatestProjectionLagMsX1000": Int(latestProjectionLagMs * 1000.0),
      "rendererOwnedComposeDurationP95Ms":
        rendererOwnedCompositeRefreshDuration.p95Ms(),
      "rendererOwnedComposeDurationP95MsX1000":
        Int(rendererOwnedCompositeRefreshDuration.p95Ms() * 1000.0),
      "rendererOwnedComposeDisplayClockSource":
        compositeClockDiagnostics["viewportClockSource"] ?? "unknown",
      "rendererOwnedComposeDisplayClockRunning":
        compositeClockDiagnostics["viewportClockRunning"] ?? false,
      "rendererOwnedComposeDisplayTickHz":
        compositeClockDiagnostics["displayTickHz"] ?? 0.0,
      "rendererOwnedComposeDisplayTickHzX1000":
        compositeClockDiagnostics["displayTickHzX1000"] ?? 0,
      "rendererOwnedComposeDisplayDeliveredTickHz":
        compositeClockDiagnostics["displayDeliveredTickHz"] ?? 0.0,
      "rendererOwnedComposeDisplayDeliveredTickHzX1000":
        compositeClockDiagnostics["displayDeliveredTickHzX1000"] ?? 0,
      "rendererOwnedSourceCacheHz": sourceCacheHz,
      "rendererOwnedSourceCacheHzX1000": Int(sourceCacheHz * 1000.0),
      "rendererOwnedSourceProjectionHz": sourceProjectionHz,
      "rendererOwnedSourceProjectionHzX1000": Int(sourceProjectionHz * 1000.0),
      "sourceFrameHz": sourceCacheHz,
      "sourcePublishHz": sourceCacheHz,
      "sourceBakeHz": sourceCacheHz,
      "sourceRingBakeCount": sourceCachePublishCount,
      "sourceRingBakeHz": sourceCacheHz,
      "sourceRingBakeP95Ms": 0.0,
      "sourceRingBakeLastMs": 0.0,
      "sourceRingPublishCount": sourceCachePublishCount,
      "sourceRingPublishHz": sourceCacheHz,
      "sourceRingDepth": sourceCacheTextureCount,
      "sourceRetainMode": "wgpu-owned-ring",
      "retainedComposeHitRatio":
        Double(retainedComposeHitRatioX1000) / 1000.0,
      "retainedComposeHitRatioX1000": retainedComposeHitRatioX1000,
      "projectionUpdateHz": sourceProjectionHz,
      "composeRequestHz": compositeRequestHz,
      "composeSubmitHz": compositeSubmitHz,
      "presentHz": compositePresentHz,
      "composeSkippedInFlight": rendererOwnedCompositeRefreshCoalescedCount,
      "latestProjectionLagMs": latestProjectionLagMs,
    ]
    if let targetDiagnostics = rendererTarget?.rendererOwnedTargetDiagnostics() {
      diagnostics.merge(targetDiagnostics) { _, next in next }
    } else {
      diagnostics.merge([
        "rendererOwnedTargetPixelFormat":
          configuration.edrOutputEnabled ? "64RGBAHalf" : "32BGRA",
        "rendererOwnedEDROutputEnabled": configuration.edrOutputEnabled,
        "rendererOwnedEDRTargetSampleCount": 0,
        "rendererOwnedEDRTargetMaxRGBX1000": 0,
        "rendererOwnedEDRTargetPixelsOver1X1000": 0,
      ]) { _, next in next }
    }
    return diagnostics
  }

  private func retainedComposeHitRatioX1000(
    state: [String: Any],
    perfStats: [String: Any]
  ) -> Int {
    if let direct = perfStats["sourceFrameCacheHitRatioX1000"] as? Int {
      return direct
    }
    if let direct = state["sourceFrameCacheHitRatioX1000"] as? Int {
      return direct
    }
    let hits = int64DiagnosticValue(
      perfStats["sourceFrameCacheHitCount"] ?? state["sourceFrameCacheHitCount"]
    )
    let misses = int64DiagnosticValue(
      perfStats["sourceFrameCacheMissCount"] ?? state["sourceFrameCacheMissCount"]
    )
    let total = hits + misses
    guard total > 0 else { return 0 }
    return Int((hits * 1000) / total)
  }

  private func int64DiagnosticValue(_ value: Any?) -> Int64 {
    if let value = value as? Int64 {
      return value
    }
    if let value = value as? Int {
      return Int64(value)
    }
    if let value = value as? NSNumber {
      return value.int64Value
    }
    return 0
  }

  private func recordRendererOwnedSourceCachePublishCount(_ count: Int64) {
    if count < rendererOwnedSourceCacheLastPublishCount {
      rendererOwnedSourceCachePublishRate.reset()
      rendererOwnedSourceCacheLastPublishCount = count
      return
    }
    let delta = count - rendererOwnedSourceCacheLastPublishCount
    guard delta > 0 else { return }
    let nowNs = DispatchTime.now().uptimeNanoseconds
    for _ in 0..<min(delta, 8) {
      rendererOwnedSourceCachePublishRate.record(nowNs: nowNs)
    }
    rendererOwnedSourceCacheLastPublishCount = count
  }

  private func debugFlutterSurfaceInfo() -> Any {
    guard let engine = flutterEngine else {
      return FlutterError(code: "NO_ENGINE", message: "Flutter engine is unavailable", details: nil)
    }

    guard MacOSFlutterSurfaceExporter.isAvailable(engine: engine) else {
      NSLog("VoidPlayer HDR compositor: Flutter surface export API is unavailable")
      return FlutterError(
        code: "FLUTTER_SURFACE_EXPORT_UNAVAILABLE",
        message: "Flutter surface export API is unavailable",
        details: nil
      )
    }
    let infos = MacOSFlutterSurfaceExporter.currentSurfaceInfos(engine: engine)
    guard let info = infos.first else {
      NSLog("VoidPlayer renderer-owned presentation: no Flutter front surface is currently available")
      return FlutterError(code: "NO_FLUTTER_SURFACE", message: "No Flutter front surface", details: nil)
    }

    let texture = info["texture"] as? MTLTexture
    let ioSurface = info["ioSurface"]
    var payload = info
    payload.removeValue(forKey: "texture")
    payload.removeValue(forKey: "ioSurface")
    payload["flutterTextureObjectAvailable"] = texture != nil
    payload["flutterIOSurfaceObjectAvailable"] = ioSurface != nil

    NSLog(
      "VoidPlayer renderer-owned presentation: exported Flutter surface texture available=%@ ioSurface=%@ pointer=%@ format=%@ size=%@x%@ wideGamut=%@",
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

  private func prewarmNativePresentationTargetSize(arguments: Any?) {
    guard let texture else { return }
    texture.prewarmRendererTarget(
      width: MacOSFlutterArguments.intArg(arguments, "width") ?? 0,
      height: MacOSFlutterArguments.intArg(arguments, "height") ?? 0
    )
  }

  private func prepareRendererOwnedSourceProjection(arguments: Any?) {
    let trace = compositorLatencyProfiler.receive(
      route: "source-projection",
      arguments: arguments
    )
    let sourceOrder = MacOSFlutterArguments.intListArg(arguments, "sourceOrder")
    let sourceSlots = MacOSFlutterArguments.intListArg(arguments, "sourceSlots")
    let displayOffsetX = MacOSFlutterArguments.doubleListArg(arguments, "displayOffsetX")
    let displayOffsetY = MacOSFlutterArguments.doubleListArg(arguments, "displayOffsetY")
    let invDisplaySizeX = MacOSFlutterArguments.doubleListArg(arguments, "invDisplaySizeX")
    let invDisplaySizeY = MacOSFlutterArguments.doubleListArg(arguments, "invDisplaySizeY")
    let viewOffsetUvX = MacOSFlutterArguments.doubleListArg(arguments, "viewOffsetUvX")
    let viewOffsetUvY = MacOSFlutterArguments.doubleListArg(arguments, "viewOffsetUvY")
    let mode = MacOSFlutterArguments.intArg(arguments, "mode") ?? 0
    let splitPos = MacOSFlutterArguments.doubleArg(arguments, "splitPos") ?? 0.5
    let activeTrackCount = MacOSFlutterArguments.intArg(arguments, "activeTrackCount") ?? 1
    if sourceSlots.isEmpty {
      nativePlayer?.clearSourceProjection()
    } else {
      if nativePlayer?.updateSourceProjection(
        mode: mode,
        splitPos: splitPos,
        activeTrackCount: activeTrackCount,
        order: sourceOrder,
        displayOffsetX: displayOffsetX,
        displayOffsetY: displayOffsetY,
        invDisplaySizeX: invDisplaySizeX,
        invDisplaySizeY: invDisplaySizeY,
        viewOffsetUvX: viewOffsetUvX,
        viewOffsetUvY: viewOffsetUvY
      ) == true {
        rendererOwnedSourceProjectionRate.record()
        rendererOwnedCompositeRefreshLatestProjectionNs =
          DispatchTime.now().uptimeNanoseconds
      }
    }
    if let trace {
      compositorLatencyProfiler.recordApplied(
        trace,
        applyNs: DispatchTime.now().uptimeNanoseconds
      )
    }
    scheduleRendererOwnedCompositeRefresh(reason: "source-projection")
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
    scheduleRendererOwnedCompositeRefresh(reason: "analysis-overlay")
  }

  private func clearRendererOwnedSourceProjection(arguments: Any?) {
    _ = compositorLatencyProfiler.receive(route: "source-clear", arguments: arguments)
    nativePlayer?.clearSourceProjection()
    let reason = MacOSFlutterArguments.stringArg(arguments, "reason") ?? "source-clear"
    if rendererTarget?.rendererOwnedRunnerLayerActive == true {
      scheduleRendererOwnedCompositeRefresh(reason: reason)
    } else if backendName == MacOSVideoTrackPayload.nativeFormatName,
       !playback.currentIsPlaying(player: nativePlayer) {
      if presentation.refreshCurrentFrame(context: presentationContext()) {
        scheduleRendererOwnedCompositeRefreshNextTick(
          reason: "\(reason)-followup"
        )
      }
    } else {
      scheduleRendererOwnedCompositeRefresh(reason: reason)
    }
  }

  private func scheduleRendererOwnedCompositeRefreshNextTick(reason: String) {
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.016) { [weak self] in
      self?.scheduleRendererOwnedCompositeRefresh(reason: reason)
    }
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
      contentView: contentView,
      playback: playback,
      tracks: tracks,
      presentationState: presentationState,
      markFrameAvailable: { [weak self] in
        self?.markFrameAvailable()
      }
    )
    refreshPresentationPolicyForCurrentTracks()
    ensureRendererOwnedPresentationMatchesCurrentConfiguration()
    emitPlaybackClock(force: true)
    return result
  }

  private func destroyPlayer() {
    playbackSpeed = 1.0
    texture?.clearStableDisplaySnapshot()
    presentation.resetLayout()
    lifecycle.destroy(playback: playback, tracks: tracks, presentationState: presentationState)
    MacOSPresentationConfiguration.resetForNoMedia()
    ensureRendererOwnedPresentationMatchesCurrentConfiguration()
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

    let addResult = tracks.addTrack(
      arguments: arguments,
      backendName: backendName,
      nativePlayer: nativePlayer,
      textureDimensions: texture?.dimensions()
    )
    nativePlayer?.clearSourceProjection()
    refreshPresentationPolicyForCurrentTracks()
    if addResult.refreshCurrentFrame {
      if rendererTarget?.rendererOwnedRunnerLayerActive == true {
        scheduleRendererOwnedCompositeRefresh(reason: "add-track")
      } else {
        presentation.refreshCurrentFrame(context: presentationContext())
      }
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
    nativePlayer?.clearSourceProjection()
    if removeResult.destroyPlayer {
      destroyPlayer()
    } else if removeResult.refreshCurrentFrame {
      refreshPresentationPolicyForCurrentTracks()
      if rendererTarget?.rendererOwnedRunnerLayerActive == true {
        scheduleRendererOwnedCompositeRefresh(reason: "remove-track")
      } else {
        presentation.refreshCurrentFrame(context: presentationContext())
      }
    }
  }

  private func markFrameAvailable() {
    frameAvailableCount += 1
    frameAvailableRate.record()
    let rendererOwnedReady = rendererOwnedPresentationReady()
    compositorVideoTextureRefreshCount += 1
    let rendererOwnedLayerActive =
      rendererOwnedReady && (rendererTarget?.rendererOwnedRunnerLayerActive == true)
    if rendererOwnedLayerActive && playback.isPlaying {
      flutterTextureFrameAvailableSkippedWhilePlayingCount += 1
      compositorVideoTextureRefreshSkippedWhilePlayingCount += 1
      return
    }
    lifecycle.markFrameAvailable()
    if playback.isPlaying &&
        !rendererOwnedReady &&
        backendName == MacOSVideoTrackPayload.nativeFormatName {
      compositorVideoTextureRefreshSkippedWhilePlayingCount += 1
    }
  }

  private func scheduleRendererOwnedCompositeRefresh(reason: String) {
    guard Thread.isMainThread else {
      DispatchQueue.main.async { [weak self] in
        self?.scheduleRendererOwnedCompositeRefresh(reason: reason)
      }
      return
    }
    guard rendererOwnedPresentationReady(),
          let player = nativePlayer,
          let rendererTarget = rendererTarget else {
      return
    }
    _ = player
    _ = rendererTarget
    rendererOwnedCompositeRefreshPendingGeneration &+= 1
    rendererOwnedCompositeRefreshPendingReason = reason
    rendererOwnedCompositeRefreshRequestCount += 1
    rendererOwnedCompositeRefreshRequestRate.record()
    if rendererOwnedCompositeRefreshInFlightCount >=
        Self.rendererOwnedCompositeRefreshMaxInFlight {
      rendererOwnedCompositeRefreshCoalescedCount += 1
      return
    }
    rendererOwnedCompositeDisplayLink.start()
    logRendererOwnedCompositeSummaryIfNeeded(reason: reason)
  }

  private func processRendererOwnedCompositeRefreshTick() {
    guard Thread.isMainThread else {
      DispatchQueue.main.async { [weak self] in
        self?.processRendererOwnedCompositeRefreshTick()
      }
      return
    }
    guard rendererOwnedPresentationReady(),
          let player = nativePlayer,
          let rendererTarget = rendererTarget else {
      rendererOwnedCompositeDisplayLink.stop()
      return
    }
    let tickNs = DispatchTime.now().uptimeNanoseconds
    let hasPendingComposite =
      rendererOwnedCompositeRefreshPendingGeneration >
        rendererOwnedCompositeRefreshSubmittedGeneration
    let flutterSurfaceWarmActive =
      rendererOwnedFlutterSurfaceWarmActive(nowNs: tickNs)
    let flutterSurfaceContinuousActive =
      rendererOwnedFlutterSurfaceContinuousActive()
    let flutterSurfaceContinuousComposeDue =
      rendererOwnedFlutterSurfaceContinuousComposeDue(nowNs: tickNs)
    let flutterSurfaceContinuousSampleDue =
      rendererOwnedFlutterSurfaceContinuousSampleDue(nowNs: tickNs)
    let flutterSurfaceSampleLatestActive =
      flutterSurfaceWarmActive || flutterSurfaceContinuousSampleDue
    guard hasPendingComposite || flutterSurfaceWarmActive ||
            flutterSurfaceContinuousComposeDue else {
      if rendererOwnedCompositeRefreshInFlightCount == 0 {
        if flutterSurfaceContinuousActive {
          rendererOwnedCompositeDisplayLink.start()
        } else {
          rendererOwnedCompositeDisplayLink.stop()
        }
      }
      return
    }
    if rendererOwnedCompositeRefreshInFlightCount >=
        Self.rendererOwnedCompositeRefreshMaxInFlight {
      rendererOwnedCompositeRefreshCoalescedCount += 1
      return
    }
    if !hasPendingComposite {
      rendererOwnedCompositeRefreshPendingGeneration &+= 1
      if flutterSurfaceContinuousComposeDue {
        rendererOwnedCompositeRefreshPendingReason = "flutter-surface-continuous"
        rendererOwnedFlutterSurfaceContinuousTickCount += 1
      } else {
        rendererOwnedCompositeRefreshPendingReason = "flutter-surface-warm"
        rendererOwnedFlutterSurfaceWarmTickCount += 1
      }
    }
    let submittedGeneration = rendererOwnedCompositeRefreshPendingGeneration
    let reason = rendererOwnedCompositeRefreshPendingReason ?? "display-link"
    let startNs = tickNs
    rendererOwnedCompositeRefreshInFlightCount += 1
    rendererOwnedCompositeRefreshInFlight = true
    rendererOwnedCompositeRefreshSubmittedGeneration = submittedGeneration
    rendererOwnedCompositeRefreshSubmitCount += 1
    rendererOwnedCompositeRefreshSubmitRate.record(nowNs: startNs)
    if flutterSurfaceContinuousComposeDue {
      rendererOwnedFlutterSurfaceLastContinuousComposeNs = tickNs
    }
    if flutterSurfaceContinuousSampleDue {
      rendererOwnedFlutterSurfaceLastContinuousSampleNs = tickNs
    }
    refreshRendererOwnedFlutterSurfaceIfNeeded(
      reason: reason,
      sampleLatest: flutterSurfaceSampleLatestActive
    )
    if rendererTarget.rendererOwnedRunnerLayerActive &&
        rendererOwnedFlutterSurfaceLastGeneration == 0 {
      rendererOwnedFlutterSurfaceAwaitFirstCount += 1
      rendererOwnedFlutterSurfaceLastReason = "\(reason):awaiting-first-graph"
      finishRendererOwnedCompositeRefresh(
        submittedGeneration: submittedGeneration,
        startNs: startNs
      )
      return
    }
    if rendererTarget.rendererOwnedRunnerLayerActive &&
        !presentation.rendererOwnedViewportRectReady() {
      rendererOwnedViewportAwaitFirstCount += 1
      rendererOwnedFlutterSurfaceLastReason = "\(reason):awaiting-viewport"
      finishRendererOwnedCompositeRefresh(
        submittedGeneration: submittedGeneration,
        startNs: startNs
      )
      return
    }
    let maxTrackSlots = tracks.activeSlotCapacity()
    if rendererTarget.rendererOwnedRunnerLayerActive {
      guard playback.ensurePresentationPump(
        player: player,
        rendererTarget: rendererTarget,
        maxTrackSlots: maxTrackSlots,
        userData: Unmanaged.passUnretained(self).toOpaque(),
        presentationState: presentationState
      ) else {
        rendererOwnedCompositeRefreshFailureCount += 1
        finishRendererOwnedCompositeRefresh(
          submittedGeneration: submittedGeneration,
          startNs: startNs
        )
        return
      }
      rendererOwnedCompositeRefreshAsyncStartNsByGeneration[submittedGeneration] =
        startNs
      rendererOwnedCompositeRefreshQueue.async { [weak self, weak player, weak rendererTarget] in
        let submitOutcome: MacOSRendererOwnedCompositeSubmitOutcome
        if let player, let rendererTarget {
          do {
            try rendererTarget.submitRetainedCompositeFromNativePlayer(
              player,
              maxTrackSlots: maxTrackSlots
            ) { [weak self, weak player, weak rendererTarget] result in
              DispatchQueue.main.async { [weak self, weak player, weak rendererTarget] in
                guard let self else { return }
                guard let submissionStartNs =
                        self.rendererOwnedCompositeRefreshAsyncStartNsByGeneration[
                          submittedGeneration
                        ] else {
                  return
                }
                self.rendererOwnedCompositeRefreshAsyncStartNsByGeneration
                  .removeValue(forKey: submittedGeneration)
                let completionWasTimedOut =
                  self.rendererOwnedCompositeRefreshTimedOutGenerations
                    .remove(submittedGeneration) != nil
                guard let player,
                      let rendererTarget,
                      self.nativePlayer === player,
                      self.rendererTarget === rendererTarget else {
                  self.rendererOwnedCompositeRefreshFailureCount += 1
                  if !completionWasTimedOut {
                    self.finishRendererOwnedCompositeRefresh(
                      submittedGeneration: submittedGeneration,
                      startNs: submissionStartNs
                    )
                  }
                  return
                }
                switch result {
                case .presented(let frameInfo):
                  self.presentationState.recordPresentation(rendererOwned: true)
                  if let frameInfo {
                    self.presentationState.recordFrame(frameInfo)
                    self.transport.resolvePendingSeekPreviewIfPresented(
                      presentationState: self.presentationState,
                      emitSeekPreviewPresented: { [weak self] requestId, targetPtsUs in
                        self?.emitSeekPreviewPresented(
                          requestId: requestId,
                          targetPtsUs: targetPtsUs
                        )
                      }
                    )
                  }
                  self.rendererOwnedCompositeRefreshAppliedCount += 1
                  self.rendererOwnedCompositeRefreshPresentRate.record()
                  self.markFrameAvailable()
                case .coalesced(_):
                  self.presentationState.recordMiss()
                  self.rendererOwnedCompositeRefreshCoalescedCount += 1
                  self.rendererOwnedCompositeRefreshPendingGeneration &+= 1
                  self.rendererOwnedCompositeRefreshPendingReason = "\(reason)-retry"
                case .failed(_):
                  self.presentationState.recordMiss()
                  self.rendererOwnedCompositeRefreshFailureCount += 1
                }
                if !completionWasTimedOut {
                  self.finishRendererOwnedCompositeRefresh(
                    submittedGeneration: submittedGeneration,
                    startNs: submissionStartNs
                  )
                }
                self.logRendererOwnedCompositeSummaryIfNeeded(reason: reason)
              }
            }
            submitOutcome = .submitted
          } catch {
            submitOutcome =
              (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
              ? .coalesced
              : .failed
          }
        } else {
          submitOutcome = .failed
        }
        DispatchQueue.main.async { [weak self, weak player, weak rendererTarget] in
          guard let self else { return }
          guard self.rendererOwnedCompositeRefreshAsyncStartNsByGeneration[
            submittedGeneration
          ] != nil else {
            return
          }
          guard let player,
                let rendererTarget,
                self.nativePlayer === player,
                self.rendererTarget === rendererTarget else {
            let submissionStartNs =
              self.rendererOwnedCompositeRefreshAsyncStartNsByGeneration
                .removeValue(forKey: submittedGeneration) ?? startNs
            self.rendererOwnedCompositeRefreshFailureCount += 1
            self.finishRendererOwnedCompositeRefresh(
              submittedGeneration: submittedGeneration,
              startNs: submissionStartNs
            )
            return
          }
          switch submitOutcome {
          case .submitted:
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(500)) {
              [weak self] in
              self?.timeoutRendererOwnedCompositeRefreshIfStillPending(
                submittedGeneration: submittedGeneration
              )
            }
          case .coalesced:
            let submissionStartNs =
              self.rendererOwnedCompositeRefreshAsyncStartNsByGeneration
                .removeValue(forKey: submittedGeneration) ?? startNs
            self.presentationState.recordMiss()
            self.rendererOwnedCompositeRefreshCoalescedCount += 1
            self.rendererOwnedCompositeRefreshPendingGeneration &+= 1
            self.rendererOwnedCompositeRefreshPendingReason = "\(reason)-retry"
            self.finishRendererOwnedCompositeRefresh(
              submittedGeneration: submittedGeneration,
              startNs: submissionStartNs
            )
          case .failed:
            let submissionStartNs =
              self.rendererOwnedCompositeRefreshAsyncStartNsByGeneration
                .removeValue(forKey: submittedGeneration) ?? startNs
            self.presentationState.recordMiss()
            self.rendererOwnedCompositeRefreshFailureCount += 1
            self.finishRendererOwnedCompositeRefresh(
              submittedGeneration: submittedGeneration,
              startNs: submissionStartNs
            )
          }
          self.logRendererOwnedCompositeSummaryIfNeeded(reason: reason)
        }
      }
      return
    }
    rendererOwnedCompositeRefreshQueue.async { [weak self, weak player, weak rendererTarget] in
      guard let self,
            let player,
            let rendererTarget else {
        DispatchQueue.main.async { [weak self] in
          self?.finishRendererOwnedCompositeRefresh(
            submittedGeneration: submittedGeneration,
            startNs: startNs
          )
        }
        return
      }
      let drawResult = MacOSNativeFrameRefresh.drawCurrentFrameForLayoutRefresh(
        player: player,
        rendererTarget: rendererTarget,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 8
      )
      DispatchQueue.main.async { [weak self, weak player, weak rendererTarget] in
        guard let self else { return }
        defer {
          self.finishRendererOwnedCompositeRefresh(
            submittedGeneration: submittedGeneration,
            startNs: startNs
          )
        }
        guard let player,
              let rendererTarget,
              self.nativePlayer === player,
              self.rendererTarget === rendererTarget else {
          if case .ready(let pending) = drawResult {
            rendererTarget?.discardPendingNativeFrame(pending)
          }
          self.rendererOwnedCompositeRefreshFailureCount += 1
          return
        }
        switch drawResult {
        case .ready(let pending):
          if MacOSNativeFrameRefresh.publishLayoutRefreshFrame(
            pending,
            player: player,
            rendererTarget: rendererTarget,
            maxTrackSlots: self.tracks.activeSlotCapacity(),
            presentationState: self.presentationState,
            framePump: self.playback.framePumpForRefresh
          ) {
            self.rendererOwnedCompositeRefreshAppliedCount += 1
            self.rendererOwnedCompositeRefreshPresentRate.record()
            self.markFrameAvailable()
          } else {
            self.rendererOwnedCompositeRefreshFailureCount += 1
          }
        case .coalesced:
          self.presentationState.recordMiss()
          self.rendererOwnedCompositeRefreshCoalescedCount += 1
          self.rendererOwnedCompositeRefreshPendingGeneration &+= 1
          self.rendererOwnedCompositeRefreshPendingReason = "\(reason)-retry"
        case .failed:
          self.presentationState.recordMiss()
          self.rendererOwnedCompositeRefreshFailureCount += 1
        }
        self.logRendererOwnedCompositeSummaryIfNeeded(reason: reason)
      }
    }
  }

  private func finishRendererOwnedCompositeRefresh(
    submittedGeneration: UInt64,
    startNs: UInt64
  ) {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    rendererOwnedCompositeRefreshDuration.record(nowNs - startNs)
    rendererOwnedCompositeRefreshInFlightCount = max(
      0,
      rendererOwnedCompositeRefreshInFlightCount - 1
    )
    rendererOwnedCompositeRefreshInFlight =
      rendererOwnedCompositeRefreshInFlightCount > 0
    rendererOwnedCompositeRefreshCompletedGeneration = max(
      rendererOwnedCompositeRefreshCompletedGeneration,
      submittedGeneration
    )
    guard rendererOwnedCompositeRefreshPendingGeneration > submittedGeneration else {
      let flutterSurfaceContinuousActive =
        rendererOwnedFlutterSurfaceContinuousActive()
      if rendererOwnedFlutterSurfaceWarmActive(nowNs: nowNs) {
        rendererOwnedCompositeRefreshPendingGeneration &+= 1
        rendererOwnedCompositeRefreshPendingReason = "flutter-surface-warm"
        rendererOwnedFlutterSurfaceWarmComposeCount += 1
        rendererOwnedCompositeDisplayLink.start()
        return
      }
      if flutterSurfaceContinuousActive {
        rendererOwnedCompositeDisplayLink.start()
        return
      }
      rendererOwnedCompositeRefreshPendingReason = nil
      rendererOwnedCompositeDisplayLink.stop()
      return
    }
    rendererOwnedCompositeDisplayLink.start()
  }

  private func finishRendererOwnedCompositeRefreshFromCallback() {
    guard rendererTarget?.rendererOwnedRunnerLayerActive != true,
          rendererOwnedCompositeRefreshInFlight,
          let submittedGeneration =
            rendererOwnedCompositeRefreshAsyncStartNsByGeneration.keys.min(),
          let startNs =
            rendererOwnedCompositeRefreshAsyncStartNsByGeneration
              .removeValue(forKey: submittedGeneration) else {
      return
    }
    rendererOwnedCompositeRefreshAppliedCount += 1
    rendererOwnedCompositeRefreshPresentRate.record()
    finishRendererOwnedCompositeRefresh(
      submittedGeneration: submittedGeneration,
      startNs: startNs
    )
  }

  private func timeoutRendererOwnedCompositeRefreshIfStillPending(
    submittedGeneration: UInt64
  ) {
    guard rendererOwnedCompositeRefreshInFlight,
          let startNs =
            rendererOwnedCompositeRefreshAsyncStartNsByGeneration[submittedGeneration],
          !rendererOwnedCompositeRefreshTimedOutGenerations.contains(submittedGeneration) else {
      return
    }
    rendererOwnedCompositeRefreshTimedOutGenerations.insert(submittedGeneration)
    rendererOwnedCompositeRefreshCoalescedCount += 1
    presentationState.recordMiss()
    finishRendererOwnedCompositeRefresh(
      submittedGeneration: submittedGeneration,
      startNs: startNs
    )
  }

  private func logRendererOwnedCompositeSummaryIfNeeded(reason: String) {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    guard nowNs > rendererOwnedCompositeRefreshLastSummaryLogNs + 1_000_000_000 else {
      return
    }
    rendererOwnedCompositeRefreshLastSummaryLogNs = nowNs
    let requestHz = rendererOwnedCompositeRefreshRequestRate.rateHz(nowNs: nowNs)
    let submitHz = rendererOwnedCompositeRefreshSubmitRate.rateHz(nowNs: nowNs)
    let presentHz = rendererOwnedCompositeRefreshPresentRate.rateHz(nowNs: nowNs)
    let projectionHz = rendererOwnedSourceProjectionRate.rateHz(nowNs: nowNs)
    let sourceHz = rendererOwnedSourceCachePublishRate.rateHz(nowNs: nowNs)
    let displayDiagnostics = rendererOwnedCompositeDisplayLink.diagnosticMap()
    let displayHz = displayDiagnostics["displayDeliveredTickHz"] as? Double ?? 0.0
    let projectionLagMs =
      rendererOwnedCompositeRefreshLatestProjectionNs > 0
        ? Double(nowNs - rendererOwnedCompositeRefreshLatestProjectionNs) / 1_000_000.0
        : 0.0
    let targetDiagnostics = rendererTarget?.rendererOwnedTargetDiagnostics() ?? [:]
    let layerSubmit = int64DiagnosticValue(
      targetDiagnostics["rendererOwnedLayerRetainedCompositeSubmitCount"]
    )
    let layerPresent = int64DiagnosticValue(
      targetDiagnostics["rendererOwnedLayerRetainedCompositePresentCount"]
    )
    let layerFailure = int64DiagnosticValue(
      targetDiagnostics["rendererOwnedLayerRetainedCompositeFailureCount"]
    )
    let layerInFlight = int64DiagnosticValue(
      targetDiagnostics["rendererOwnedLayerInFlightDrawableCount"]
    )
    let drawableAcquire = int64DiagnosticValue(
      targetDiagnostics["rendererOwnedLayerDrawableAcquireCount"]
    )
    let drawableAcquireFailure = int64DiagnosticValue(
      targetDiagnostics["rendererOwnedLayerDrawableAcquireFailureCount"]
    )
    let drawableAcquireP95Ms =
      targetDiagnostics["rendererOwnedLayerDrawableAcquireP95Ms"] as? Double ?? 0.0
    let layerCoalescedReason =
      targetDiagnostics["rendererOwnedLayerLastCoalescedReason"] as? String ?? "n/a"
    let summary = String(
      format: "VoidPlayer renderer-owned compose summary reason=%@ requestHz=%.1f submitHz=%.1f presentHz=%.1f displayTickHz=%.1f sourceFrameHz=%.1f projectionUpdateHz=%.1f pending=%llu submitted=%llu completed=%llu inFlight=%@ skippedInFlight=%d failures=%d projectionLagMs=%.2f composeP95Ms=%.2f flutterSurfaceDirty=%d sample=%d publish=%d unchanged=%d sourceChanges=%d awaitFirst=%d viewportAwait=%d warmActive=%@ warmTicks=%d warmComposes=%d continuousTicks=%d continuousComposes=%d sampleP95Ms=%.2f flutterReason=%@ layerSubmit=%lld layerPresent=%lld layerFailure=%lld layerInFlight=%lld drawableAcquire=%lld drawableAcquireFailure=%lld drawableAcquireP95Ms=%.2f layerCoalescedReason=%@",
      reason,
      requestHz,
      submitHz,
      presentHz,
      displayHz,
      sourceHz,
      projectionHz,
      rendererOwnedCompositeRefreshPendingGeneration,
      rendererOwnedCompositeRefreshSubmittedGeneration,
      rendererOwnedCompositeRefreshCompletedGeneration,
      rendererOwnedCompositeRefreshInFlight ? "true" : "false",
      rendererOwnedCompositeRefreshCoalescedCount,
      rendererOwnedCompositeRefreshFailureCount,
      projectionLagMs,
      rendererOwnedCompositeRefreshDuration.p95Ms(),
      rendererOwnedFlutterSurfaceDirtyCount,
      rendererOwnedFlutterSurfaceSampleCount,
      rendererOwnedFlutterSurfacePublishCount,
      rendererOwnedFlutterSurfaceUnchangedCount,
      rendererOwnedFlutterSurfaceSourceChangeCount,
      rendererOwnedFlutterSurfaceAwaitFirstCount,
      rendererOwnedViewportAwaitFirstCount,
      rendererOwnedFlutterSurfaceWarmActive(nowNs: nowNs) ? "true" : "false",
      rendererOwnedFlutterSurfaceWarmTickCount,
      rendererOwnedFlutterSurfaceWarmComposeCount,
      rendererOwnedFlutterSurfaceContinuousTickCount,
      rendererOwnedFlutterSurfaceContinuousComposeCount,
      rendererOwnedFlutterSurfaceSampleDuration.p95Ms(),
      rendererOwnedFlutterSurfaceLastReason,
      layerSubmit,
      layerPresent,
      layerFailure,
      layerInFlight,
      drawableAcquire,
      drawableAcquireFailure,
      drawableAcquireP95Ms,
      layerCoalescedReason
    )
    NSLog("%@", summary)
    summary.withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
  }

  private func markRendererOwnedFlutterSurfaceDirty(reason: String) {
    guard Thread.isMainThread else {
      DispatchQueue.main.async { [weak self] in
        self?.markRendererOwnedFlutterSurfaceDirty(reason: reason)
      }
      return
    }
    rendererOwnedFlutterSurfaceDirty = true
    rendererOwnedFlutterSurfaceDirtyCount += 1
    rendererOwnedFlutterSurfaceContentGeneration =
      rendererOwnedFlutterSurfaceContentGeneration == UInt64.max
        ? 1
        : rendererOwnedFlutterSurfaceContentGeneration + 1
    rendererOwnedFlutterSurfaceLastReason = reason
    let shouldWarmSample = reason.contains("warm=1")
    if shouldWarmSample {
      rendererOwnedFlutterSurfaceWarmUntilNs =
        DispatchTime.now().uptimeNanoseconds + Self.rendererOwnedFlutterSurfaceWarmGraceNs
    }
    scheduleRendererOwnedCompositeRefresh(reason: "flutter-surface-\(reason)")
  }

  private func rendererOwnedFlutterSurfaceWarmActive(nowNs: UInt64 = DispatchTime.now().uptimeNanoseconds) -> Bool {
    rendererOwnedFlutterSurfaceWarmUntilNs > 0 &&
      nowNs < rendererOwnedFlutterSurfaceWarmUntilNs
  }

  private func rendererOwnedFlutterSurfaceContinuousActive() -> Bool {
    rendererOwnedPresentationReady() &&
      rendererTarget?.rendererOwnedRunnerLayerActive == true
  }

  private func rendererOwnedFlutterSurfaceContinuousComposeDue(nowNs: UInt64) -> Bool {
    guard rendererOwnedFlutterSurfaceContinuousActive() else { return false }
    let lastNs = rendererOwnedFlutterSurfaceLastContinuousComposeNs
    return lastNs == 0 ||
      nowNs >= lastNs + Self.rendererOwnedFlutterSurfaceContinuousComposeIntervalNs
  }

  private func rendererOwnedFlutterSurfaceContinuousSampleDue(nowNs: UInt64) -> Bool {
    guard rendererOwnedFlutterSurfaceContinuousActive() else { return false }
    let lastNs = rendererOwnedFlutterSurfaceLastContinuousSampleNs
    return lastNs == 0 ||
      nowNs >= lastNs + Self.rendererOwnedFlutterSurfaceContinuousSampleIntervalNs
  }

  @discardableResult
  private func refreshRendererOwnedFlutterSurfaceIfNeeded(
    reason: String,
    force: Bool = false,
    sampleLatest: Bool = false
  ) -> Bool {
    guard Thread.isMainThread else { return false }
    guard backendName == MacOSVideoTrackPayload.nativeFormatName,
          MacOSPresentationConfiguration.current.rendererOwnedPresentationEnabled,
          rendererTarget?.rendererOwnedRunnerLayerActive == true,
          let player = nativePlayer else {
      return false
    }
    let needsInitialSurface = rendererOwnedFlutterSurfaceLastGeneration == 0
    let shouldSample = force ||
      rendererOwnedFlutterSurfaceDirty ||
      needsInitialSurface ||
      sampleLatest
    guard shouldSample else {
      return false
    }
    rendererOwnedFlutterSurfaceSampleCount += 1
    let sampleStartNs = DispatchTime.now().uptimeNanoseconds
    guard let engine = flutterEngine,
          MacOSFlutterSurfaceExporter.isAvailable(engine: engine) else {
      rendererOwnedFlutterSurfaceSampleDuration.record(
        DispatchTime.now().uptimeNanoseconds - sampleStartNs
      )
      if needsInitialSurface {
        rendererOwnedFlutterSurfaceAwaitFirstCount += 1
        rendererOwnedFlutterSurfaceLastReason = "\(reason):awaiting-exporter"
      } else {
        rendererOwnedFlutterSurfaceDirty = false
        rendererOwnedFlutterSurfaceLastReason = "\(reason):exporter-unavailable"
      }
      return false
    }
    let infos = MacOSFlutterSurfaceExporter.currentSurfaceInfos(engine: engine)
    rendererOwnedFlutterSurfaceSampleDuration.record(
      DispatchTime.now().uptimeNanoseconds - sampleStartNs
    )
    guard let info = infos.first,
          let texture = info["texture"] as? MTLTexture else {
      if needsInitialSurface {
        rendererOwnedFlutterSurfaceAwaitFirstCount += 1
        rendererOwnedFlutterSurfaceLastReason = "\(reason):awaiting-surface"
      } else {
        rendererOwnedFlutterSurfaceDirty = false
        rendererOwnedFlutterSurfaceLastReason = "\(reason):surface-unavailable"
      }
      return false
    }
    let sourceKey = flutterSurfaceSourceKey(info: info, texture: texture)
    let sourceChanged = sourceKey != rendererOwnedFlutterSurfaceLastSourceKey
    if sampleLatest && !rendererOwnedFlutterSurfaceDirty &&
        !needsInitialSurface && !sourceChanged {
      rendererOwnedFlutterSurfaceUnchangedCount += 1
      rendererOwnedFlutterSurfaceLastReason = "\(reason):latest-unchanged"
      return false
    }
    if sourceChanged && !rendererOwnedFlutterSurfaceDirty &&
        !needsInitialSurface {
      rendererOwnedFlutterSurfaceContentGeneration =
        rendererOwnedFlutterSurfaceContentGeneration == UInt64.max
          ? 1
          : rendererOwnedFlutterSurfaceContentGeneration + 1
    }
    let generation = rendererOwnedFlutterSurfaceContentGeneration
    if generation == rendererOwnedFlutterSurfaceLastGeneration && !sourceChanged {
      rendererOwnedFlutterSurfaceUnchangedCount += 1
      rendererOwnedFlutterSurfaceDirty = false
      if sampleLatest {
        rendererOwnedFlutterSurfaceLastReason = "\(reason):latest-unchanged"
      } else {
        rendererOwnedFlutterSurfaceLastReason = "\(reason):unchanged"
      }
      return false
    }
    let updated = publishFlutterSurfaceToNativeRenderer(
      player: player,
      texture: texture,
      frameGeneration: generation
    )
    rendererOwnedFlutterSurfaceDirty = false
    rendererOwnedFlutterSurfaceLastReason = reason
    guard updated else {
      return false
    }
    if sourceChanged {
      rendererOwnedFlutterSurfaceSourceChangeCount += 1
    }
    rendererOwnedFlutterSurfaceLastGeneration = generation
    rendererOwnedFlutterSurfaceLastSourceKey = sourceKey
    rendererOwnedFlutterSurfacePublishCount += 1
    let nowNs = DispatchTime.now().uptimeNanoseconds
    if nowNs > rendererOwnedFlutterSurfaceLastPublishLogNs + 1_000_000_000 {
      rendererOwnedFlutterSurfaceLastPublishLogNs = nowNs
      NSLog(
        "VoidPlayer renderer-owned presentation: published Flutter surface reason=%@ generation=%llu sourceKey=%llu sourceChanged=%@ size=%dx%d format=%llu sample=%d publish=%d unchanged=%d sourceChanges=%d continuousComposes=%d sampleP95Ms=%.3f",
        reason,
        generation,
        sourceKey,
        sourceChanged ? "true" : "false",
        texture.width,
        texture.height,
        UInt64(texture.pixelFormat.rawValue),
        rendererOwnedFlutterSurfaceSampleCount,
        rendererOwnedFlutterSurfacePublishCount,
        rendererOwnedFlutterSurfaceUnchangedCount,
        rendererOwnedFlutterSurfaceSourceChangeCount,
        rendererOwnedFlutterSurfaceContinuousComposeCount,
        rendererOwnedFlutterSurfaceSampleDuration.p95Ms()
      )
    }
    return true
  }

  private func publishFlutterSurfaceToNativeRenderer(
    player: MacOSNativePlayerSession,
    texture: MTLTexture,
    frameGeneration: UInt64
  ) -> Bool {
    player.updateExternalFlutterSurface(
      texture: texture,
      frameGeneration: frameGeneration
    )
  }

  private func flutterSurfaceSourceKey(info: [String: Any], texture: MTLTexture) -> UInt64 {
    if let ioSurfaceId = info["ioSurfaceId"] as? UInt64 {
      return ioSurfaceId
    }
    if let ioSurfaceId = info["ioSurfaceId"] as? Int {
      return UInt64(max(0, ioSurfaceId))
    }
    if let texturePointer = info["texturePointer"] as? UInt64 {
      return texturePointer
    }
    if let texturePointer = info["texturePointer"] as? Int {
      return UInt64(max(0, texturePointer))
    }
    return UInt64(UInt(bitPattern: Unmanaged.passUnretained(texture as AnyObject).toOpaque()))
  }

  private func ensureRendererOwnedPresentationMatchesCurrentConfiguration() {
    guard textureId != nil,
          texture != nil else {
      nativePlayer?.clearSourceProjection()
      emitRendererOwnedPresentationState()
      return
    }
    markRendererOwnedFlutterSurfaceDirty(reason: "renderer-owned-presentation-configured")
    emitRendererOwnedPresentationState()
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
    let targetWasRebuilt = rebuildRendererTargetIfNeeded(for: nextConfiguration)
    if let rendererTarget, !targetWasRebuilt {
      let pixelFormatChanged = rendererTarget.setRendererTargetPixelFormat(
        nextConfiguration.rendererTargetPixelFormat,
        player: nativePlayer
      )
      if pixelFormatChanged {
        playback.setTargetInstalled(false)
      }
    }
    if targetWasRebuilt {
      playback.setTargetInstalled(false)
      presentation.applyRendererOwnedViewportRect(to: rendererTarget)
      if rendererTarget?.rendererOwnedRunnerLayerActive != true {
        _ = presentation.refreshCurrentFrame(context: presentationContext())
      }
    }
    ensureRendererOwnedPresentationMatchesCurrentConfiguration()
  }

  @discardableResult
  private func rebuildRendererTargetIfNeeded(
    for configuration: MacOSPresentationConfiguration
  ) -> Bool {
    guard backendName == MacOSVideoTrackPayload.nativeFormatName,
          textureId != nil,
          let texture else {
      return false
    }
    let exporterAvailable = MacOSFlutterSurfaceExporter.isAvailable(engine: flutterEngine)
    let wantsLayer = configuration.edrOutputEnabled && exporterAvailable
    let hasLayer = rendererTarget?.rendererOwnedRunnerLayerActive == true
    guard wantsLayer != hasLayer else {
      return false
    }
    if wantsLayer {
      let dimensions = texture.dimensions()
      guard let layerTarget = MacOSRendererOwnedLayerTarget(
        nativeWidth: max(16, dimensions.width),
        nativeHeight: max(16, dimensions.height),
        contentView: contentView
      ) else {
        return false
      }
      lifecycle.replaceRendererTarget(layerTarget)
      return true
    }
    lifecycle.replaceRendererTarget(texture as? MacOSRendererOwnedPresentationTarget)
    nativePlayer?.clearExternalFlutterSurface()
    return true
  }

  private func emitRendererOwnedPresentationState() {
    let configuration = MacOSPresentationConfiguration.current
    let rendererOwnedReady = rendererOwnedPresentationReady()
    let active = rendererOwnedReady
    let failure = active
      ? ""
      : "renderer-owned presentation is not ready"
    nativeEvents.emitRendererOwnedPresentationState(
      active: active,
      runnerLayerActive: active && (rendererTarget?.rendererOwnedRunnerLayerActive == true),
      rendererOwnedActive: rendererOwnedReady,
      requested: configuration.rendererOwnedPresentationEnabled,
      edrEnabled: configuration.edrOutputEnabled,
      mode: configuration.mode.rawValue,
      reason: configuration.reason,
      failure: failure
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

  private func rendererOwnedPresentationReady() -> Bool {
    if rendererTarget?.rendererOwnedRunnerLayerActive == true {
      return nativePlayer != nil &&
        MacOSPresentationConfiguration.current.rendererOwnedPresentationEnabled &&
        MacOSFlutterSurfaceExporter.isAvailable(engine: flutterEngine)
    }
    let state = nativePlayer?.rendererOwnedPresentationState() ?? [:]
    return (state["rendererInitialized"] as? Bool ?? false) &&
      (state["targetInstalled"] as? Bool ?? false) &&
      (state["backendAvailable"] as? Bool ?? false)
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
      player: nativePlayer,
      texture: texture,
      rendererTarget: rendererTarget,
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
      rendererTarget: rendererTarget,
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
        callbackContext: callbackContext,
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

  func scheduleNativeSourceCacheFrameAvailableFromCallback(
    callbackGeneration: UInt64,
    callbackContext: MacOSNativeFrameCallbackContext
  ) {
    DispatchQueue.main.async { [weak self, weak callbackContext] in
      guard let self else { return }
      guard callbackContext?.isCurrent(callbackGeneration) == true else {
        return
      }
      self.rendererOwnedSourceCachePublishRate.record()
      self.scheduleRendererOwnedCompositeRefresh(reason: "source-cache-publish")
    }
  }

  private func processNativeFrameCallback(
    enqueueNs: UInt64,
    callbackGeneration: UInt64,
    callbackContext: MacOSNativeFrameCallbackContext?,
    immediateDepth: Int
  ) {
    guard callbackContext?.isCurrent(callbackGeneration) == true else {
      _ = frameCallbackProfiler.finishProcessing(
        endNs: DispatchTime.now().uptimeNanoseconds
      )
      return
    }
    let startNs = DispatchTime.now().uptimeNanoseconds
    let nativePlaying = playback.syncPlayingState(player: nativePlayer)
    if nativePlaying {
      emitPlaybackClock()
    }
    frameCallbackProfiler.recordMainStart(enqueueNs: enqueueNs, startNs: startNs)
    if let generation = currentRendererOwnedTargetGeneration() {
      frameCallbackProfiler.recordTargetGeneration(generation, nowNs: startNs)
    }
    let suppressLayoutPublication =
      backendName == MacOSVideoTrackPayload.nativeFormatName &&
      presentation.shouldSuppressNativeCallbackPublicationDuringLayout()
    var publishedFromCallback = false
    if suppressLayoutPublication {
      presentation.recordLayoutCallbackPublicationSuppressed()
    } else {
      publishedFromCallback = playback.handleFrameCallback(
        player: nativePlayer,
        rendererTarget: rendererTarget,
        maxTrackSlots: tracks.activeSlotCapacity(),
        nativeBackendActive: backendName == MacOSVideoTrackPayload.nativeFormatName,
        presentationState: presentationState,
        markFrameAvailable: {
          self.markFrameAvailable()
          self.transport.resolvePendingSeekPreviewIfPresented(
            presentationState: self.presentationState,
            emitSeekPreviewPresented: { [weak self] requestId, targetPtsUs in
              self?.emitSeekPreviewPresented(requestId: requestId, targetPtsUs: targetPtsUs)
            }
          )
        }
      )
    }
    if publishedFromCallback {
      finishRendererOwnedCompositeRefreshFromCallback()
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
        if !nativePlaying {
          _ = self.playback.syncPlayingState(player: self.nativePlayer)
        }
        self.processNativeFrameCallback(
          enqueueNs: nextEnqueueNs,
          callbackGeneration: callbackGeneration,
          callbackContext: callbackContext,
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
    let compositorDiagnostics = compositorLatencyProfiler.diagnosticMap()
    let textureStats = texture?.diagnostics()
    let textureDiagnostics: [String: Any] = [
      "rendererTargetRebuildCount": textureStats?.rebuildCount ?? 0,
      "rendererTargetAllocationCount": textureStats?.allocationCount ?? 0,
      "rendererTargetRebuildReuseCount": textureStats?.rebuildReuseCount ?? 0,
      "rendererTargetRebuildLastAllocatedCount": textureStats?.rebuildLastAllocatedCount ?? 0,
      "rendererTargetRebuildLastReusedCount": textureStats?.rebuildLastReusedCount ?? 0,
      "rendererTargetRebuildLastDurationMs": textureStats?.rebuildLastDurationMs ?? 0.0,
      "rendererTargetRetiredCount": textureStats?.retiredCount ?? 0,
      "rendererTargetPrewarmRequestCount": textureStats?.prewarmRequestCount ?? 0,
      "rendererTargetPrewarmHitCount": textureStats?.prewarmHitCount ?? 0,
      "rendererTargetPrewarmReadyCount": textureStats?.prewarmReadyCount ?? 0,
      "rendererTargetPrewarmDroppedCount": textureStats?.prewarmDroppedCount ?? 0,
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
      compositor: compositorDiagnostics
    )
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    nativeEvents.onListen(events)
    emitRendererOwnedPresentationState()
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    nativeEvents.onCancel()
    return nil
  }

}
