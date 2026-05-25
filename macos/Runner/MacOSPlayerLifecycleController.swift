import FlutterMacOS
import Foundation

final class MacOSPlayerLifecycleController {
  private let textureRegistry: FlutterTextureRegistry

  private(set) var texture: MacOSVideoTexture?
  private(set) var nativeTexture: MacOSFlutterTextureBridge?
  private(set) var textureId: Int64?
  private(set) var backendName = "synthetic-texture"
  private(set) var nativePlayer: MacOSNativePlayerSession?

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

    let registeredTextureId = textureRegistry.register(startup.texture)

    texture = startup.texture
    nativeTexture = startup.nativeTexture
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

    return [
      "textureId": registeredTextureId,
      "tracks": tracks.tracks,
    ]
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
    nativeTexture = nil
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
