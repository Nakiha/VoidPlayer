import Foundation

func macOSNativeFrameAvailable(_ userData: UnsafeMutableRawPointer?) {
  guard let userData else { return }
  let renderer = Unmanaged<MacOSVideoRendererBridge>.fromOpaque(userData).takeUnretainedValue()
  renderer.scheduleNativeFrameCopyFromCallback()
}

final class MacOSNativeFramePump {
  private var callbackRegistered = false
  private(set) var targetInstalled = false

  func setTargetInstalled(_ installed: Bool) {
    targetInstalled = installed
  }

  func ensure(
    player: MacOSNativePlayerSession,
    texture: MacOSFlutterTextureBridge?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) -> Bool {
    if !targetInstalled, let texture {
      targetInstalled = texture.installNativePresentationTarget(
        player,
        maxTrackSlots: maxTrackSlots
      )
    }
    if !callbackRegistered {
      player.setFrameAvailableCallback(macOSNativeFrameAvailable, userData: userData)
      callbackRegistered = true
    }
    guard targetInstalled else {
      presentationState.recordError()
      NSLog("VoidPlayer macOS renderer-owned Metal presentation target unavailable")
      return false
    }
    return true
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
    texture?.resetNativeUploadBaseline()
    targetInstalled = false
    if !ensure(
      player: player,
      texture: texture,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    ) {
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
