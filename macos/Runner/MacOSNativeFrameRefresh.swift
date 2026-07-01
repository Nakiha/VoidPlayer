import FlutterMacOS
import Foundation

enum MacOSNativeLayoutDrawResult {
  case ready(MacOSPendingNativeFrame)
  case coalesced
  case failed
}

enum MacOSNativeSeekRefreshResult {
  case presented
  case pending
  case failed(FlutterError)
}

enum MacOSNativeStepRefreshResult {
  case presented
  case pending(targetPtsUs: Int)
  case failed(FlutterError)
}

enum MacOSNativeFrameRefresh {
  static func acceptsSeekFrame(_ frameInfo: MacOSNativeFrameInfo, targetPtsUs: Int) -> Bool {
    frameInfo.ptsUs >= max(0, targetPtsUs - 500_000) &&
      frameInfo.ptsUs <= targetPtsUs + 1_500_000
  }

  static func acceptsSeekPts(_ ptsUs: Int, targetPtsUs: Int) -> Bool {
    ptsUs >= max(0, targetPtsUs - 500_000) &&
      ptsUs <= targetPtsUs + 1_500_000
  }

  static func seekAndRefresh(
    player: MacOSNativePlayerSession,
    rendererTarget: MacOSRendererOwnedPresentationTarget,
    targetPtsUs: Int,
    timeoutMs: Int = 3_000,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> MacOSNativeSeekRefreshResult {
    let startNs = DispatchTime.now().uptimeNanoseconds
    do {
      player.seek(targetPtsUs)
      let (frameInfo, attempts) = try updateFromNativePlayerWithTransientRetry(
        route: "seek",
        player,
        rendererTarget: rendererTarget,
        maxTrackSlots: maxTrackSlots,
        timeoutMs: timeoutMs,
        acceptFrame: { frameInfo in
          acceptsSeekFrame(frameInfo, targetPtsUs: targetPtsUs)
        }
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordDiscontinuityFrame(frameInfo)
      logRefreshProfiler(
        route: "seek",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: attempts > 1 ? "ok attempts=\(attempts)" : "ok",
        ptsUs: frameInfo.ptsUs
      )
      return .presented
    } catch {
      let transient = (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
      logRefreshProfiler(
        route: "seek",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: transient ? "pending:\(error)" : "error:\(error)",
        ptsUs: -1
      )
      if transient {
        return .pending
      }
      return .failed(FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      ))
    }
  }

  private static func updateFromNativePlayerWithTransientRetry(
    route: String,
    _ player: MacOSNativePlayerSession,
    rendererTarget: MacOSRendererOwnedPresentationTarget,
    maxTrackSlots: Int,
    timeoutMs: Int,
    acceptFrame: (MacOSNativeFrameInfo) -> Bool = { _ in true }
  ) throws -> (MacOSNativeFrameInfo, Int) {
    let deadlineNs = DispatchTime.now().uptimeNanoseconds +
      UInt64(max(1, timeoutMs)) * 1_000_000
    var attempts = 0
    while true {
      attempts += 1
      let nowNs = DispatchTime.now().uptimeNanoseconds
      let remainingMs = max(1, Int((deadlineNs > nowNs ? deadlineNs - nowNs : 0) / 1_000_000))
      do {
        let frameInfo = try rendererTarget.updateFromNativePlayer(
          player,
          maxTrackSlots: maxTrackSlots,
          waitTimeoutMs: remainingMs
        )
        guard acceptFrame(frameInfo) else {
          throw MacOSNativePlayerError.transientFrameUnavailable(
            "renderer-owned Metal refresh returned a stale frame pts=\(frameInfo.ptsUs)"
          )
        }
        return (frameInfo, attempts)
      } catch {
        let transient = (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
        if !transient || DispatchTime.now().uptimeNanoseconds >= deadlineNs {
          throw error
        }
        MacOSProfilerLog.trace(
          "VoidPlayer macOS refresh retry route=\(route) attempt=\(attempts) error=\(error)"
        )
        Thread.sleep(forTimeInterval: 0.02)
      }
    }
  }

  static func refreshCurrentFrameAfterLayoutChange(
    player: MacOSNativePlayerSession,
    rendererTarget: MacOSRendererOwnedPresentationTarget,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> Bool {
    let startNs = DispatchTime.now().uptimeNanoseconds
    let timeoutMs = rendererTarget.rendererOwnedRunnerLayerActive ? 500 : 100
    do {
      let frameInfo = try rendererTarget.updateFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: timeoutMs
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      logRefreshProfiler(
        route: "layout",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: "ok",
        ptsUs: frameInfo.ptsUs
      )
      return true
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        presentationState.recordMiss()
      } else {
        NSLog("VoidPlayer macOS native layout refresh failed: \(error)")
      }
      logRefreshProfiler(
        route: "layout",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: "error:\(error)",
        ptsUs: -1
      )
      return false
    }
  }

  static func drawCurrentFrameForLayoutRefresh(
    player: MacOSNativePlayerSession,
    rendererTarget: MacOSRendererOwnedPresentationTarget,
    maxTrackSlots: Int,
    waitTimeoutMs overrideTimeoutMs: Int? = nil
  ) -> MacOSNativeLayoutDrawResult {
    let startNs = DispatchTime.now().uptimeNanoseconds
    let timeoutMs = overrideTimeoutMs ?? (rendererTarget.rendererOwnedRunnerLayerActive ? 500 : 100)
    do {
      let pending = try rendererTarget.drawFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: timeoutMs
      )
      logRefreshProfiler(
        route: "layout-draw",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: "ok",
        ptsUs: pending.info.ptsUs
      )
      return .ready(pending)
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        logRefreshProfiler(
          route: "layout-draw",
          startNs: startNs,
          timeoutMs: timeoutMs,
          result: "coalesced:\(error)",
          ptsUs: -1
        )
        return .coalesced
      }
      logRefreshProfiler(
        route: "layout-draw",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: "error:\(error)",
        ptsUs: -1
      )
      return .failed
    }
  }

  static func publishLayoutRefreshFrame(
    _ pending: MacOSPendingNativeFrame,
    player: MacOSNativePlayerSession,
    rendererTarget: MacOSRendererOwnedPresentationTarget,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> Bool {
    let startNs = DispatchTime.now().uptimeNanoseconds
    do {
      let outcome = try rendererTarget.publishPendingNativeFrame(
        pending,
        player: player,
        maxTrackSlots: maxTrackSlots
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      let publishResult: String
      switch outcome {
      case .published:
        publishResult = "ok"
      case .alreadyPublished:
        publishResult = "already-published"
      case .notReady:
        publishResult = "not-ready"
      }
      logRefreshProfiler(
        route: "layout-publish",
        startNs: startNs,
        timeoutMs: 0,
        result: publishResult,
        ptsUs: pending.info.ptsUs
      )
      guard outcome != .notReady else {
        presentationState.recordMiss()
        return false
      }
      presentationState.recordFrame(pending.info)
      return true
    } catch {
      rendererTarget.discardPendingNativeFrame(pending)
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        presentationState.recordMiss()
      } else {
        NSLog("VoidPlayer macOS native layout publish failed: \(error)")
      }
      logRefreshProfiler(
        route: "layout-publish",
        startNs: startNs,
        timeoutMs: 0,
        result: "error:\(error)",
        ptsUs: pending.info.ptsUs
      )
      return false
    }
  }

  static func stepAndRefresh(
    player: MacOSNativePlayerSession,
    rendererTarget: MacOSRendererOwnedPresentationTarget,
    forward: Bool,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> MacOSNativeStepRefreshResult {
    let startNs = DispatchTime.now().uptimeNanoseconds
    let timeoutMs = 360
    var targetPtsUs = -1
    do {
      if forward {
        try player.stepForward()
      } else {
        try player.stepBackward()
      }
      targetPtsUs = player.currentPtsUs()
      let acceptStepFrame: (MacOSNativeFrameInfo) -> Bool = { frameInfo in
        let nearStepTarget = frameInfo.ptsUs >= max(0, targetPtsUs - 100_000) &&
          frameInfo.ptsUs <= targetPtsUs + 100_000
        let nearBackwardStepTarget = frameInfo.ptsUs >= max(0, targetPtsUs - 500_000) &&
          frameInfo.ptsUs <= targetPtsUs + 100_000
        return forward ? nearStepTarget : nearBackwardStepTarget
      }
      if let frameInfo = player.lastRendererOwnedFrameInfo(),
         acceptStepFrame(frameInfo) {
        _ = rendererTarget.publishRenderedTargetAndInstallNext(
          player,
          maxTrackSlots: maxTrackSlots,
          frameInfo: frameInfo
        )
        framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
        presentationState.recordFrame(frameInfo)
        logRefreshProfiler(
          route: forward ? "step-forward" : "step-backward",
          startNs: startNs,
          timeoutMs: timeoutMs,
          result: "ok immediate",
          ptsUs: frameInfo.ptsUs
        )
        return .presented
      }
      let (frameInfo, attempts) = try updateFromNativePlayerWithTransientRetry(
        route: forward ? "step-forward" : "step-backward",
        player,
        rendererTarget: rendererTarget,
        maxTrackSlots: maxTrackSlots,
        timeoutMs: timeoutMs,
        acceptFrame: { frameInfo in
          acceptStepFrame(frameInfo)
        }
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      logRefreshProfiler(
        route: forward ? "step-forward" : "step-backward",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: attempts > 1 ? "ok attempts=\(attempts)" : "ok",
        ptsUs: frameInfo.ptsUs
      )
      return .presented
    } catch {
      let transient = (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
      logRefreshProfiler(
        route: forward ? "step-forward" : "step-backward",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: transient ? "pending:\(error)" : "error:\(error)",
        ptsUs: -1
      )
      if transient, targetPtsUs >= 0 {
        return .pending(targetPtsUs: targetPtsUs)
      }
      return .failed(FlutterError(
        code: "STEP_FAILED",
        message: "Failed to step macOS native playback",
        details: "\(error)"
      ))
    }
  }

  private static func logRefreshProfiler(
    route: String,
    startNs: UInt64,
    timeoutMs: Int,
    result: String,
    ptsUs: Int
  ) {
    let elapsedNs = DispatchTime.now().uptimeNanoseconds - startNs
    let isError = result.hasPrefix("error:")
    let slow = elapsedNs >= 12_000_000 || isError
    guard slow else { return }
    MacOSProfilerLog.log(String(
      format: "VoidPlayer macOS refresh profiler route=%@ result=%@ elapsedMs=%.2f timeoutMs=%d ptsUs=%d",
      route,
      result,
      Double(elapsedNs) / 1_000_000.0,
      timeoutMs,
      ptsUs
    ))
  }
}
