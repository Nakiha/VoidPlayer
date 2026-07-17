import FlutterMacOS
import Foundation

final class MacOSPlayerLifecycleController {
  private let textureRegistry: FlutterTextureRegistry

  private(set) var texture: MacOSVideoSurface?
  private(set) var nativeTargetRing: MacOSNativeTargetRing?
  private(set) var playerId: Int64?
  private(set) var textureId: Int64?
  private(set) var backendName = "synthetic-texture"
  private(set) var nativePlayer: MacOSNativePlayerSession?
  private var nextPlayerId: Int64 = 1

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
  }

  func create(
    arguments: Any?,
    playback: MacOSPlaybackController,
    tracks: MacOSVideoTrackController,
    presentationState: MacOSFramePresentationState,
    markFrameAvailable: () -> Void
  ) -> Any {
    destroy(playback: playback, tracks: tracks, presentationState: presentationState)

    let startup: MacOSVideoRendererStartup
    do {
      startup = try MacOSVideoRendererStartupFactory.make(arguments: arguments)
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to open macOS native player",
        details: "\(error)"
      )
    }

    let registeredTextureId = startup.flutterTexture.map(textureRegistry.register)
    let createdPlayerId = nextPlayerId
    nextPlayerId &+= 1

    texture = startup.texture
    nativeTargetRing = startup.nativeTargetRing
    playerId = createdPlayerId
    textureId = registeredTextureId
    backendName = startup.backendName
    nativePlayer = startup.nativePlayer
    playback.setTargetInstalled(startup.presentationTargetInstalled)
    tracks.replace(with: startup.tracks, fallbackDurationUs: startup.trackDurationUs)
    presentationState.seedPresentedFrame(
      ptsUs: startup.initialPresentedPtsUs,
      dtsUs: startup.initialPresentedDtsUs,
      durationUs: startup.trackDurationUs
    )
    markFrameAvailable()

    var result: [String: Any] = [
      "playerId": createdPlayerId,
      "tracks": tracks.tracks,
    ]
    if let registeredTextureId {
      result["textureId"] = registeredTextureId
    }
    return result
  }

  func destroy(
    playback: MacOSPlaybackController,
    tracks: MacOSVideoTrackController,
    presentationState: MacOSFramePresentationState
  ) {
    playback.stopFramePump(player: nativePlayer, clearPresentationTarget: true)
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    nativeTargetRing = nil
    playerId = nil
    textureId = nil
    tracks.reset()
    presentationState.resetAll()
    backendName = "synthetic-texture"
    playback.reset()
    nativePlayer?.close()
    nativePlayer = nil
  }

  func markFrameAvailable() {
    if let id = textureId {
      textureRegistry.textureFrameAvailable(id)
    }
  }
}
