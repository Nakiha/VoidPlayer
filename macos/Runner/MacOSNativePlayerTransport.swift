import Foundation

extension MacOSNativePlayerSession {
  func play() {
    VPMacOSNativePlayerPlay(handle)
  }

  func pause() {
    VPMacOSNativePlayerPause(handle)
  }

  func setSpeed(_ speed: Double) {
    VPMacOSNativePlayerSetSpeed(handle, speed)
  }

  func setLoopRange(enabled: Bool, startUs: Int, endUs: Int) {
    VPMacOSNativePlayerSetLoopRange(
      handle,
      enabled ? 1 : 0,
      Int64(startUs),
      Int64(endUs)
    )
  }

  func setAudibleTrack(_ fileId: Int) {
    VPMacOSNativePlayerSetAudibleTrack(handle, Int32(fileId))
  }

  func setTrackOffset(fileId: Int, offsetUs: Int) {
    VPMacOSNativePlayerSetTrackOffset(handle, Int32(fileId), Int64(offsetUs))
  }

  func trackOffsetUs(fileId: Int) -> Int {
    Int(VPMacOSNativePlayerTrackOffsetUs(handle, Int32(fileId)))
  }

  func seek(_ ptsUs: Int) {
    VPMacOSNativePlayerSeek(handle, Int64(ptsUs))
  }

  func stepForward() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerStepForward(handle, &error, error.count)
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player stepForward failed with code \(ret)" : message
      )
    }
  }

  func stepBackward() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerStepBackward(handle, &error, error.count)
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player stepBackward failed with code \(ret)" : message
      )
    }
  }

  func currentPtsUs() -> Int {
    Int(VPMacOSNativePlayerCurrentPtsUs(handle))
  }

  func durationUs() -> Int {
    Int(VPMacOSNativePlayerDurationUs(handle))
  }

  func width() -> Int {
    Int(VPMacOSNativePlayerWidth(handle))
  }

  func height() -> Int {
    Int(VPMacOSNativePlayerHeight(handle))
  }

  func isPlaying() -> Bool {
    VPMacOSNativePlayerIsPlaying(handle) != 0
  }

  func hasAudio() -> Bool {
    VPMacOSNativePlayerHasAudio(handle) != 0
  }

  func audioSampleRate() -> Int {
    Int(VPMacOSNativePlayerAudioSampleRate(handle))
  }

  func audioChannels() -> Int {
    Int(VPMacOSNativePlayerAudioChannels(handle))
  }

  func activeAudioTrack() -> Int {
    Int(VPMacOSNativePlayerActiveAudioTrack(handle))
  }
}
