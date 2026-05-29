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
  private var layoutRefreshRunning = false
  private var latestLayoutRefreshRequest: LayoutRefreshRequest?
  private var layoutIntentCount = 0
  private var layoutSubmitCount = 0
  private var layoutDrawCount = 0
  private var layoutSkipCount = 0

  func resetLayout() {
    cancelPendingLayoutRefreshes()
    layout = MacOSVideoTrackPayload.defaultLayout()
  }

  func applyLayout(arguments: Any?, context: MacOSPresentationContext) {
    guard let nextLayout = MacOSNativeLayoutBridge.layoutMap(arguments: arguments) else {
      return
    }
    layoutIntentCount += 1
    layout = nextLayout
    requestDisplayLinkedLayoutRefresh(context: context, layout: nextLayout)
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

  func diagnosticMap() -> [String: Any] {
    var diagnostics = displayLink.diagnosticMap()
    diagnostics["layoutIntentCount"] = layoutIntentCount
    diagnostics["layoutSubmitCount"] = layoutSubmitCount
    diagnostics["layoutDrawCount"] = layoutDrawCount
    diagnostics["layoutSkipCount"] = layoutSkipCount
    diagnostics["layoutRefreshRunning"] = layoutRefreshRunning
    diagnostics["layoutIntentPending"] = latestLayoutRefreshRequest != nil
    return diagnostics
  }

  private func cancelPendingLayoutRefreshes() {
    latestLayoutRefreshRequest = nil
    displayLink.stop()
    if layoutRefreshRunning {
      layoutRefreshQueue.sync {}
    }
    layoutRefreshRunning = false
  }

  private func requestDisplayLinkedLayoutRefresh(
    context: MacOSPresentationContext,
    layout: [String: Any]
  ) {
    guard context.nativeBackendActive,
          context.player != nil,
          context.nativeTexture != nil else {
      context.markFrameAvailable()
      return
    }
    latestLayoutRefreshRequest = LayoutRefreshRequest(context: context, layout: layout)
    displayLink.start()
  }

  private func processViewportDisplayTick() {
    guard let request = latestLayoutRefreshRequest else {
      layoutSkipCount += 1
      if !layoutRefreshRunning {
        displayLink.stop()
      }
      return
    }
    guard !layoutRefreshRunning else {
      layoutSkipCount += 1
      return
    }
    latestLayoutRefreshRequest = nil
    layoutRefreshRunning = true
    layoutSubmitCount += 1
    layoutRefreshQueue.async { [weak self, request] in
      let startNs = DispatchTime.now().uptimeNanoseconds
      let outcome = Self.performLayoutRefresh(request: request)
      let finishNs = DispatchTime.now().uptimeNanoseconds
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        if case .applied = outcome {
          self.layoutDrawCount += 1
        } else {
          request.context.presentationState.recordMiss()
        }
        self.layoutRefreshRunning = false
        self.logLayoutProfiler(
          route: "display-link-layout",
          requestNs: request.requestNs,
          queueDelayNs: startNs >= request.requestNs ? startNs - request.requestNs : 0,
          applyNs: finishNs >= startNs ? finishNs - startNs : 0,
          outcome: outcome.profilerName
        )
        if self.latestLayoutRefreshRequest != nil {
          self.displayLink.start()
        } else {
          self.displayLink.stop()
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
      format: "VoidPlayer macOS layout profiler route=%@ outcome=%@ intents=%d submits=%d draws=%d skips=%d totalMs=%.2f queueMs=%.2f applyMs=%.2f source=%@ hz=%.1f",
      route,
      outcome,
      layoutIntentCount,
      layoutSubmitCount,
      layoutDrawCount,
      layoutSkipCount,
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
