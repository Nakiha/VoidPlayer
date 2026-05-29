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
  private let pausedLayoutRefreshIntervalNs: UInt64 = 16_000_000
  private(set) var layout: [String: Any] = MacOSVideoTrackPayload.defaultLayout()
  private let layoutRefreshQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.layout-refresh",
    qos: .userInteractive
  )
  private var layoutRefreshGeneration = 0
  private var layoutRefreshRunning = false
  private var latestLayoutRefreshRequest: LayoutRefreshRequest?
  private var layoutRequestCount = 0
  private var layoutImmediateCount = 0
  private var layoutQueuedCount = 0
  private var lastPausedLayoutRefreshStartNs: UInt64 = 0

  func resetLayout() {
    cancelPendingLayoutRefreshes()
    layout = MacOSVideoTrackPayload.defaultLayout()
  }

  func applyLayout(arguments: Any?, context: MacOSPresentationContext) {
    guard let nextLayout = MacOSNativeLayoutBridge.layoutMap(arguments: arguments) else {
      return
    }
    layoutRequestCount += 1
    layout = nextLayout
    if context.nativeBackendActive,
       context.playback.currentIsPlaying(player: context.player) {
      invalidatePendingLayoutRefreshes()
      let startNs = DispatchTime.now().uptimeNanoseconds
      MacOSNativeLayoutBridge.apply(layout: nextLayout, player: context.player)
      layoutImmediateCount += 1
      logLayoutProfiler(
        route: "playing-immediate",
        generation: layoutRefreshGeneration,
        requestNs: startNs,
        queueDelayNs: 0,
        applyNs: DispatchTime.now().uptimeNanoseconds - startNs,
        outcome: "applied"
      )
      return
    }
    requestCoalescedLayoutRefresh(context: context, layout: nextLayout)
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

  private func cancelPendingLayoutRefreshes() {
    layoutRefreshGeneration += 1
    latestLayoutRefreshRequest = nil
    lastPausedLayoutRefreshStartNs = 0
    if layoutRefreshRunning {
      layoutRefreshQueue.sync {}
    }
    layoutRefreshRunning = false
  }

  private func invalidatePendingLayoutRefreshes() {
    layoutRefreshGeneration += 1
    latestLayoutRefreshRequest = nil
    lastPausedLayoutRefreshStartNs = 0
  }

  private func requestCoalescedLayoutRefresh(
    context: MacOSPresentationContext,
    layout: [String: Any]
  ) {
    guard context.nativeBackendActive,
          context.player != nil,
          context.nativeTexture != nil else {
      context.markFrameAvailable()
      return
    }
    layoutRefreshGeneration += 1
    layoutQueuedCount += 1
    latestLayoutRefreshRequest = LayoutRefreshRequest(context: context, layout: layout)
    guard !layoutRefreshRunning else { return }
    layoutRefreshRunning = true
    runLatestLayoutRefresh(generation: layoutRefreshGeneration)
  }

  private func runLatestLayoutRefresh(generation: Int) {
    guard let request = latestLayoutRefreshRequest else {
      layoutRefreshRunning = false
      return
    }
    let nowNs = DispatchTime.now().uptimeNanoseconds
    if lastPausedLayoutRefreshStartNs > 0,
       nowNs > lastPausedLayoutRefreshStartNs,
       nowNs - lastPausedLayoutRefreshStartNs < pausedLayoutRefreshIntervalNs {
      let delayNs = pausedLayoutRefreshIntervalNs - (nowNs - lastPausedLayoutRefreshStartNs)
      DispatchQueue.main.asyncAfter(deadline: .now() + .nanoseconds(Int(delayNs))) {
        [weak self] in
        guard let self else { return }
        self.runLatestLayoutRefresh(generation: self.layoutRefreshGeneration)
      }
      return
    }
    latestLayoutRefreshRequest = nil
    lastPausedLayoutRefreshStartNs = nowNs
    layoutRefreshQueue.async { [weak self, request, generation] in
      let startNs = DispatchTime.now().uptimeNanoseconds
      let outcome = Self.performLayoutRefresh(request: request)
      let finishNs = DispatchTime.now().uptimeNanoseconds
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        self.logLayoutProfiler(
          route: "paused-coalesced",
          generation: generation,
          requestNs: request.requestNs,
          queueDelayNs: startNs >= request.requestNs ? startNs - request.requestNs : 0,
          applyNs: finishNs >= startNs ? finishNs - startNs : 0,
          outcome: outcome.profilerName
        )
        if generation == self.layoutRefreshGeneration {
          switch outcome {
          case .applied:
            break
          case .transientMiss:
            request.context.presentationState.recordMiss()
          }
        }
        if self.latestLayoutRefreshRequest != nil {
          self.runLatestLayoutRefresh(generation: self.layoutRefreshGeneration)
        } else {
          self.layoutRefreshRunning = false
        }
      }
    }
  }

  private static func performLayoutRefresh(
    request: LayoutRefreshRequest
  ) -> LayoutRefreshOutcome {
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
    MacOSNativeLayoutBridge.apply(layout: request.layout, player: player)
    return .applied
  }

  private func logLayoutProfiler(
    route: String,
    generation: Int,
    requestNs: UInt64,
    queueDelayNs: UInt64,
    applyNs: UInt64,
    outcome: String
  ) {
    let totalNs = DispatchTime.now().uptimeNanoseconds - requestNs
    let slow = totalNs >= 12_000_000 || queueDelayNs >= 8_000_000 || applyNs >= 8_000_000
    let periodic = layoutRequestCount > 0 && layoutRequestCount % 120 == 0
    guard slow || periodic else { return }
    MacOSProfilerLog.log(String(
      format: "VoidPlayer macOS layout profiler route=%@ outcome=%@ gen=%d requests=%d immediate=%d queued=%d totalMs=%.2f queueMs=%.2f applyMs=%.2f playing=%d",
      route,
      outcome,
      generation,
      layoutRequestCount,
      layoutImmediateCount,
      layoutQueuedCount,
      Self.ms(totalNs),
      Self.ms(queueDelayNs),
      Self.ms(applyNs),
      route == "playing-immediate" ? 1 : 0
    ))
  }

  private static func ms(_ ns: UInt64) -> Double {
    Double(ns) / 1_000_000.0
  }
}

private struct LayoutRefreshRequest {
  let context: MacOSPresentationContext
  let layout: [String: Any]
  let requestNs = DispatchTime.now().uptimeNanoseconds
}

private enum LayoutRefreshOutcome {
  case applied
  case transientMiss

  var profilerName: String {
    switch self {
    case .applied:
      return "applied"
    case .transientMiss:
      return "transient-miss"
    }
  }
}
