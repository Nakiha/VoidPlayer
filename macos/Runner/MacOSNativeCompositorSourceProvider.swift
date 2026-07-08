import Foundation

final class MacOSNativeCompositorSourceProvider {
  private weak var compositor: MacOSNativeCompositorView?
  private var ring: MacOSNativeCompositorSourceRing?
  private var signature = ""
  private var expectedFileIds: [Int] = []
  private var topologyRevision: UInt64 = 0
  private var topologyCommitLastError = ""

  var hasConfiguredSource: Bool {
    ring != nil && !signature.isEmpty
  }

  var configuredExpectedFileIds: [Int] {
    expectedFileIds
  }

  func attach(compositor: MacOSNativeCompositorView) {
    self.compositor = compositor
    ring = MacOSNativeCompositorSourceRing(compositor: compositor)
    signature = ""
    expectedFileIds = []
    topologyCommitLastError = ""
  }

  func detach(reason: String) {
    clear(reason: reason)
    ring = nil
    compositor = nil
  }

  func clear(reason: String) {
    ring?.unsubscribe(reason: reason)
    signature = ""
    expectedFileIds = []
  }

  func ready(nativeBackendActive: Bool) -> Bool {
    guard nativeBackendActive,
          compositor != nil,
          let ring,
          !signature.isEmpty else {
      return false
    }
    return ring.hasCompletePackage(topologyRevision: topologyRevision)
  }

  func shouldUseProjectionLayout(
    nativeBackendActive: Bool,
    isPlaying: Bool
  ) -> Bool {
    isPlaying && nativeBackendActive && hasConfiguredSource
  }

  func diagnostics(nativeBackendActive: Bool) -> [String: Any] {
    var diagnostics = ring?.diagnostics() ?? [:]
    diagnostics["sourceTopologyRevision"] =
      diagnostics["sourceRingTopologyRevision"] ?? 0
    diagnostics["sourceReadyTopologyRevision"] =
      diagnostics["sourceRingReadyTopologyRevision"] ?? 0
    diagnostics["sourceRequiredMask"] = diagnostics["sourceRingRequiredMask"] ?? 0
    diagnostics["sourceDrawnMask"] = diagnostics["sourceRingDrawnMask"] ?? 0
    diagnostics["sourceMissingMask"] = diagnostics["sourceRingMissingMask"] ?? 0
    diagnostics["sourceIncompletePublishSuppressedCount"] =
      diagnostics["sourceRingIncompletePublishSuppressedCount"] ?? 0
    diagnostics["sourceLastIncompleteReason"] =
      diagnostics["sourceRingLastIncompleteReason"] ?? ""
    diagnostics["sourceTopologyCommitRevision"] =
      Int64(min(topologyRevision, UInt64(Int64.max)))
    let sourceReady = ready(nativeBackendActive: nativeBackendActive)
    diagnostics["sourceReady"] = sourceReady
    diagnostics["sourceTopologyCommitReady"] = sourceReady
    diagnostics["sourceTopologyCommitLastError"] = topologyCommitLastError
    return diagnostics
  }

