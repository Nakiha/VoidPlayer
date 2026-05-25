import Foundation

final class MacOSFramePresentationState {
  private let traceCapacity = 240

  private(set) var currentPtsUs = 0
  private(set) var lastPresentedPtsUs: Int?
  private(set) var lastPresentedDtsUs: Int?
  private(set) var lastPresentedDurationUs: Int?
  private var callbackCount = 0
  private var presentationCount = 0
  private var rendererOwnedPresentCount = 0
  private var missCount = 0
  private var errorCount = 0
  private var firstHostNs: UInt64?
  private var lastHostNs: UInt64?
  private var ptsTrace: [Int] = []
  private var ptsSampleCount = 0
  private var ptsDistinctCount = 0
  private var ptsFirstUs: Int?
  private var ptsLastStepUs = 0
  private var ptsMonotonicViolationCount = 0

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
    rendererOwnedPresentCount = 0
    missCount = 0
    errorCount = 0
    firstHostNs = nil
    lastHostNs = nil
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

  func recordPresentation(rendererOwned: Bool) {
    let now = DispatchTime.now().uptimeNanoseconds
    if firstHostNs == nil {
      firstHostNs = now
    }
    lastHostNs = now
    presentationCount += 1
    if rendererOwned {
      rendererOwnedPresentCount += 1
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
      "nativeFrameRendererOwnedPresentCount": rendererOwnedPresentCount,
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
  }

  private func recordPresentedPts(_ ptsUs: Int) {
    if let last = ptsTrace.last {
      let step = ptsUs - last
      ptsLastStepUs = step
      if step < 0 {
        ptsMonotonicViolationCount += 1
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
