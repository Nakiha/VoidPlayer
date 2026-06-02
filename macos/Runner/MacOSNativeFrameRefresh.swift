import FlutterMacOS
import Foundation

enum MacOSNativeLayoutDrawResult {
  case ready(MacOSPendingNativeFrame)
  case coalesced
  case failed
}

enum MacOSNativeFrameRefresh {
  static func seekAndRefresh(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge,
    targetPtsUs: Int,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> FlutterError? {
    let startNs = DispatchTime.now().uptimeNanoseconds
    let timeoutMs = 3_000
    do {
      player.seek(targetPtsUs)
      let (frameInfo, attempts) = try updateFromNativePlayerWithTransientRetry(
        route: "seek",
        player,
        texture: texture,
        maxTrackSlots: maxTrackSlots,
        timeoutMs: timeoutMs,
        acceptFrame: { frameInfo in
          frameInfo.ptsUs >= max(0, targetPtsUs - 500_000) &&
            frameInfo.ptsUs <= targetPtsUs + 1_500_000
        }
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      logRefreshProfiler(
        route: "seek",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: attempts > 1 ? "ok attempts=\(attempts)" : "ok",
        ptsUs: frameInfo.ptsUs
      )
      return nil
    } catch {
      logRefreshProfiler(
        route: "seek",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: "error:\(error)",
        ptsUs: -1
      )
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      )
    }
  }

  private static func updateFromNativePlayerWithTransientRetry(
    route: String,
    _ player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge,
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
        let frameInfo = try texture.updateFromNativePlayer(
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
    texture: MacOSFlutterTextureBridge,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> Bool {
    let startNs = DispatchTime.now().uptimeNanoseconds
    do {
      let frameInfo = try texture.updateFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 100
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      logRefreshProfiler(
        route: "layout",
        startNs: startNs,
        timeoutMs: 100,
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
        timeoutMs: 100,
        result: "error:\(error)",
        ptsUs: -1
      )
      return false
    }
  }

  static func drawCurrentFrameForLayoutRefresh(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge,
    maxTrackSlots: Int
  ) -> MacOSNativeLayoutDrawResult {
    let startNs = DispatchTime.now().uptimeNanoseconds
    do {
      let pending = try texture.drawFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 100
      )
      logRefreshProfiler(
        route: "layout-draw",
        startNs: startNs,
        timeoutMs: 100,
        result: "ok",
        ptsUs: pending.info.ptsUs
      )
      return .ready(pending)
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        logRefreshProfiler(
          route: "layout-draw",
          startNs: startNs,
          timeoutMs: 100,
          result: "coalesced:\(error)",
          ptsUs: -1
        )
        return .coalesced
      }
      logRefreshProfiler(
        route: "layout-draw",
        startNs: startNs,
        timeoutMs: 100,
        result: "error:\(error)",
        ptsUs: -1
      )
      return .failed
    }
  }

  static func publishLayoutRefreshFrame(
    _ pending: MacOSPendingNativeFrame,
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> Bool {
    let startNs = DispatchTime.now().uptimeNanoseconds
    do {
      let outcome = try texture.publishPendingNativeFrame(
        pending,
        player: player,
        maxTrackSlots: maxTrackSlots
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(pending.info)
      logRefreshProfiler(
        route: "layout-publish",
        startNs: startNs,
        timeoutMs: 0,
        result: outcome == .published ? "ok" : "already-published",
        ptsUs: pending.info.ptsUs
      )
      return true
    } catch {
      texture.discardPendingNativeFrame(pending)
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
    texture: MacOSFlutterTextureBridge,
    forward: Bool,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> FlutterError? {
    let startNs = DispatchTime.now().uptimeNanoseconds
    let timeoutMs = 3_000
    do {
      let previousFrameInfo = player.lastRendererOwnedFrameInfo()
      if forward {
        try player.stepForward()
      } else {
        try player.stepBackward()
      }
      let targetPtsUs = player.currentPtsUs()
      let (frameInfo, attempts) = try updateFromNativePlayerWithTransientRetry(
        route: forward ? "step-forward" : "step-backward",
        player,
        texture: texture,
        maxTrackSlots: maxTrackSlots,
        timeoutMs: timeoutMs,
        acceptFrame: { frameInfo in
          let nearStepTarget = frameInfo.ptsUs >= max(0, targetPtsUs - 100_000) &&
            frameInfo.ptsUs <= targetPtsUs + 100_000
          let nearBackwardStepTarget = frameInfo.ptsUs >= max(0, targetPtsUs - 500_000) &&
            frameInfo.ptsUs <= targetPtsUs + 100_000
          let acceptedTarget = forward ? nearStepTarget : nearBackwardStepTarget
          guard acceptedTarget else { return false }
          guard let previousFrameInfo,
                previousFrameInfo.ptsUs != targetPtsUs else {
            return true
          }
          return frameInfo.ptsUs != previousFrameInfo.ptsUs
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
    } catch {
      logRefreshProfiler(
        route: forward ? "step-forward" : "step-backward",
        startNs: startNs,
        timeoutMs: timeoutMs,
        result: "error:\(error)",
        ptsUs: -1
      )
      return FlutterError(
        code: "STEP_FAILED",
        message: "Failed to step macOS native playback",
        details: "\(error)"
      )
    }
    return nil
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
