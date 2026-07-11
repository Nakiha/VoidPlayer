import Foundation

final class MacOSFramePresentationState {
  private let traceCapacity = 240

  private(set) var currentPtsUs = 0
  private(set) var lastPresentedPtsUs: Int?
  private(set) var lastPresentedDtsUs: Int?
  private(set) var lastPresentedDurationUs: Int?
  private var callbackCount = 0
  private var presentationCount = 0
  private var nativeTargetPresentCount = 0
  private var missCount = 0
  private var errorCount = 0
  private var firstHostNs: UInt64?
  private var lastHostNs: UInt64?
  private var hostIntervalSampleCount = 0
  private var hostIntervalTotalNs: UInt64 = 0
  private var hostIntervalMaxNs: UInt64 = 0
  private var hostIntervalsNs: [UInt64] = []
  private var ptsTrace: [Int] = []
  private var ptsSampleCount = 0
  private var ptsDistinctCount = 0
  private var ptsFirstUs: Int?
  private var ptsLastStepUs = 0
  private var ptsMonotonicViolationCount = 0
  private var ptsDuplicateCount = 0
  private var ptsLargeGapCount = 0

  func resetAll() {
    currentPtsUs = 0
    lastPresentedPtsUs = nil
    lastPresentedDtsUs = nil
    lastPresentedDurationUs = nil
    resetFrameCounters()
  }

  func resetFrameCounters() {
    callbackCount = 0
    presentationCount = 0
    nativeTargetPresentCount = 0
    missCount = 0
    errorCount = 0
    firstHostNs = nil
    lastHostNs = nil
    hostIntervalSampleCount = 0
    hostIntervalTotalNs = 0
    hostIntervalMaxNs = 0
    hostIntervalsNs.removeAll(keepingCapacity: true)
    resetPtsTrace()
  }

  func setCurrentPts(_ ptsUs: Int) {
    currentPtsUs = ptsUs
  }

  func recordFrame(_ info: MacOSNativeFrameInfo) {
    currentPtsUs = info.ptsUs
    lastPresentedPtsUs = info.ptsUs
    lastPresentedDtsUs = Self.normalizedDtsUs(info)
    lastPresentedDurationUs = info.durationUs
    recordPresentedPts(info.ptsUs)
  }

  func recordDiscontinuityFrame(_ info: MacOSNativeFrameInfo) {
    resetPtsTrace()
    recordFrame(info)
  }

  func seedPresentedFrame(ptsUs: Int, dtsUs: Int, durationUs: Int) {
    currentPtsUs = ptsUs
    lastPresentedPtsUs = ptsUs
    lastPresentedDtsUs = dtsUs
    lastPresentedDurationUs = durationUs
    recordPresentedPts(ptsUs)
  }

  func recordCallback() {
    callbackCount += 1
  }

  func recordPresentation(nativeTarget: Bool) {
    let now = DispatchTime.now().uptimeNanoseconds
    if firstHostNs == nil {
      firstHostNs = now
    }
    if let last = lastHostNs, now >= last {
      let interval = now - last
      hostIntervalSampleCount += 1
      hostIntervalTotalNs += interval
      hostIntervalMaxNs = max(hostIntervalMaxNs, interval)
      hostIntervalsNs.append(interval)
      if hostIntervalsNs.count > traceCapacity {
        hostIntervalsNs.removeFirst(hostIntervalsNs.count - traceCapacity)
      }
    }
    lastHostNs = now
    presentationCount += 1
    if nativeTarget {
      nativeTargetPresentCount += 1
    }
  }

  func recordMiss() {
    missCount += 1
  }

  func recordError() {
    errorCount += 1
  }

  func currentPresentedFrameMap() -> [String: Any] {
    [
      "ptsUs": lastPresentedPtsUs ?? currentPtsUs,
      "dtsUs": lastPresentedDtsUs ?? lastPresentedPtsUs ?? currentPtsUs,
      "durationUs": lastPresentedDurationUs ?? 0,
    ]
  }

