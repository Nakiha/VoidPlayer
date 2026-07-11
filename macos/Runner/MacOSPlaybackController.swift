import Foundation

final class MacOSPlaybackController {
  private let framePump = MacOSNativeFramePump()
  private(set) var isPlaying = false

  var targetInstalled: Bool {
    framePump.targetInstalled
  }

  var framePumpForRefresh: MacOSNativeFramePump {
    framePump
  }

  func setTargetInstalled(_ installed: Bool) {
    framePump.setTargetInstalled(installed)
  }

  func reset() {
    isPlaying = false
    framePump.setTargetInstalled(false)
  }

  func currentIsPlaying(player: MacOSNativePlayerSession?) -> Bool {
    player?.isPlaying() ?? isPlaying
  }

  @discardableResult
  func syncPlayingState(player: MacOSNativePlayerSession?) -> Bool {
    let nativePlaying = player?.isPlaying()
    if let nativePlaying {
      isPlaying = nativePlaying
      return nativePlaying
    }
    return isPlaying
  }

  func play(
    player: MacOSNativePlayerSession?,
    texture: MacOSNativeTargetRing?,
    textureRegistered: Bool,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) {
    isPlaying = player != nil || textureRegistered
    player?.play()
    startFramePump(
      player: player,
      texture: texture,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
  }

  func pause(player: MacOSNativePlayerSession?) {
    isPlaying = false
    player?.pause()
  }

  func stopFramePump(
    player: MacOSNativePlayerSession?,
    clearPresentationTarget: Bool = false
  ) {
    framePump.stop(player: player, clearPresentationTarget: clearPresentationTarget)
  }

  func stopForBlockingCommand(player: MacOSNativePlayerSession?, pausePlayer: Bool) {
    stopFramePump(player: player)
    if pausePlayer {
      player?.pause()
    }
    isPlaying = false
  }

  func resumeIfNeeded(
    _ shouldResume: Bool,
    player: MacOSNativePlayerSession?,
    texture: MacOSNativeTargetRing?,
    textureRegistered: Bool,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) {
    guard shouldResume else { return }
    play(
      player: player,
      texture: texture,
      textureRegistered: textureRegistered,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
  }

  func reinstallPresentationTargetIfPlaying(
    player: MacOSNativePlayerSession?,
    texture: MacOSNativeTargetRing?,
    maxTrackSlots: Int
  ) {
    guard isPlaying,
          let player,
          let texture else {
      return
    }
    setTargetInstalled(texture.installNativePresentationTarget(
      player,
      maxTrackSlots: maxTrackSlots,
      refresh: false
    ))
  }

  func handleFrameCallback(
    player: MacOSNativePlayerSession?,
    texture: MacOSNativeTargetRing?,
    maxTrackSlots: Int,
    nativeBackendActive: Bool,
    presentationState: MacOSFramePresentationState,
    markFrameAvailable: () -> Void
  ) {
    presentationState.recordCallback()
    guard nativeBackendActive else {
      return
    }
    if player?.lastNativeTargetPresentationSucceeded() == true {
      presentationState.recordPresentation(nativeTarget: true)
      let frameInfo = player?.lastNativeTargetFrameInfo()
      if let frameInfo {
        presentationState.recordFrame(frameInfo)
      }
      if let player, let texture {
        guard texture.publishRenderedTargetAndInstallNext(
          player,
          maxTrackSlots: maxTrackSlots,
          frameInfo: frameInfo
        ) else {
          return
        }
      }
      markFrameAvailable()
      return
    }
    if targetInstalled {
      return
    }
    presentationState.recordError()
    NSLog("VoidPlayer macOS native Metal presentation failed")
    isPlaying = false
    player?.pause()
    stopFramePump(player: player)
  }

  @discardableResult
  func ensurePresentationPump(
    player: MacOSNativePlayerSession?,
    texture: MacOSNativeTargetRing?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) -> Bool {
    guard let player else { return false }
    return framePump.ensure(
      player: player,
      texture: texture,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
  }

  private func startFramePump(
    player: MacOSNativePlayerSession?,
    texture: MacOSNativeTargetRing?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) {
    stopFramePump(player: player)
    guard let player else { return }
    let started = framePump.start(
      player: player,
      texture: texture,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
    if !started {
      isPlaying = false
    }
  }
}
