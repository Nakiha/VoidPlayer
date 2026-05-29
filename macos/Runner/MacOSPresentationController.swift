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

  private func cancelPendingLayoutRefreshes() {
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
        switch outcome {
        case .applied:
          self.layoutDrawCount += 1
          self.layoutDrawRate.record()
        case .deferredToPlayback:
          self.layoutDeferredToPlaybackCount += 1
        case .stale:
          self.layoutStaleDropCount += 1
        case .transientMiss:
          request.context.presentationState.recordMiss()
        }
        self.logLayoutTrace(
          event: "complete",
          revision: request.revision,
          layout: request.layout,
          outcome: outcome.profilerName
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
          outcome: outcome.profilerName
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
    if context.playback.currentIsPlaying(player: player) {
      return .deferredToPlayback
    }
    return .applied
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
      layoutStaleDropCount,
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
      layoutStaleDropCount,
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
  case applied
  case deferredToPlayback
  case stale
  case transientMiss

  var profilerName: String {
    switch self {
    case .applied:
      return "applied"
    case .deferredToPlayback:
      return "deferred-to-playback"
    case .stale:
      return "stale"
    case .transientMiss:
      return "transient-miss"
    }
  }
}

private final class MacOSViewportDisplayLink {
  private let lock = NSLock()
  private let onTick: () -> Void
  private var displayLink: CVDisplayLink?
  private var fallbackTimer: DispatchSourceTimer?
  private var running = false
  private var mainTickScheduled = false
  private var targetDisplayId: CGDirectDisplayID = 0
  private var source = "stopped"
  private var tickCount = 0
  private var deliveredTickCount = 0
  private let tickRate = MacOSRateWindow()
  private let deliveredTickRate = MacOSRateWindow()
  private var lastCallbackNs: UInt64 = 0
  private var refreshHz = 0.0
  private var intervalsNs: [UInt64] = []

  var clockSource: String {
    lock.lock()
    defer { lock.unlock() }
    return source
  }

  var refreshHzEstimate: Double {
    lock.lock()
    defer { lock.unlock() }
    return refreshHz
  }

  var isRunning: Bool {
    lock.lock()
    defer { lock.unlock() }
    return running
  }

  init(onTick: @escaping () -> Void) {
    self.onTick = onTick
  }

  deinit {
    stop()
  }

  func start() {
    lock.lock()
    let displayId = Self.currentDisplayId()
    let needsRecreate = displayLink == nil || displayId != targetDisplayId
    lock.unlock()
    if needsRecreate {
      recreateDisplayLink(displayId: displayId)
    }

    lock.lock()
    if running {
      lock.unlock()
      return
    }
    if let displayLink {
      let status = CVDisplayLinkStart(displayLink)
      running = status == kCVReturnSuccess
      source = running ? "cvdisplaylink" : "fallback-timer"
      lock.unlock()
      if status != kCVReturnSuccess {
        startFallbackTimer()
      }
      return
    }
    source = "fallback-timer"
    lock.unlock()
    startFallbackTimer()
  }

  func stop() {
    lock.lock()
    running = false
    mainTickScheduled = false
    let link = displayLink
    let timer = fallbackTimer
    fallbackTimer = nil
    lock.unlock()
    if let link, CVDisplayLinkIsRunning(link) {
      CVDisplayLinkStop(link)
    }
    timer?.cancel()
  }

  func diagnosticMap() -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }
    return [
      "viewportClockSource": source,
      "viewportClockRunning": running,
      "displayRefreshHzEstimate": refreshHz,
      "displayRefreshHzEstimateX1000": Int(refreshHz * 1000.0),
      "displayTickCount": tickCount,
      "displayDeliveredTickCount": deliveredTickCount,
      "displayTickHz": tickRate.rateHz(),
      "displayTickHzX1000": Int(tickRate.rateHz() * 1000.0),
      "displayDeliveredTickHz": deliveredTickRate.rateHz(),
      "displayDeliveredTickHzX1000": Int(deliveredTickRate.rateHz() * 1000.0),
      "viewportTickP95Ms": intervalP95MsLocked(),
    ]
  }

  private func recreateDisplayLink(displayId: CGDirectDisplayID) {
    lock.lock()
    let oldLink = displayLink
    displayLink = nil
    targetDisplayId = displayId
    lock.unlock()
    if let oldLink, CVDisplayLinkIsRunning(oldLink) {
      CVDisplayLinkStop(oldLink)
    }

    var newLink: CVDisplayLink?
    let status = CVDisplayLinkCreateWithCGDisplay(displayId, &newLink)
    guard status == kCVReturnSuccess, let created = newLink else {
      lock.lock()
      source = "fallback-timer"
      refreshHz = Self.fallbackRefreshHz()
      lock.unlock()
      return
    }
    CVDisplayLinkSetOutputCallback(
      created,
      MacOSViewportDisplayLink.displayLinkCallback,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
    )
    lock.lock()
    displayLink = created
    source = "cvdisplaylink"
    refreshHz = Self.nominalRefreshHz(displayLink: created)
    lock.unlock()
  }

  private func startFallbackTimer() {
    lock.lock()
    if fallbackTimer != nil {
      running = true
      lock.unlock()
      return
    }
    let hz = Self.fallbackRefreshHz()
    let intervalNs = max(1_000_000, UInt64(1_000_000_000.0 / hz))
    refreshHz = hz
    running = true
    source = "fallback-timer"
    lock.unlock()

    let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .userInteractive))
    timer.schedule(deadline: .now(), repeating: .nanoseconds(Int(intervalNs)))
    timer.setEventHandler { [weak self] in
      self?.recordTickAndScheduleMain()
    }

    lock.lock()
    fallbackTimer = timer
    lock.unlock()
    timer.resume()
  }

  private static let displayLinkCallback: CVDisplayLinkOutputCallback = {
    _, _, _, _, _, userInfo in
    guard let userInfo else { return kCVReturnSuccess }
    let driver = Unmanaged<MacOSViewportDisplayLink>
      .fromOpaque(userInfo)
      .takeUnretainedValue()
    driver.recordTickAndScheduleMain()
    return kCVReturnSuccess
  }

  private func recordTickAndScheduleMain() {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    lock.lock()
    guard running else {
      lock.unlock()
      return
    }
    tickCount += 1
    tickRate.record(nowNs: nowNs)
    if lastCallbackNs > 0, nowNs > lastCallbackNs {
      let interval = nowNs - lastCallbackNs
      intervalsNs.append(interval)
      if intervalsNs.count > 240 {
        intervalsNs.removeFirst(intervalsNs.count - 240)
      }
      let observedHz = 1_000_000_000.0 / Double(interval)
      if observedHz.isFinite && observedHz > 1.0 {
        refreshHz = refreshHz > 1.0 ? (refreshHz * 0.9 + observedHz * 0.1) : observedHz
      }
    }
    lastCallbackNs = nowNs
    if mainTickScheduled {
      lock.unlock()
      return
    }
    mainTickScheduled = true
    lock.unlock()

    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.lock.lock()
      self.mainTickScheduled = false
      self.deliveredTickCount += 1
      self.deliveredTickRate.record()
      let shouldDeliver = self.running
      self.lock.unlock()
      if shouldDeliver {
        self.onTick()
      }
    }
  }

  private func intervalP95MsLocked() -> Double {
    guard !intervalsNs.isEmpty else { return 0.0 }
    let sorted = intervalsNs.sorted()
    let index = min(sorted.count - 1, Int(Double(sorted.count - 1) * 0.95))
    return Double(sorted[index]) / 1_000_000.0
  }

  private static func nominalRefreshHz(displayLink: CVDisplayLink) -> Double {
    let nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(displayLink)
    guard nominal.timeValue > 0, nominal.timeScale > 0 else {
      return fallbackRefreshHz()
    }
    return Double(nominal.timeScale) / Double(nominal.timeValue)
  }

  private static func fallbackRefreshHz() -> Double {
    let screen = currentScreen()
    if #available(macOS 10.15, *) {
      let fps = screen?.maximumFramesPerSecond ?? 0
      if fps > 0 {
        return Double(fps)
      }
    }
    return 60.0
  }

  private static func currentDisplayId() -> CGDirectDisplayID {
    let screen = currentScreen()
    let key = NSDeviceDescriptionKey("NSScreenNumber")
    if let number = screen?.deviceDescription[key] as? NSNumber {
      return CGDirectDisplayID(number.uint32Value)
    }
    return CGMainDisplayID()
  }

  private static func currentScreen() -> NSScreen? {
    NSApplication.shared.keyWindow?.screen
      ?? NSApplication.shared.mainWindow?.screen
      ?? NSApplication.shared.windows.first(where: { $0.isVisible && $0.screen != nil })?.screen
      ?? NSScreen.main
      ?? NSScreen.screens.first
  }
}
