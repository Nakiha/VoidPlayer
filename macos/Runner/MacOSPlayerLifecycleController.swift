import AppKit
import FlutterMacOS
import Foundation

final class MacOSPlayerLifecycleController {
  private let textureRegistry: FlutterTextureRegistry

  private(set) var texture: MacOSVideoTexture?
  private(set) var rendererTarget: MacOSRendererOwnedPresentationTarget?
  private(set) var textureId: Int64?
  private(set) var backendName = "synthetic-texture"
  private(set) var nativePlayer: MacOSNativePlayerSession?

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
  }

  func create(
    arguments: Any?,
    contentView: NSView?,
    playback: MacOSPlaybackController,
    tracks: MacOSVideoTrackController,
    presentationState: MacOSFramePresentationState,
    markFrameAvailable: () -> Void
  ) -> Any {
    destroy(playback: playback, tracks: tracks, presentationState: presentationState)

    let startup: MacOSVideoRendererStartup
    do {
      startup = try MacOSVideoRendererStartupFactory.make(
        arguments: arguments,
        contentView: contentView
      )
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to open macOS native player",
        details: "\(error)"
      )
    }

    let registeredTextureId = textureRegistry.register(startup.texture)

    texture = startup.texture
    rendererTarget = startup.rendererTarget
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
    playback.stopFramePump(player: nativePlayer, clearPresentationTarget: false)
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    rendererTarget = nil
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

  func replaceRendererTarget(_ nextTarget: MacOSRendererOwnedPresentationTarget?) {
    rendererTarget = nextTarget
  }
}
