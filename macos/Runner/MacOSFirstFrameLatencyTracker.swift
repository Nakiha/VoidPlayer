import Foundation

final class MacOSFirstFrameLatencyTracker {
  private let lock = NSLock()
  private var generation: UInt64 = 0
  private var addCount: UInt64 = 0
  private var active = false
  private var succeeded = false
  private var trackCountBefore = 0
  private var startNs: UInt64 = 0
  private var nativeReturnNs: UInt64 = 0
  private var policyReadyNs: UInt64 = 0
  private var sourceReadyNs: UInt64 = 0
  private var compositeCompleteNs: UInt64 = 0

  func beginAdd(trackCountBefore: Int) {
    lock.lock()
    generation &+= 1
    addCount &+= 1
    active = true
    succeeded = false
    self.trackCountBefore = trackCountBefore
    startNs = DispatchTime.now().uptimeNanoseconds
    nativeReturnNs = 0
    policyReadyNs = 0
    sourceReadyNs = 0
    compositeCompleteNs = 0
    lock.unlock()
  }

  func markNativeReturned(succeeded: Bool) {
    lock.lock()
    guard active else {
      lock.unlock()
      return
    }
    nativeReturnNs = DispatchTime.now().uptimeNanoseconds
    self.succeeded = succeeded
    if !succeeded {
      active = false
    }
    lock.unlock()
  }

  func markPolicyReady() {
    lock.lock()
    if active, succeeded, policyReadyNs == 0 {
      policyReadyNs = DispatchTime.now().uptimeNanoseconds
    }
    lock.unlock()
  }

  func markSourceReady() {
    lock.lock()
    if active, succeeded, sourceReadyNs == 0 {
      sourceReadyNs = DispatchTime.now().uptimeNanoseconds
    }
    lock.unlock()
  }

  func markCompositeCompleted() -> String? {
    lock.lock()
    defer { lock.unlock() }
    guard active, succeeded, sourceReadyNs > 0, compositeCompleteNs == 0 else {
      return nil
    }
    compositeCompleteNs = DispatchTime.now().uptimeNanoseconds
    active = false
    return "generation=\(generation) ffiMs=\(milliseconds(nativeReturnNs, since: startNs)) " +
      "policyMs=\(milliseconds(policyReadyNs, since: startNs)) " +
      "sourceMs=\(milliseconds(sourceReadyNs, since: startNs)) " +
      "compositeMs=\(milliseconds(compositeCompleteNs, since: startNs))"
  }

  func diagnostics() -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }
    return [
      "nativeTrackAddLatencyGeneration": Int64(min(generation, UInt64(Int64.max))),
      "nativeTrackAddLatencyCount": Int64(min(addCount, UInt64(Int64.max))),
      "nativeTrackAddLatencyActive": active,
      "nativeTrackAddLatencySucceeded": succeeded,
      "nativeTrackAddLatencyTrackCountBefore": trackCountBefore,
      "nativeTrackAddLatencyFFIMsX1000": millisecondsX1000(nativeReturnNs, since: startNs),
      "nativeTrackAddLatencyPolicyMsX1000": millisecondsX1000(policyReadyNs, since: startNs),
      "nativeTrackAddLatencySourceReadyMsX1000": millisecondsX1000(sourceReadyNs, since: startNs),
      "nativeTrackAddLatencyCompositeMsX1000": millisecondsX1000(
        compositeCompleteNs,
        since: startNs
      ),
      "nativeTrackAddLatencySourceToCompositeMsX1000": millisecondsX1000(
        compositeCompleteNs,
        since: sourceReadyNs
      ),
    ]
  }

  private func milliseconds(_ endNs: UInt64, since startNs: UInt64) -> String {
    guard endNs >= startNs, startNs > 0 else { return "pending" }
    return String(format: "%.3f", Double(endNs - startNs) / 1_000_000.0)
  }

  private func millisecondsX1000(_ endNs: UInt64, since startNs: UInt64) -> Int64 {
    guard endNs >= startNs, startNs > 0 else { return -1 }
    return Int64(min((endNs - startNs) / 1_000, UInt64(Int64.max)))
  }
}
