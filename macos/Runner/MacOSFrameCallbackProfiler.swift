import Foundation

enum MacOSProfilerLog {
  static let enabled =
    ProcessInfo.processInfo.environment["VOIDPLAYER_MACOS_PROFILER"] == "1"
  static let traceEnabled =
    ProcessInfo.processInfo.environment["VOIDPLAYER_VIEWPORT_TRACE"] == "1"

  static func log(_ message: @autoclosure () -> String) {
    guard enabled else { return }
    NSLog("%@", message())
  }

  static func trace(_ message: @autoclosure () -> String) {
    guard traceEnabled else { return }
    NSLog("%@", message())
  }
}

final class MacOSRateWindow {
  private let lock = NSLock()
  private let windowNs: UInt64
  private let capacity: Int
  private var samples: [UInt64] = []
  private(set) var totalCount = 0

  init(windowMs: UInt64 = 1000, capacity: Int = 512) {
    self.windowNs = windowMs * 1_000_000
    self.capacity = capacity
  }

  func reset() {
    lock.lock()
    samples.removeAll(keepingCapacity: true)
    totalCount = 0
    lock.unlock()
  }

  func record(nowNs: UInt64 = DispatchTime.now().uptimeNanoseconds) {
    lock.lock()
    totalCount += 1
    samples.append(nowNs)
    pruneLocked(nowNs: nowNs)
    if samples.count > capacity {
      samples.removeFirst(samples.count - capacity)
    }
    lock.unlock()
  }

  func rateHz(nowNs: UInt64 = DispatchTime.now().uptimeNanoseconds) -> Double {
    lock.lock()
    pruneLocked(nowNs: nowNs)
    let rate = rateHzLocked()
    lock.unlock()
    return rate
  }

  func total() -> Int {
    lock.lock()
    let value = totalCount
    lock.unlock()
    return value
  }

  private func pruneLocked(nowNs: UInt64) {
    guard !samples.isEmpty else { return }
    let cutoff = nowNs > windowNs ? nowNs - windowNs : 0
    if let firstKept = samples.firstIndex(where: { $0 >= cutoff }) {
      if firstKept > 0 {
        samples.removeFirst(firstKept)
      }
    } else {
      samples.removeAll(keepingCapacity: true)
    }
  }

  private func rateHzLocked() -> Double {
    guard samples.count > 1,
          let first = samples.first,
          let last = samples.last,
          last > first else {
      return 0.0
    }
    return Double(samples.count - 1) * 1_000_000_000.0 / Double(last - first)
  }
}

final class MacOSDurationWindow {
  private let lock = NSLock()
  private let capacity: Int
  private var samplesNs: [UInt64] = []
  private var lastNs: UInt64 = 0

  init(capacity: Int = 240) {
    self.capacity = capacity
  }

  func reset() {
    lock.lock()
    samplesNs.removeAll(keepingCapacity: true)
    lastNs = 0
    lock.unlock()
  }

  func record(_ ns: UInt64) {
    lock.lock()
    lastNs = ns
    samplesNs.append(ns)
    if samplesNs.count > capacity {
      samplesNs.removeFirst(samplesNs.count - capacity)
    }
    lock.unlock()
  }

  func lastMs() -> Double {
    lock.lock()
    let value = Self.ms(lastNs)
    lock.unlock()
    return value
  }

  func p95Ms() -> Double {
    lock.lock()
    let value = p95MsLocked()
    lock.unlock()
    return value
  }

  private func p95MsLocked() -> Double {
    guard !samplesNs.isEmpty else { return 0.0 }
    let sorted = samplesNs.sorted()
    let index = min(
      sorted.count - 1,
      max(0, Int(ceil(Double(sorted.count) * 0.95)) - 1)
    )
    return Self.ms(sorted[index])
  }

  private static func ms(_ ns: UInt64) -> Double {
    Double(ns) / 1_000_000.0
  }
}

