import Cocoa
import CoreVideo
import Foundation

private final class MacOSViewportDisplayLinkCallbackContext {
  private let condition = NSCondition()
  private weak var displayLink: MacOSViewportDisplayLink?
  private var alive = true
  private var inFlight = 0

  init(displayLink: MacOSViewportDisplayLink) {
    self.displayLink = displayLink
  }

  func beginTick() -> MacOSViewportDisplayLink? {
    condition.lock()
    guard alive, let displayLink else {
      condition.unlock()
      return nil
    }
    inFlight += 1
    condition.unlock()
    return displayLink
  }

  func endTick() {
    condition.lock()
    if inFlight > 0 {
      inFlight -= 1
    }
    if inFlight == 0 {
      condition.broadcast()
    }
    condition.unlock()
  }

  func invalidateAndWait() {
    condition.lock()
    alive = false
    while inFlight > 0 {
      condition.wait()
    }
    condition.unlock()
  }
}

final class MacOSViewportDisplayLink {
  private let lock = NSLock()
  private let onTick: () -> Void
  private var displayLink: CVDisplayLink?
  private var displayLinkUserData: UnsafeMutableRawPointer?
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
    clearDisplayLink()
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
    let oldUserData = displayLinkUserData
    let oldTimer = fallbackTimer
    displayLink = nil
    displayLinkUserData = nil
    fallbackTimer = nil
    running = false
    mainTickScheduled = false
    targetDisplayId = displayId
    lock.unlock()
    if let oldLink, CVDisplayLinkIsRunning(oldLink) {
      CVDisplayLinkStop(oldLink)
    }
    oldTimer?.cancel()
    releaseDisplayLinkUserData(oldUserData)

    var newLink: CVDisplayLink?
    let status = CVDisplayLinkCreateWithCGDisplay(displayId, &newLink)
    guard status == kCVReturnSuccess, let created = newLink else {
      lock.lock()
      source = "fallback-timer"
      refreshHz = Self.fallbackRefreshHz()
      lock.unlock()
      return
    }
    let userData = Unmanaged.passRetained(
      MacOSViewportDisplayLinkCallbackContext(displayLink: self)
    ).toOpaque()
    let callbackStatus = CVDisplayLinkSetOutputCallback(
      created,
      MacOSViewportDisplayLink.displayLinkCallback,
      userData
    )
    guard callbackStatus == kCVReturnSuccess else {
      releaseDisplayLinkUserData(userData)
      lock.lock()
      source = "fallback-timer"
      refreshHz = Self.fallbackRefreshHz()
      lock.unlock()
      return
    }
    lock.lock()
    displayLink = created
    displayLinkUserData = userData
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
    let context = Unmanaged<MacOSViewportDisplayLinkCallbackContext>
      .fromOpaque(userInfo)
      .takeUnretainedValue()
    guard let driver = context.beginTick() else {
      return kCVReturnSuccess
    }
    defer { context.endTick() }
    driver.recordTickAndScheduleMain()
    return kCVReturnSuccess
  }

  private func clearDisplayLink() {
    lock.lock()
    let oldLink = displayLink
    let oldUserData = displayLinkUserData
    displayLink = nil
    displayLinkUserData = nil
    lock.unlock()
    if let oldLink, CVDisplayLinkIsRunning(oldLink) {
      CVDisplayLinkStop(oldLink)
    }
    releaseDisplayLinkUserData(oldUserData)
  }

  private func releaseDisplayLinkUserData(_ userData: UnsafeMutableRawPointer?) {
    guard let userData else { return }
    let unmanaged = Unmanaged<MacOSViewportDisplayLinkCallbackContext>
      .fromOpaque(userData)
    unmanaged.takeUnretainedValue().invalidateAndWait()
    unmanaged.release()
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
