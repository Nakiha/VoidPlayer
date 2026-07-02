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
    rendererTarget: MacOSRendererOwnedPresentationTarget?,
    textureRegistered: Bool,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) {
    isPlaying = textureRegistered
    player?.play()
    startFramePump(
      player: player,
      rendererTarget: rendererTarget,
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
    rendererTarget: MacOSRendererOwnedPresentationTarget?,
    textureRegistered: Bool,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) {
    guard shouldResume else { return }
    play(
      player: player,
      rendererTarget: rendererTarget,
      textureRegistered: textureRegistered,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
  }

  func reinstallPresentationTargetIfPlaying(
    player: MacOSNativePlayerSession?,
    rendererTarget: MacOSRendererOwnedPresentationTarget?,
    maxTrackSlots: Int
  ) {
    guard isPlaying,
          let player,
          let rendererTarget else {
      return
    }
    setTargetInstalled(rendererTarget.installNativePresentationTarget(
      player,
      maxTrackSlots: maxTrackSlots,
      refresh: false
    ))
  }

  func handleFrameCallback(
    player: MacOSNativePlayerSession?,
    rendererTarget: MacOSRendererOwnedPresentationTarget?,
    maxTrackSlots: Int,
    nativeBackendActive: Bool,
    presentationState: MacOSFramePresentationState,
    markFrameAvailable: () -> Void
  ) {
    presentationState.recordCallback()
    guard nativeBackendActive else {
      return
    }
    if player?.lastRendererOwnedPresentationSucceeded() == true {
      presentationState.recordPresentation(rendererOwned: true)
      let frameInfo = player?.lastRendererOwnedFrameInfo()
      if let frameInfo {
        presentationState.recordFrame(frameInfo)
      }
      if let player, let rendererTarget {
        let publishOutcome = rendererTarget.publishRenderedTargetAndInstallNext(
          player,
          maxTrackSlots: maxTrackSlots,
          frameInfo: frameInfo
        )
        guard publishOutcome != .notReady else {
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
    NSLog("VoidPlayer macOS renderer-owned Metal presentation failed")
    isPlaying = false
    player?.pause()
    stopFramePump(player: player)
  }

  @discardableResult
  func ensurePresentationPump(
    player: MacOSNativePlayerSession?,
    rendererTarget: MacOSRendererOwnedPresentationTarget?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) -> Bool {
    guard let player else { return false }
    return framePump.ensure(
      player: player,
      rendererTarget: rendererTarget,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
  }

  private func startFramePump(
    player: MacOSNativePlayerSession?,
    rendererTarget: MacOSRendererOwnedPresentationTarget?,
    maxTrackSlots: Int,
    userData: UnsafeMutableRawPointer,
    presentationState: MacOSFramePresentationState
  ) {
    stopFramePump(player: player)
    guard let player else { return }
    let started = framePump.start(
      player: player,
      rendererTarget: rendererTarget,
      maxTrackSlots: maxTrackSlots,
      userData: userData,
      presentationState: presentationState
    )
    if !started {
      isPlaying = false
    }
  }
}