final class MacOSFrameCallbackProfiler {
  private let lock = NSLock()
  private let queuedRate = MacOSRateWindow()
  private let processedRate = MacOSRateWindow()
  private let coalescedRate = MacOSRateWindow()
  private var pending = false
  private var dirty = false
  private var queuedCount = 0
  private var coalescedCount = 0
  private var processedCount = 0
  private var mainWaitSampleCount = 0
  private var mainWaitTotalNs: UInt64 = 0
  private var mainWaitMaxNs: UInt64 = 0
  private var handleSampleCount = 0
  private var handleTotalNs: UInt64 = 0
  private var handleMaxNs: UInt64 = 0
  private var lastMainWaitNs: UInt64 = 0
  private var lastHandleStartNs: UInt64 = 0
  private var lastHandleNs: UInt64 = 0

  func tryEnqueue(enqueueNs: UInt64) -> Bool {
    lock.lock()
    defer { lock.unlock() }
    if pending {
      dirty = true
      coalescedCount += 1
      coalescedRate.record(nowNs: enqueueNs)
      return false
    }
    pending = true
    dirty = false
    queuedCount += 1
    queuedRate.record(nowNs: enqueueNs)
    return true
  }

  func recordMainStart(enqueueNs: UInt64, startNs: UInt64) {
    lock.lock()
    defer { lock.unlock() }
    let waitNs = startNs >= enqueueNs ? startNs - enqueueNs : 0
    lastMainWaitNs = waitNs
    mainWaitSampleCount += 1
    mainWaitTotalNs += waitNs
    mainWaitMaxNs = max(mainWaitMaxNs, waitNs)
    lastHandleStartNs = startNs
  }

  func finishProcessing(endNs: UInt64) -> UInt64? {
    lock.lock()
    defer { lock.unlock() }
    let handleNs = endNs >= lastHandleStartNs ? endNs - lastHandleStartNs : 0
    lastHandleNs = handleNs
    handleSampleCount += 1
    handleTotalNs += handleNs
    handleMaxNs = max(handleMaxNs, handleNs)
    processedCount += 1
    processedRate.record(nowNs: endNs)
    if dirty {
      dirty = false
      queuedCount += 1
      queuedRate.record()
      return DispatchTime.now().uptimeNanoseconds
    }
    pending = false
    return nil
  }

  func diagnosticMap() -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }
    return [
      "macosFrameCallbackQueuedCount": queuedCount,
      "macosFrameCallbackCoalescedCount": coalescedCount,
      "macosFrameCallbackProcessedCount": processedCount,
      "macosFrameCallbackQueuedHz": queuedRate.rateHz(),
      "macosFrameCallbackQueuedHzX1000": Int(queuedRate.rateHz() * 1000.0),
      "macosFrameCallbackCoalescedHz": coalescedRate.rateHz(),
      "macosFrameCallbackCoalescedHzX1000": Int(coalescedRate.rateHz() * 1000.0),
      "macosFrameCallbackProcessedHz": processedRate.rateHz(),
      "macosFrameCallbackProcessedHzX1000": Int(processedRate.rateHz() * 1000.0),
      "macosFrameCallbackPending": pending,
      "macosFrameCallbackDirty": dirty,
      "macosFrameCallbackMainWaitLastMs": nsToMs(lastMainWaitNs),
      "macosFrameCallbackMainWaitAvgMs": averageMs(
        totalNs: mainWaitTotalNs,
        count: mainWaitSampleCount
      ),
      "macosFrameCallbackMainWaitMaxMs": nsToMs(mainWaitMaxNs),
      "macosFrameCallbackHandleLastMs": nsToMs(lastHandleNs),
      "macosFrameCallbackHandleAvgMs": averageMs(
        totalNs: handleTotalNs,
        count: handleSampleCount
      ),
      "macosFrameCallbackHandleMaxMs": nsToMs(handleMaxNs),
    ]
  }

  private func nsToMs(_ ns: UInt64) -> Int {
    Int(ns / 1_000_000)
  }

  private func averageMs(totalNs: UInt64, count: Int) -> Int {
    guard count > 0 else { return 0 }
    return Int(totalNs / UInt64(count) / 1_000_000)
  }
}
