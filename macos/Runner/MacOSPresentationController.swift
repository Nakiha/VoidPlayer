import Cocoa
import CoreVideo
import Foundation

struct MacOSPresentationContext {
  let nativeBackendActive: Bool
  let player: MacOSNativePlayerSession?
  let texture: MacOSVideoTexture?
  let nativeTexture: MacOSFlutterTextureBridge?
  let maxTrackSlots: Int
  let playback: MacOSPlaybackController
  let presentationState: MacOSFramePresentationState
  let userData: UnsafeMutableRawPointer
  let markFrameAvailable: () -> Void
}

final class MacOSPresentationController {
  private(set) var layout: [String: Any] = MacOSVideoTrackPayload.defaultLayout()
  private let layoutRefreshQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.layout-refresh",
    qos: .userInteractive
  )
  private lazy var displayLink = MacOSViewportDisplayLink { [weak self] in
    self?.processViewportDisplayTick()
  }
  private let layoutRevisionLock = NSLock()
  private var layoutRefreshRunning = false
  private var latestLayoutRefreshRequest: LayoutRefreshRequest?
  private var latestLayoutRevision: UInt64 = 0
  private var layoutIntentCount = 0
  private var layoutSubmitCount = 0
  private var layoutDrawCount = 0
  private var layoutSkipCount = 0
  private var layoutStaleDropCount = 0
  private var layoutStaleAfterDrawDropCount = 0
  private var layoutPublishedCount = 0
  private var layoutRefreshSupersededCount = 0
  private var layoutCallbackPublicationSuppressedCount = 0
  private var layoutDeferredToPlaybackCount = 0
  private var displayLinkIdleUntilNs: UInt64 = 0
  private let displayLinkIdleGraceNs: UInt64 = 250_000_000
  private let layoutIntentRate = MacOSRateWindow()
  private let layoutSubmitRate = MacOSRateWindow()
  private let layoutDrawRate = MacOSRateWindow()
  private let layoutSkipRate = MacOSRateWindow()
  private let layoutQueueDuration = MacOSDurationWindow()
  private let layoutApplyDuration = MacOSDurationWindow()
  private let layoutTotalDuration = MacOSDurationWindow()

  func resetLayout() {
    cancelPendingLayoutRefreshes()
    layout = MacOSVideoTrackPayload.defaultLayout()
  }

  func applyLayout(arguments: Any?, context: MacOSPresentationContext) {
    guard let nextLayout = MacOSNativeLayoutBridge.layoutMap(arguments: arguments) else {
      return
    }
    layoutIntentCount += 1
    layoutIntentRate.record()
    let revision = nextLayoutRevision()
    layout = nextLayout
    logLayoutTrace(
      event: "intent",
      revision: revision,
      layout: nextLayout,
      outcome: "queued"
    )
    requestDisplayLinkedLayoutRefresh(
      context: context,
      layout: nextLayout,
      revision: revision
    )
  }

  func resize(arguments: Any?, context: MacOSPresentationContext) {
    var nativeRefreshAttempted = false
    let width = MacOSFlutterArguments.intArg(arguments, "width")
    let height = MacOSFlutterArguments.intArg(arguments, "height")
    if let width, let height {
      let nextWidth = max(16, width)
      let nextHeight = max(16, height)
      let currentDimensions = context.texture?.dimensions()
      let willChange = currentDimensions?.width != nextWidth ||
        currentDimensions?.height != nextHeight
      if context.nativeBackendActive, willChange {
        context.player?.clearMetalPresentationTarget()
      }
      _ = context.texture?.resize(width: nextWidth, height: nextHeight) ?? false
      if context.nativeBackendActive {
        nativeRefreshAttempted = true
        let refreshed = refreshCurrentFrame(context: context)
        context.playback.reinstallPresentationTargetIfPlaying(
          player: context.player,
          texture: context.nativeTexture,
          maxTrackSlots: context.maxTrackSlots
        )
        if !refreshed {
          return
        }
      }
    }
    if !nativeRefreshAttempted {
      context.markFrameAvailable()
    }
  }

  @discardableResult
  func refreshCurrentFrame(context: MacOSPresentationContext) -> Bool {
    guard context.nativeBackendActive,
          let player = context.player,
          let texture = context.nativeTexture else {
      context.markFrameAvailable()
      return true
    }
    if context.playback.currentIsPlaying(player: player) {
      context.playback.reinstallPresentationTargetIfPlaying(
        player: player,
        texture: texture,
        maxTrackSlots: context.maxTrackSlots
      )
      return true
    }
    let refreshed = MacOSNativeFrameRefresh.refreshCurrentFrameAfterLayoutChange(
      player: player,
      texture: texture,
      maxTrackSlots: context.maxTrackSlots,
      presentationState: context.presentationState,
      framePump: context.playback.framePumpForRefresh
    )
    if refreshed {
      context.markFrameAvailable()
    }
    return refreshed
  }

  func diagnosticMap() -> [String: Any] {
    var diagnostics = displayLink.diagnosticMap()
    diagnostics["layoutIntentCount"] = layoutIntentCount
    diagnostics["layoutSubmitCount"] = layoutSubmitCount
    diagnostics["layoutDrawCount"] = layoutDrawCount
    diagnostics["layoutSkipCount"] = layoutSkipCount
    diagnostics["layoutStaleDropCount"] = layoutStaleDropCount
    diagnostics["layoutStaleAfterDrawDropCount"] = layoutStaleAfterDrawDropCount
    diagnostics["layoutPublishedCount"] = layoutPublishedCount
    diagnostics["layoutRefreshSupersededCount"] = layoutRefreshSupersededCount
    diagnostics["layoutCallbackPublicationSuppressedCount"] =
      layoutCallbackPublicationSuppressedCount
    diagnostics["viewportLayoutDeferredToPlaybackCount"] = layoutDeferredToPlaybackCount
    diagnostics["viewportClockWarm"] = displayLink.isRunning
    diagnostics["displayIdleGraceMs"] = Int(displayLinkIdleGraceNs / 1_000_000)
    let intentHz = layoutIntentRate.rateHz()
    let submitHz = layoutSubmitRate.rateHz()
    let drawHz = layoutDrawRate.rateHz()
    let skipHz = layoutSkipRate.rateHz()
    diagnostics["layoutIntentHz"] = intentHz
    diagnostics["layoutIntentHzX1000"] = Int(intentHz * 1000.0)
    diagnostics["layoutSubmitHz"] = submitHz
    diagnostics["layoutSubmitHzX1000"] = Int(submitHz * 1000.0)
    diagnostics["layoutDrawHz"] = drawHz
    diagnostics["layoutDrawHzX1000"] = Int(drawHz * 1000.0)
    diagnostics["layoutSkipHz"] = skipHz
    diagnostics["layoutSkipHzX1000"] = Int(skipHz * 1000.0)
    diagnostics["layoutRefreshQueueP95Ms"] = layoutQueueDuration.p95Ms()
    diagnostics["layoutRefreshApplyP95Ms"] = layoutApplyDuration.p95Ms()
    diagnostics["layoutRefreshTotalP95Ms"] = layoutTotalDuration.p95Ms()
    diagnostics["layoutRefreshTotalLastMs"] = layoutTotalDuration.lastMs()
    diagnostics["layoutRefreshRunning"] = layoutRefreshRunning
    diagnostics["layoutIntentPending"] = latestLayoutRefreshRequest != nil
    return diagnostics
  }

  func shouldSuppressNativeCallbackPublicationDuringLayout() -> Bool {
    // Layout-owned refreshes are published through the revision gate. Native
    // callbacks that arrive while a layout refresh is active are only
    // diagnostic signals; publishing them here would bypass stale-revision
    // checks and can double-drive Flutter texture notifications during pan/zoom.
    layoutRefreshRunning || latestLayoutRefreshRequest != nil
  }

  func recordLayoutCallbackPublicationSuppressed() {
    layoutCallbackPublicationSuppressedCount += 1
  }

  func cancelPendingLayoutRefreshes() {
    invalidateLayoutRevision()
    latestLayoutRefreshRequest = nil
    displayLinkIdleUntilNs = 0
    displayLink.stop()
    if layoutRefreshRunning {
      layoutRefreshQueue.sync {}
    }
    layoutRefreshRunning = false
  }

  private func requestDisplayLinkedLayoutRefresh(
    context: MacOSPresentationContext,
    layout: [String: Any],
    revision: UInt64
  ) {
    guard context.nativeBackendActive,
          context.player != nil,
          context.nativeTexture != nil else {
      context.markFrameAvailable()
      return
    }
    if latestLayoutRefreshRequest != nil || layoutRefreshRunning {
      layoutRefreshSupersededCount += 1
    }
    latestLayoutRefreshRequest = LayoutRefreshRequest(
      context: context,
      layout: layout,
      revision: revision
    )
    extendDisplayLinkIdleGrace()
    logLayoutTrace(
      event: "display-link-pending",
      revision: revision,
      layout: layout,
      outcome: "latest"
    )
    displayLink.start()
  }

  private func processViewportDisplayTick() {
    guard let request = latestLayoutRefreshRequest else {
      layoutSkipCount += 1
      layoutSkipRate.record()
      if !layoutRefreshRunning && !shouldKeepDisplayLinkWarm() {
        displayLink.stop()
      }
      return
    }
    guard !layoutRefreshRunning else {
      layoutSkipCount += 1
      layoutSkipRate.record()
      return
    }
    latestLayoutRefreshRequest = nil
    layoutRefreshRunning = true
    layoutSubmitCount += 1
    layoutSubmitRate.record()
    logLayoutTrace(
      event: "submit",
      revision: request.revision,
      layout: request.layout,
      outcome: "start"
    )
    layoutRefreshQueue.async { [weak self, request] in
      guard let self else { return }
      let startNs = DispatchTime.now().uptimeNanoseconds
      let outcome = self.performLayoutRefresh(request: request)
      let finishNs = DispatchTime.now().uptimeNanoseconds
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        var finalOutcomeName = outcome.profilerName
        switch outcome {
        case .ready(let pending):
          guard self.isCurrentLayoutRequest(request) else {
            request.context.nativeTexture?.discardPendingNativeFrame(pending)
            self.layoutStaleAfterDrawDropCount += 1
            finalOutcomeName = LayoutRefreshOutcome.staleAfterDraw.profilerName
            break
          }
          guard let player = request.context.player,
                let texture = request.context.nativeTexture else {
            request.context.presentationState.recordMiss()
            finalOutcomeName = LayoutRefreshOutcome.transientMiss.profilerName
            break
          }
          if MacOSNativeFrameRefresh.publishLayoutRefreshFrame(
            pending,
            player: player,
            texture: texture,
            maxTrackSlots: request.context.maxTrackSlots,
            presentationState: request.context.presentationState,
            framePump: request.context.playback.framePumpForRefresh
          ) {
            request.context.markFrameAvailable()
            self.layoutDrawCount += 1
            self.layoutPublishedCount += 1
            self.layoutDrawRate.record()
            finalOutcomeName = "applied"
          } else {
            request.context.presentationState.recordMiss()
            finalOutcomeName = LayoutRefreshOutcome.transientMiss.profilerName
          }
        case .deferredToPlayback:
          self.layoutDeferredToPlaybackCount += 1
        case .stale:
          self.layoutStaleDropCount += 1
        case .staleAfterDraw:
          self.layoutStaleAfterDrawDropCount += 1
        case .transientMiss:
          request.context.presentationState.recordMiss()
        case .coalesced:
          self.layoutSkipCount += 1
          self.layoutSkipRate.record()
        }
        self.logLayoutTrace(
          event: "complete",
          revision: request.revision,
          layout: request.layout,
          outcome: finalOutcomeName
        )
        self.layoutRefreshRunning = false
        let queueDelayNs = startNs >= request.requestNs ? startNs - request.requestNs : 0
        let applyNs = finishNs >= startNs ? finishNs - startNs : 0
        let totalNs = DispatchTime.now().uptimeNanoseconds - request.requestNs
        self.layoutQueueDuration.record(queueDelayNs)
        self.layoutApplyDuration.record(applyNs)
        self.layoutTotalDuration.record(totalNs)
        self.logLayoutProfiler(
          route: "display-link-layout",
          requestNs: request.requestNs,
          queueDelayNs: queueDelayNs,
          applyNs: applyNs,
          outcome: finalOutcomeName
        )
        if self.latestLayoutRefreshRequest != nil {
          self.extendDisplayLinkIdleGrace()
          self.displayLink.start()
        } else if !self.shouldKeepDisplayLinkWarm() {
          self.displayLink.stop()
        }
      }
    }
  }

  private func performLayoutRefresh(
    request: LayoutRefreshRequest
  ) -> LayoutRefreshOutcome {
    guard isCurrentLayoutRequest(request) else {
      return .stale
    }
    let context = request.context
    guard let player = context.player else {
      return .transientMiss
    }
    let pumpReady = context.playback.ensurePresentationPump(
      player: player,
      texture: context.nativeTexture,
      maxTrackSlots: context.maxTrackSlots,
      userData: context.userData,
      presentationState: context.presentationState
    )
    guard pumpReady else {
      return .transientMiss
    }
    guard isCurrentLayoutRequest(request) else {
      return .stale
    }
    MacOSNativeLayoutBridge.apply(layout: request.layout, player: player)
    guard let texture = context.nativeTexture else {
      return .transientMiss
    }
    let drawResult = MacOSNativeFrameRefresh.drawCurrentFrameForLayoutRefresh(
      player: player,
      texture: texture,
      maxTrackSlots: context.maxTrackSlots
    )
    let pendingFrame: MacOSPendingNativeFrame
    switch drawResult {
    case .ready(let pending):
      pendingFrame = pending
    case .coalesced:
      return .coalesced
    case .failed:
      return .transientMiss
    }
    guard isCurrentLayoutRequest(request) else {
      texture.discardPendingNativeFrame(pendingFrame)
      return .staleAfterDraw
    }
    return .ready(pendingFrame)
  }

  private func extendDisplayLinkIdleGrace() {
    displayLinkIdleUntilNs = DispatchTime.now().uptimeNanoseconds + displayLinkIdleGraceNs
  }

  private func shouldKeepDisplayLinkWarm() -> Bool {
    DispatchTime.now().uptimeNanoseconds < displayLinkIdleUntilNs
  }

  private func nextLayoutRevision() -> UInt64 {
    layoutRevisionLock.lock()
    latestLayoutRevision &+= 1
    let revision = latestLayoutRevision
    layoutRevisionLock.unlock()
    return revision
  }

  private func invalidateLayoutRevision() {
    layoutRevisionLock.lock()
    latestLayoutRevision &+= 1
    layoutRevisionLock.unlock()
  }

  private func isCurrentLayoutRequest(_ request: LayoutRefreshRequest) -> Bool {
    layoutRevisionLock.lock()
    let current = latestLayoutRevision
    layoutRevisionLock.unlock()
    return request.revision == current
  }

  private func logLayoutTrace(
    event: String,
    revision: UInt64,
    layout: [String: Any],
    outcome: String
  ) {
    let zoom = MacOSFlutterArguments.doubleValue(layout["zoomRatio"]) ?? 1.0
    let offsetX = MacOSFlutterArguments.doubleValue(layout["viewOffsetX"]) ?? 0.0
    let offsetY = MacOSFlutterArguments.doubleValue(layout["viewOffsetY"]) ?? 0.0
    let mode = MacOSFlutterArguments.intValue(layout["mode"]) ?? 0
    MacOSProfilerLog.trace(String(
      format: "VoidPlayer viewport trace swift event=%@ outcome=%@ revision=%llu intent=%d submit=%d draw=%d skip=%d stale=%d running=%d pending=%d source=%@ hz=%.2f mode=%d zoom=%.4f offset=(%.1f,%.1f)",
      event,
      outcome,
      revision,
      layoutIntentCount,
      layoutSubmitCount,
      layoutDrawCount,
      layoutSkipCount,
      layoutStaleDropCount + layoutStaleAfterDrawDropCount,
      layoutRefreshRunning ? 1 : 0,
      latestLayoutRefreshRequest != nil ? 1 : 0,
      displayLink.clockSource,
      displayLink.refreshHzEstimate,
      mode,
      zoom,
      offsetX,
      offsetY
    ))
  }

  private func logLayoutProfiler(
    route: String,
    requestNs: UInt64,
    queueDelayNs: UInt64,
    applyNs: UInt64,
    outcome: String
  ) {
    let totalNs = DispatchTime.now().uptimeNanoseconds - requestNs
    let slow = totalNs >= 12_000_000 || queueDelayNs >= 8_000_000 || applyNs >= 8_000_000
    let periodic = layoutIntentCount > 0 && layoutIntentCount % 120 == 0
    guard slow || periodic else { return }
    MacOSProfilerLog.log(String(
      format: "VoidPlayer macOS layout profiler route=%@ outcome=%@ intents=%d submits=%d draws=%d skips=%d stale=%d totalMs=%.2f queueMs=%.2f applyMs=%.2f source=%@ hz=%.1f",
      route,
      outcome,
      layoutIntentCount,
      layoutSubmitCount,
      layoutDrawCount,
      layoutSkipCount,
      layoutStaleDropCount + layoutStaleAfterDrawDropCount,
      Self.ms(totalNs),
      Self.ms(queueDelayNs),
      Self.ms(applyNs),
      displayLink.clockSource,
      displayLink.refreshHzEstimate
    ))
  }

  private static func ms(_ ns: UInt64) -> Double {
    Double(ns) / 1_000_000.0
  }
}

private struct LayoutRefreshRequest {
  let context: MacOSPresentationContext
  let layout: [String: Any]
  let revision: UInt64
  let requestNs = DispatchTime.now().uptimeNanoseconds
}

private enum LayoutRefreshOutcome {
  case ready(MacOSPendingNativeFrame)
  case deferredToPlayback
  case stale
  case staleAfterDraw
  case transientMiss
  case coalesced

  var profilerName: String {
    switch self {
    case .ready:
      return "ready"
    case .deferredToPlayback:
      return "deferred-to-playback"
    case .stale:
      return "stale"
    case .staleAfterDraw:
      return "stale-after-draw"
    case .transientMiss:
      return "transient-miss"
    case .coalesced:
      return "coalesced"
    }
  }
}
