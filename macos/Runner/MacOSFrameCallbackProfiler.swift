import Foundation

final class MacOSFrameCallbackProfiler {
  private let lock = NSLock()
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
      return false
    }
    pending = true
    dirty = false
    queuedCount += 1
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
    if dirty {
      dirty = false
      queuedCount += 1
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
