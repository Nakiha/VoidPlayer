import Foundation

final class MacOSNativeFramePump {
  private var callbackRegistered = false
  private(set) var targetInstalled = false

  func setTargetInstalled(_ installed: Bool) {
    targetInstalled = installed
  }

  func start(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) -> Bool {
    stop(player: player)
    presentationState.resetFrameCounters()
    player.resetRendererOwnedPresentationStats()
    callbackRegistered = true
    targetInstalled = false
    if let texture {
      targetInstalled = texture.installNativePresentationTarget(
        player,
        maxTrackSlots: maxTrackSlots
      )
    }
    player.setFrameAvailableCallback(macOSNativeFrameAvailable, userData: userData)
    guard targetInstalled else {
      presentationState.recordError()
      NSLog("VoidPlayer macOS renderer-owned Metal presentation target unavailable")
      player.pause()
      stop(player: player)
      return false
    }
    return true
  }

  func stop(player: MacOSNativePlayerSession?, clearPresentationTarget: Bool = false) {
    if clearPresentationTarget {
      player?.clearMetalPresentationTarget()
      targetInstalled = false
    }
    if callbackRegistered {
      player?.setFrameAvailableCallback(nil, userData: nil)
      callbackRegistered = false
    }
  }
}