  func diagnosticMap() -> [String: Any] {
    let fps = presentationFps()
    return [
      "nativeFrameCallbackCount": callbackCount,
      "nativeFramePresentationCount": presentationCount,
      "nativeFramePresentationElapsedMs": presentationElapsedMs(),
      "nativeFramePresentationFps": fps,
      "nativeFramePresentationFpsX1000": Int(fps * 1000.0),
      "nativeFrameCopyCount": presentationCount,
      "nativeFrameTargetPresentCount": nativeTargetPresentCount,
      "nativeFrameTargetRatioX1000": nativeTargetRatioX1000(),
      "nativeFrameCopyMissCount": missCount,
      "nativeFrameCopyErrorCount": errorCount,
      "nativeFrameCopyElapsedMs": presentationElapsedMs(),
      "nativeFrameCopyFps": fps,
      "nativeFrameCopyFpsX1000": Int(fps * 1000.0),
      "presentedFramePtsSampleCount": ptsSampleCount,
      "presentedFramePtsDistinctCount": ptsDistinctCount,
      "presentedFramePtsFirstUs": ptsFirstUs ?? -1,
      "presentedFramePtsLastUs": lastPresentedPtsUs ?? -1,
      "presentedFrameDtsLastUs": lastPresentedDtsUs ?? -1,
      "presentedFrameDurationLastUs": lastPresentedDurationUs ?? 0,
      "presentedFramePtsAdvanceUs": presentedPtsAdvanceUs(),
      "presentedFramePtsLastStepUs": ptsLastStepUs,
      "presentedFramePtsMonotonicViolationCount": ptsMonotonicViolationCount,
      "presentedFramePtsDuplicateCount": ptsDuplicateCount,
      "presentedFramePtsLargeGapCount": ptsLargeGapCount,
      "presentedFrameExpectedIntervalUs": expectedPresentedIntervalUs(),
      "presentedFrameHostIntervalSampleCount": hostIntervalSampleCount,
      "presentedFrameHostIntervalAvgMs": hostIntervalAvgMs(),
      "presentedFrameHostIntervalMaxMs": hostIntervalMaxMs(),
      "presentedFrameHostIntervalP95Ms": hostIntervalP95Ms(),
      "presentedFrameDropCount": missCount,
      "presentedFrameLateCount": 0,
      "presentedFrameErrorCount": errorCount,
      "presentedFramePtsTrace": ptsTrace.suffix(32).map { String($0) }.joined(separator: ","),
    ]
  }

  static func normalizedDtsUs(_ info: MacOSNativeFrameInfo) -> Int {
    info.dtsUs == Int.min ? info.ptsUs : info.dtsUs
  }

  private func resetPtsTrace() {
    ptsTrace.removeAll(keepingCapacity: true)
    ptsSampleCount = 0
    ptsDistinctCount = 0
    ptsFirstUs = nil
    ptsLastStepUs = 0
    ptsMonotonicViolationCount = 0
    ptsDuplicateCount = 0
    ptsLargeGapCount = 0
  }

  private func recordPresentedPts(_ ptsUs: Int) {
    if let last = ptsTrace.last {
      let step = ptsUs - last
      ptsLastStepUs = step
      if step < 0 {
        ptsMonotonicViolationCount += 1
      }
      if step == 0 {
        ptsDuplicateCount += 1
      }
      if step > largeGapThresholdUs() {
        ptsLargeGapCount += 1
      }
      if step != 0 {
        ptsDistinctCount += 1
      }
    } else {
      ptsDistinctCount = 1
    }
    if ptsFirstUs == nil {
      ptsFirstUs = ptsUs
    }
    ptsSampleCount += 1
    ptsTrace.append(ptsUs)
    if ptsTrace.count > traceCapacity {
      ptsTrace.removeFirst(ptsTrace.count - traceCapacity)
    }
  }

  private func presentedPtsAdvanceUs() -> Int {
    guard let first = ptsFirstUs,
          let last = lastPresentedPtsUs else {
      return 0
    }
    return max(0, last - first)
  }

  private func expectedPresentedIntervalUs() -> Int {
    max(0, lastPresentedDurationUs ?? 0)
  }

  private func largeGapThresholdUs() -> Int {
    max(Int(Double(expectedPresentedIntervalUs()) * 2.5), 100_000)
  }

  private func nativeTargetRatioX1000() -> Int {
    guard presentationCount > 0 else {
      return 0
    }
    return nativeTargetPresentCount * 1000 / presentationCount
  }

  private func hostIntervalAvgMs() -> Int {
    guard hostIntervalSampleCount > 0 else {
      return 0
    }
    return Int(hostIntervalTotalNs / UInt64(hostIntervalSampleCount) / 1_000_000)
  }

  private func hostIntervalMaxMs() -> Int {
    Int(hostIntervalMaxNs / 1_000_000)
  }

  private func hostIntervalP95Ms() -> Int {
    guard !hostIntervalsNs.isEmpty else {
      return 0
    }
    let sorted = hostIntervalsNs.sorted()
    let index = min(
      sorted.count - 1,
      max(0, Int(ceil(Double(sorted.count) * 0.95)) - 1)
    )
    return Int(sorted[index] / 1_000_000)
  }

  private func presentationElapsedMs() -> Int {
    guard let first = firstHostNs,
          let last = lastHostNs,
          last >= first else {
      return 0
    }
    return Int((last - first) / 1_000_000)
  }

  private func presentationFps() -> Double {
    let elapsedMs = presentationElapsedMs()
    guard presentationCount > 1, elapsedMs > 0 else {
      return 0.0
    }
    return Double(presentationCount - 1) * 1000.0 / Double(elapsedMs)
  }
}
