import Foundation

struct MacOSPresentationContext {
  let nativeBackendActive: Bool
  let player: MacOSNativePlayerSession?
  let texture: MacOSVideoTexture?
  let nativeTexture: MacOSFlutterTextureBridge?
  let maxTrackSlots: Int
  let playback: MacOSPlaybackController
  let presentationState: MacOSFramePresentationState
  let markFrameAvailable: () -> Void
}

final class MacOSPresentationController {
  private(set) var layout: [String: Any] = MacOSVideoTrackPayload.defaultLayout()
  private let layoutRefreshQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.layout-refresh",
    qos: .userInteractive
  )
  private var layoutRefreshGeneration = 0
  private var layoutRefreshRunning = false
  private var latestLayoutRefreshContext: MacOSPresentationContext?

  func resetLayout() {
    cancelPendingLayoutRefreshes()
    layout = MacOSVideoTrackPayload.defaultLayout()
  }

  func applyLayout(arguments: Any?, context: MacOSPresentationContext) {
    guard let nextLayout = MacOSNativeLayoutBridge.apply(
      arguments: arguments,
      player: context.player
    ) else {
      return
    }
    layout = nextLayout
    requestCoalescedLayoutRefresh(context: context)
  }

  func resize(arguments: Any?, context: MacOSPresentationContext) {
    var nativeRefreshAttempted = false
    let width = MacOSFlutterArguments.intArg(arguments, "width")
    let height = MacOSFlutterArguments.intArg(arguments, "height")
    if let width, let height {
      let nextWidth = max(16, width)
      let nextHeight = max(16, height)
      let currentDimensions = context.texture?.dimensions()
      let willChange = currentDimensions?.width != nextWidth ||
        currentDimensions?.height != nextHeight
      if context.nativeBackendActive, willChange {
        context.player?.clearMetalPresentationTarget()
      }
      _ = context.texture?.resize(width: nextWidth, height: nextHeight) ?? false
      if context.nativeBackendActive {
        nativeRefreshAttempted = true
        let refreshed = refreshCurrentFrame(context: context)
        context.playback.reinstallPresentationTargetIfPlaying(
          player: context.player,
          texture: context.nativeTexture,
          maxTrackSlots: context.maxTrackSlots
        )
        if !refreshed {
          return
        }
      }
    }
    if !nativeRefreshAttempted {
      context.markFrameAvailable()
    }
  }

  @discardableResult
  func refreshCurrentFrame(context: MacOSPresentationContext) -> Bool {
    guard context.nativeBackendActive,
          let player = context.player,
          let texture = context.nativeTexture else {
      context.markFrameAvailable()
      return true
    }
    let refreshed = MacOSNativeFrameRefresh.refreshCurrentFrameAfterLayoutChange(
      player: player,
      texture: texture,
      maxTrackSlots: context.maxTrackSlots,
      presentationState: context.presentationState,
      framePump: context.playback.framePumpForRefresh
    )
    if refreshed {
      context.markFrameAvailable()
    }
    return refreshed
  }

  private func cancelPendingLayoutRefreshes() {
    layoutRefreshGeneration += 1
    latestLayoutRefreshContext = nil
    if layoutRefreshRunning {
      layoutRefreshQueue.sync {}
    }
    layoutRefreshRunning = false
  }

  private func requestCoalescedLayoutRefresh(context: MacOSPresentationContext) {
    guard context.nativeBackendActive,
          context.player != nil,
          context.nativeTexture != nil else {
      context.markFrameAvailable()
      return
    }
    layoutRefreshGeneration += 1
    latestLayoutRefreshContext = context
    guard !layoutRefreshRunning else { return }
    layoutRefreshRunning = true
    runLatestLayoutRefresh(generation: layoutRefreshGeneration)
  }

  private func runLatestLayoutRefresh(generation: Int) {
    guard let context = latestLayoutRefreshContext else {
      layoutRefreshRunning = false
      return
    }
    layoutRefreshQueue.async { [weak self, context, generation] in
      let outcome = Self.performLayoutRefresh(context: context)
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        if generation == self.layoutRefreshGeneration {
          switch outcome {
          case .refreshed(let frameInfo, let targetInstalled):
            context.playback.framePumpForRefresh.setTargetInstalled(targetInstalled)
            context.presentationState.recordFrame(frameInfo)
            context.markFrameAvailable()
          case .transientMiss:
            context.presentationState.recordMiss()
          case .failed(let error):
            NSLog("VoidPlayer macOS native layout refresh failed: \(error)")
          }
        }
        if generation != self.layoutRefreshGeneration {
          self.runLatestLayoutRefresh(generation: self.layoutRefreshGeneration)
        } else {
          self.layoutRefreshRunning = false
        }
      }
    }
  }

  private static func performLayoutRefresh(
    context: MacOSPresentationContext
  ) -> LayoutRefreshOutcome {
    guard let player = context.player,
          let texture = context.nativeTexture else {
      return .transientMiss
    }
    do {
      let frameInfo = try texture.updateFromNativePlayer(
        player,
        maxTrackSlots: context.maxTrackSlots,
        waitTimeoutMs: 100
      )
      return .refreshed(
        frameInfo: frameInfo,
        targetInstalled: player.rendererOwnedPresentationActive()
      )
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        return .transientMiss
      }
      return .failed(error)
    }
  }
}

private enum LayoutRefreshOutcome {
  case refreshed(frameInfo: MacOSNativeFrameInfo, targetInstalled: Bool)
  case transientMiss
  case failed(Error)
}
