import FlutterMacOS
import Foundation

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
    do {
      player.seek(targetPtsUs)
      let frameInfo = try texture.updateFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 3_000
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      logRefreshProfiler(
        route: "seek",
        startNs: startNs,
        timeoutMs: 3_000,
        result: "ok",
        ptsUs: frameInfo.ptsUs
      )
      return nil
    } catch {
      logRefreshProfiler(
        route: "seek",
        startNs: startNs,
        timeoutMs: 3_000,
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

  static func stepAndRefresh(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge,
    forward: Bool,
    maxTrackSlots: Int,
    presentationState: MacOSFramePresentationState,
    framePump: MacOSNativeFramePump
  ) -> FlutterError? {
    let startNs = DispatchTime.now().uptimeNanoseconds
    do {
      if forward {
        try player.stepForward()
      } else {
        try player.stepBackward()
      }
      let frameInfo = try texture.updateFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 3_000
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      logRefreshProfiler(
        route: forward ? "step-forward" : "step-backward",
        startNs: startNs,
        timeoutMs: 3_000,
        result: "ok",
        ptsUs: frameInfo.ptsUs
      )
    } catch {
      logRefreshProfiler(
        route: forward ? "step-forward" : "step-backward",
        startNs: startNs,
        timeoutMs: 3_000,
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
    let slow = elapsedNs >= 12_000_000 || result != "ok"
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