  @discardableResult
  func subscribe(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    projection: MacOSNativeCompositorSourceProjection,
    isPlaying: Bool,
    edrOutputEnabled: Bool,
    presentationState: MacOSFramePresentationState,
    reason: String
  ) -> Bool {
    guard let compositor, !descriptors.isEmpty else { return false }
    let pixelFormat = edrOutputEnabled ? "edr" : "sdr"
    let nextSignature = ([pixelFormat] +
      descriptors.map { "\($0.slot):\($0.fileId):\($0.width)x\($0.height)" })
      .joined(separator: "|")
    if nextSignature == signature,
       let existingRing = ring {
      compositor.setSourceProjection(
        mode: projection.mode,
        splitPos: projection.splitPos,
        activeTrackCount: projection.activeTrackCount,
        order: projection.order,
        displayOffsetX: projection.displayOffsetX,
        displayOffsetY: projection.displayOffsetY,
        invDisplaySizeX: projection.invDisplaySizeX,
        invDisplaySizeY: projection.invDisplaySizeY,
        viewOffsetUvX: projection.viewOffsetUvX,
        viewOffsetUvY: projection.viewOffsetUvY,
        trace: projection.trace
      )
      existingRing.updateProjection(projection)
      compositor.setOverlayPrimitives(player.currentOverlayPrimitives())
      if ready(nativeBackendActive: true) {
        return true
      }
      topologyCommitLastError = "source provider ring has no complete package"
      return false
    }

    let activeRing = ring ?? MacOSNativeCompositorSourceRing(compositor: compositor)
    ring = activeRing
    let candidateTopologyRevision = topologyRevision &+ 1
    if !isPlaying {
      let previewReady = commitPreview(
        player: player,
        descriptors: descriptors,
        presentationState: presentationState,
        timeoutMs: 3_000,
        reason: reason
      )
      if !previewReady {
        topologyCommitLastError = "source provider preview did not become ready"
        return false
      }
    }
    let result = activeRing.subscribe(
      player: player,
      descriptors: descriptors,
      order: order,
      edrOutputEnabled: edrOutputEnabled,
      topologyRevision: candidateTopologyRevision,
      projection: projection
    )
    guard result.published,
          activeRing.hasCompletePackage(topologyRevision: candidateTopologyRevision) else {
      topologyCommitLastError = result.error.isEmpty
        ? "source provider did not publish a complete source package"
        : result.error
      if MacOSProfilerLog.enabled {
        NSLog(
          "VoidPlayer WGPU source provider subscribe failed reason=\(reason) " +
            "topology=\(candidateTopologyRevision) " +
            "drawnMask=\(result.drawnMask) missingMask=\(result.missingMask) " +
            "error=\(topologyCommitLastError)"
        )
      }
      return false
    }
    topologyRevision = candidateTopologyRevision
    signature = nextSignature
    expectedFileIds = descriptors.map { $0.fileId }
    topologyCommitLastError = ""
    if MacOSProfilerLog.enabled {
      NSLog(
        "VoidPlayer WGPU source provider subscribed reason=\(reason) " +
          "topology=\(topologyRevision)"
      )
    }
    return true
  }

  func requestRefresh(player: MacOSNativePlayerSession) {
    ring?.requestRefresh(player: player)
  }

  @discardableResult
  func requestRefreshIfConfigured(player: MacOSNativePlayerSession) -> Bool {
    guard hasConfiguredSource else { return false }
    player.noteViewportCompositorActivity()
    ring?.requestRefresh(player: player)
    return true
  }

  func publishReadyFrame(
    player: MacOSNativePlayerSession,
    timeoutMs: Int,
    reason: String
  ) -> String? {
    guard ready(nativeBackendActive: true),
          let ring else {
      return "source provider is not ready"
    }
    player.noteViewportCompositorActivity()
    let result = ring.refreshAndWait(player: player, timeoutMs: timeoutMs)
    if result.published {
      if MacOSProfilerLog.enabled {
        NSLog(
          "VoidPlayer WGPU source provider published reason=\(reason) " +
            "publish=\(result.publishCount) ptsUs=\(result.ptsUs) " +
            "drawnMask=\(result.drawnMask) reusedMask=\(result.reusedMask)"
        )
      }
      return nil
    }
    let error = result.error.isEmpty
      ? "source provider publish timed out"
      : result.error
    topologyCommitLastError = error
    if MacOSProfilerLog.enabled {
      NSLog(
        "VoidPlayer WGPU source provider publish failed reason=\(reason) " +
          "publish=\(result.publishCount) drawnMask=\(result.drawnMask) " +
          "missingMask=\(result.missingMask) error=\(error)"
      )
    }
    return error
  }

  private func commitPreview(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    presentationState: MacOSFramePresentationState,
    timeoutMs: Int,
    reason: String
  ) -> Bool {
    let expectedFileIds = descriptors.map { $0.fileId }
    guard !expectedFileIds.isEmpty else { return false }
    do {
      let frame = try player.commitSourceProviderPreview(
        timeoutMs: timeoutMs,
        expectedFileIds: expectedFileIds
      )
      presentationState.recordDiscontinuityFrame(frame)
      return true
    } catch {
      NSLog(
        "VoidPlayer WGPU source provider preview not ready reason=\(reason) expectedFileIds=\(expectedFileIds) error=\(error)"
      )
      return false
    }
  }
}
