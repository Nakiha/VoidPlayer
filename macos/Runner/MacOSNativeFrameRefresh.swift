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
    do {
      player.seek(targetPtsUs)
      let frameInfo = try texture.updateFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 3_000
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      return nil
    } catch {
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
    do {
      let frameInfo = try texture.updateFromNativePlayer(
        player,
        maxTrackSlots: maxTrackSlots,
        waitTimeoutMs: 100
      )
      framePump.setTargetInstalled(player.rendererOwnedPresentationActive())
      presentationState.recordFrame(frameInfo)
      return true
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        presentationState.recordMiss()
      } else {
        NSLog("VoidPlayer macOS native layout refresh failed: \(error)")
      }
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
    } catch {
      return FlutterError(
        code: "STEP_FAILED",
        message: "Failed to step macOS native playback",
        details: "\(error)"
      )
    }
    return nil
  }
}
