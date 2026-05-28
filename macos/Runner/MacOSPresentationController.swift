import Foundation

struct MacOSPresentationContext {
  let nativeBackendActive: Bool
  let player: MacOSNativePlayerSession?
  let texture: MacOSVideoTexture?
  let nativeTexture: MacOSFlutterTextureBridge?
  let maxTrackSlots: Int
  let playback: MacOSPlaybackController
  let presentationState: MacOSFramePresentationState
  let userData: UnsafeMutableRawPointer
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
  private var latestLayoutRefreshRequest: LayoutRefreshRequest?

  func resetLayout() {
    cancelPendingLayoutRefreshes()
    layout = MacOSVideoTrackPayload.defaultLayout()
  }

  func applyLayout(arguments: Any?, context: MacOSPresentationContext) {
    guard let nextLayout = MacOSNativeLayoutBridge.layoutMap(arguments: arguments) else {
      return
    }
    layout = nextLayout
    if context.nativeBackendActive,
       context.playback.currentIsPlaying(player: context.player) {
      invalidatePendingLayoutRefreshes()
      MacOSNativeLayoutBridge.apply(layout: nextLayout, player: context.player)
      return
    }
    requestCoalescedLayoutRefresh(context: context, layout: nextLayout)
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
    latestLayoutRefreshRequest = nil
    if layoutRefreshRunning {
      layoutRefreshQueue.sync {}
    }
    layoutRefreshRunning = false
  }

  private func invalidatePendingLayoutRefreshes() {
    layoutRefreshGeneration += 1
    latestLayoutRefreshRequest = nil
  }

  private func requestCoalescedLayoutRefresh(
    context: MacOSPresentationContext,
    layout: [String: Any]
  ) {
    guard context.nativeBackendActive,
          context.player != nil,
          context.nativeTexture != nil else {
      context.markFrameAvailable()
      return
    }
    layoutRefreshGeneration += 1
    latestLayoutRefreshRequest = LayoutRefreshRequest(context: context, layout: layout)
    guard !layoutRefreshRunning else { return }
    layoutRefreshRunning = true
    runLatestLayoutRefresh(generation: layoutRefreshGeneration)
  }

  private func runLatestLayoutRefresh(generation: Int) {
    guard let request = latestLayoutRefreshRequest else {
      layoutRefreshRunning = false
      return
    }
    layoutRefreshQueue.async { [weak self, request, generation] in
      let outcome = Self.performLayoutRefresh(request: request)
      DispatchQueue.main.async { [weak self] in
        guard let self else { return }
        if generation == self.layoutRefreshGeneration {
          switch outcome {
          case .applied:
            break
          case .transientMiss:
            request.context.presentationState.recordMiss()
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
    request: LayoutRefreshRequest
  ) -> LayoutRefreshOutcome {
    let context = request.context
    guard let player = context.player else {
      return .transientMiss
    }
    let pumpReady = context.playback.ensurePresentationPump(
      player: player,
      texture: context.nativeTexture,
      maxTrackSlots: context.maxTrackSlots,
      userData: context.userData,
      presentationState: context.presentationState
    )
    guard pumpReady else {
      return .transientMiss
    }
    MacOSNativeLayoutBridge.apply(layout: request.layout, player: player)
    return .applied
  }
}

private struct LayoutRefreshRequest {
  let context: MacOSPresentationContext
  let layout: [String: Any]
}

private enum LayoutRefreshOutcome {
  case applied
  case transientMiss
}
