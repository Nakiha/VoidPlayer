import CoreVideo
import Foundation

/// Per-track projection metadata + dimensions for a source-cache subscription.
/// Parsed by `MacOSVideoRendererBridge` from the Dart `prepareNativeCompositorSourceCache`
/// arguments and handed to the ring, which owns allocation and baking.
struct MacOSCompositorSourceTrackDescriptor {
  let slot: Int
  let fileId: Int
  let width: Int
  let height: Int
  let displayOffsetX: Float
  let displayOffsetY: Float
  let invDisplaySizeX: Float
  let invDisplaySizeY: Float
  let viewOffsetUvX: Float
  let viewOffsetUvY: Float
}

struct MacOSNativeCompositorSourceRefreshResult {
  let published: Bool
  let publishCount: Int
  let drawnMask: UInt64
  let reusedMask: UInt64
  let missingMask: UInt64
  let ptsUs: Int64
  let error: String
}

/// Owns the retained source package used by the native Metal compositor.
///
/// Producers may ask the ring to bake or refresh a candidate package, but only a
/// complete package is published to the compositor. Failed or incomplete
/// candidates leave the previously published package intact, so display-link
/// ticks can keep sampling a stable ready state instead of seeing blank or
/// half-topology source textures.
final class MacOSNativeCompositorSourceRing {
  private struct TrackRing {
    let slot: Int
    let fileId: Int
    let width: Int
    let height: Int
    let displayOffsetX: Float
    let displayOffsetY: Float
    let invDisplaySizeX: Float
    let invDisplaySizeY: Float
    let viewOffsetUvX: Float
    let viewOffsetUvY: Float
    let buffers: [CVPixelBuffer]
    var writeIndex: Int
    var publishedIndex: Int
  }

  private static let ringDepth = 3
  /// Total ring budget. Above this a live triple ring will not fit, so the
  /// subscription degrades to a single-buffer frozen snapshot (paused-quality
  /// reveal only) rather than allocating hundreds of MB for 8K sources.
  private static let ringBudgetBytes = 384 * 1024 * 1024

  private weak var compositor: MacOSNativeCompositorView?
  private let ringQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.source-ring",
    qos: .userInteractive
  )

  // All of the following are confined to `ringQueue`.
  private var rings: [TrackRing] = []
  private var order: [Int] = []
  private var projection: MacOSNativeCompositorSourceProjection?
  private var bakeTarget: MacOSNativeMetalPresentationTarget?
  private var live = false
  private var baking = false
  private var pending = false
  private var hasPublished = false
  private var depth = MacOSNativeCompositorSourceRing.ringDepth
  private let refreshRequestRate = MacOSRateWindow()
  private let bakeRate = MacOSRateWindow()
  private let publishRate = MacOSRateWindow()
  private let bakeDuration = MacOSDurationWindow()
  private let refreshQueueWaitDuration = MacOSDurationWindow()
  private let requestToPublishDuration = MacOSDurationWindow()
  private let publishedPtsStepDuration = MacOSDurationWindow()
  private var refreshRequestCount = 0
  private var refreshCoalescedCount = 0
  private var bakeCount = 0
  private var publishCount = 0
  private var publishMissCount = 0
  private var lastBakeDrawnCount = 0
  private var lastBakeError = ""
  private var topologyRevision: UInt64 = 0
  private var lastPublishedTopologyRevision: UInt64 = 0
  private var lastRequiredMask: UInt64 = 0
  private var lastDrawnMask: UInt64 = 0
  private var lastMissingMask: UInt64 = 0
  private var lastReusedMask: UInt64 = 0
  private var reusedPublishedSlotCount = 0
  private var incompletePublishSuppressedCount = 0
  private var lastIncompleteReason = ""
  private var lastPublishedSlotSignature = ""
  private var lastPublishedFileIdSignature = ""
  private var lastPublishedActualFileIdSignature = ""
  private var lastPublishedBufferSignature = ""
  private var lastPublishedDuplicateSlotCount = 0
  private var lastPublishedDuplicateFileIdCount = 0
  private var lastPublishedDuplicateActualFileIdCount = 0
  private var lastPublishedDuplicateBufferCount = 0
  private var lastPublishedPtsUs: Int64 = -1
  private var lastPublishedDurationUs: Int64 = 0
  private var publishedPtsDuplicateCount = 0
  private var publishedPtsLargeStepCount = 0
  private var publishedPtsRegressionCount = 0

  init(compositor: MacOSNativeCompositorView) {
    self.compositor = compositor
  }

  /// Sets up the ring for an interaction. Allocates per-track buffers, bakes the
  /// initial frame, and publishes it before returning. Failed candidate
  /// subscriptions do not replace the previously published source package.
  func subscribe(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    edrOutputEnabled: Bool,
    topologyRevision: UInt64,
    projection: MacOSNativeCompositorSourceProjection
  ) -> MacOSNativeCompositorSourceRefreshResult {
    let semaphore = DispatchSemaphore(value: 0)
    var result = MacOSNativeCompositorSourceRefreshResult(
      published: false,
      publishCount: 0,
      drawnMask: 0,
      reusedMask: 0,
      missingMask: 0,
      ptsUs: -1,
      error: "source ring subscribe did not run"
    )
    ringQueue.async { [weak self] in
      if let self {
        result = self.subscribeOnQueue(
          player: player,
          descriptors: descriptors,
          order: order,
          edrOutputEnabled: edrOutputEnabled,
          topologyRevision: topologyRevision,
          projection: projection
        )
      }
      semaphore.signal()
    }
    semaphore.wait()
    return result
  }

  func hasCompletePackage(topologyRevision expectedTopologyRevision: UInt64) -> Bool {
    ringQueue.sync {
      live &&
        hasPublished &&
        !rings.isEmpty &&
        lastPublishedTopologyRevision == expectedTopologyRevision
    }
  }

  func updateProjection(_ projection: MacOSNativeCompositorSourceProjection) {
    ringQueue.async { [weak self] in
      self?.projection = projection
    }
  }

  /// Requests a re-bake of the current frame into the next ring buffers. Coalesced
  /// on the ring queue: a burst of frame callbacks collapses into baking the
  /// latest available frame. No-op when not subscribed.
  func requestRefresh(player: MacOSNativePlayerSession) {
    let requestedNs = DispatchTime.now().uptimeNanoseconds
    ringQueue.async { [weak self] in
      guard let self, self.live, !self.rings.isEmpty else { return }
      self.recordRefreshRequest(requestedNs: requestedNs)
      // Frozen-snapshot fallback (depth == 1) never refreshes: the source is too
      // large for a live ring, so it intentionally behaves like the old paused
      // one-shot.
      if self.depth <= 1 { return }
      if self.baking {
        self.refreshCoalescedCount += 1
        self.pending = true
        return
      }
      self.baking = true
      repeat {
        self.pending = false
        _ = self.bakeAndPublishOnQueue(player: player, requestNs: requestedNs)
      } while self.pending && self.live
      self.baking = false
    }
  }

  func refreshAndWait(
    player: MacOSNativePlayerSession,
    timeoutMs: Int
  ) -> MacOSNativeCompositorSourceRefreshResult {
    let requestedNs = DispatchTime.now().uptimeNanoseconds
    let timeout = DispatchTimeInterval.milliseconds(max(0, timeoutMs))
    let semaphore = DispatchSemaphore(value: 0)
    var result = MacOSNativeCompositorSourceRefreshResult(
      published: false,
      publishCount: 0,
      drawnMask: 0,
      reusedMask: 0,
      missingMask: 0,
      ptsUs: -1,
      error: "source ring refresh timed out"
    )

    ringQueue.async { [weak self] in
      guard let self else {
        semaphore.signal()
        return
      }
      guard self.live, !self.rings.isEmpty else {
        result = MacOSNativeCompositorSourceRefreshResult(
          published: false,
          publishCount: self.publishCount,
          drawnMask: 0,
          reusedMask: 0,
          missingMask: self.lastRequiredMask,
          ptsUs: self.lastPublishedPtsUs,
          error: "source ring is not live"
        )
        semaphore.signal()
        return
      }
      self.recordRefreshRequest(requestedNs: requestedNs)
      if self.depth <= 1 {
        result = MacOSNativeCompositorSourceRefreshResult(
          published: false,
          publishCount: self.publishCount,
          drawnMask: 0,
          reusedMask: 0,
          missingMask: self.lastRequiredMask,
          ptsUs: self.lastPublishedPtsUs,
          error: "source ring is frozen"
        )
        semaphore.signal()
        return
      }
      if self.baking {
        self.refreshCoalescedCount += 1
        self.pending = true
        result = MacOSNativeCompositorSourceRefreshResult(
          published: false,
          publishCount: self.publishCount,
          drawnMask: self.lastDrawnMask,
          reusedMask: self.lastReusedMask,
          missingMask: self.lastMissingMask,
          ptsUs: self.lastPublishedPtsUs,
          error: "source ring is already baking"
        )
        semaphore.signal()
        return
      }
      self.baking = true
      self.pending = false
      result = self.bakeAndPublishOnQueue(player: player, requestNs: requestedNs)
      self.baking = false
      semaphore.signal()
    }

    if semaphore.wait(timeout: DispatchTime.now() + timeout) == .timedOut {
      return result
    }
    return result
  }

  /// Tears down the ring and clears the compositor source cache.
  func unsubscribe(reason: String) {
    ringQueue.async { [weak self] in
      guard let self else { return }
      self.live = false
      self.rings = []
      self.order = []
      self.projection = nil
      self.bakeTarget = nil
      self.compositor?.clearSource(reason: reason)
    }
  }

  func diagnostics() -> [String: Any] {
    ringQueue.sync {
      [
        "sourceRingLive": live,
        "sourceRingHasPublished": hasPublished,
        "sourceRingComplete": live && hasPublished && !rings.isEmpty,
        "sourceRingDepth": depth,
        "sourceRingTrackCount": rings.count,
        "sourceRingRefreshRequestCount": refreshRequestCount,
        "sourceRingRefreshRequestHz": refreshRequestRate.rateHz(),
        "sourceRingRefreshQueueWaitP95Ms": refreshQueueWaitDuration.p95Ms(),
        "sourceRingRefreshQueueWaitLastMs": refreshQueueWaitDuration.lastMs(),
        "sourceRingRefreshCoalescedCount": refreshCoalescedCount,
        "sourceRingBaking": baking,
        "sourceRingPending": pending,
        "sourceRingBakeCount": bakeCount,
        "sourceRingBakeHz": bakeRate.rateHz(),
        "sourceRingBakeP95Ms": bakeDuration.p95Ms(),
        "sourceRingBakeLastMs": bakeDuration.lastMs(),
        "sourceRingLastBakeDrawnCount": lastBakeDrawnCount,
        "sourceRingLastBakeError": lastBakeError,
        "sourceRingTopologyRevision": Int64(min(topologyRevision, UInt64(Int64.max))),
        "sourceRingReadyTopologyRevision": Int64(
          min(lastPublishedTopologyRevision, UInt64(Int64.max))
        ),
        "sourceRingRequiredMask": Int64(min(lastRequiredMask, UInt64(Int64.max))),
        "sourceRingDrawnMask": Int64(min(lastDrawnMask, UInt64(Int64.max))),
        "sourceRingMissingMask": Int64(min(lastMissingMask, UInt64(Int64.max))),
        "sourceRingReusedMask": Int64(min(lastReusedMask, UInt64(Int64.max))),
        "sourceRingReusedPublishedSlotCount": reusedPublishedSlotCount,
        "sourceRingIncompletePublishSuppressedCount": incompletePublishSuppressedCount,
        "sourceRingLastIncompleteReason": lastIncompleteReason,
        "sourceRingPublishedSlotSignature": lastPublishedSlotSignature,
        "sourceRingPublishedFileIdSignature": lastPublishedFileIdSignature,
        "sourceRingPublishedActualFileIdSignature": lastPublishedActualFileIdSignature,
        "sourceRingPublishedBufferSignature": lastPublishedBufferSignature,
        "sourceRingPublishedDuplicateSlotCount": lastPublishedDuplicateSlotCount,
        "sourceRingPublishedDuplicateFileIdCount": lastPublishedDuplicateFileIdCount,
        "sourceRingPublishedDuplicateActualFileIdCount": lastPublishedDuplicateActualFileIdCount,
        "sourceRingPublishedDuplicateBufferCount": lastPublishedDuplicateBufferCount,
        "sourceRingPublishCount": publishCount,
        "sourceRingPublishHz": publishRate.rateHz(),
        "sourceRingRequestToPublishP95Ms": requestToPublishDuration.p95Ms(),
        "sourceRingRequestToPublishLastMs": requestToPublishDuration.lastMs(),
        "sourceRingPublishMissCount": publishMissCount,
        "sourceRingLastPublishedPtsUs": lastPublishedPtsUs,
        "sourceRingLastPublishedDurationUs": lastPublishedDurationUs,
        "sourceRingPublishedPtsStepP95Ms": publishedPtsStepDuration.p95Ms(),
        "sourceRingPublishedPtsStepLastMs": publishedPtsStepDuration.lastMs(),
        "sourceRingPublishedPtsDuplicateCount": publishedPtsDuplicateCount,
        "sourceRingPublishedPtsLargeStepCount": publishedPtsLargeStepCount,
        "sourceRingPublishedPtsRegressionCount": publishedPtsRegressionCount,
      ]
    }
  }

  // MARK: - Ring queue internals

  private func subscribeOnQueue(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    edrOutputEnabled: Bool,
    topologyRevision: UInt64,
    projection: MacOSNativeCompositorSourceProjection
  ) -> MacOSNativeCompositorSourceRefreshResult {
    let previousRings = rings
    let previousOrder = self.order
    let previousProjection = self.projection
    let previousBakeTarget = bakeTarget
    let previousLive = live
    let previousDepth = depth
    let previousHasPublished = hasPublished
    let previousTopologyRevision = self.topologyRevision
    let previousPublishedTopologyRevision = lastPublishedTopologyRevision

    func restorePreviousPublishedState() {
      rings = previousRings
      self.order = previousOrder
      self.projection = previousProjection
      bakeTarget = previousBakeTarget
      live = previousLive
      depth = previousDepth
      hasPublished = previousHasPublished
      self.topologyRevision = previousTopologyRevision
      lastPublishedTopologyRevision = previousPublishedTopologyRevision
      baking = false
      pending = false
    }

    func failure(_ message: String, requiredMask: UInt64 = 0) -> MacOSNativeCompositorSourceRefreshResult {
      lastIncompleteReason = message
      if requiredMask != 0 {
        lastRequiredMask = requiredMask
        lastMissingMask = requiredMask
      }
      restorePreviousPublishedState()
      return MacOSNativeCompositorSourceRefreshResult(
        published: false,
        publishCount: publishCount,
        drawnMask: lastDrawnMask,
        reusedMask: lastReusedMask,
        missingMask: requiredMask != 0 ? requiredMask : lastMissingMask,
        ptsUs: lastPublishedPtsUs,
        error: message
      )
    }

    guard !descriptors.isEmpty else {
      return failure("no source tracks")
    }

    let pixelFormat: OSType = edrOutputEnabled
      ? kCVPixelFormatType_64RGBAHalf
      : kCVPixelFormatType_32BGRA
    let bytesPerPixel = edrOutputEnabled ? 8 : 4

    // Decide ring depth within budget; degrade to a frozen single buffer if a
    // live triple ring would blow the cap (e.g. 8K sources).
    var perFrameBytes = 0
    for d in descriptors {
      perFrameBytes += d.width * d.height * bytesPerPixel
    }
    if perFrameBytes <= 0 {
      return failure("no source cache targets")
    }
    var chosenDepth = Self.ringDepth
    if perFrameBytes * chosenDepth > Self.ringBudgetBytes {
      chosenDepth = 1
    }
    if perFrameBytes * chosenDepth > Self.ringBudgetBytes {
      return failure("source cache memory cap exceeded")
    }
    depth = chosenDepth

    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    var built: [TrackRing] = []
    var maxWidth = 1
    var maxHeight = 1
    for d in descriptors {
      guard d.width > 0, d.height > 0 else { continue }
      var buffers: [CVPixelBuffer] = []
      var ok = true
      for _ in 0..<chosenDepth {
        var pixelBuffer: CVPixelBuffer?
        let status = CVPixelBufferCreate(
          kCFAllocatorDefault, d.width, d.height, pixelFormat, attributes, &pixelBuffer
        )
        guard status == kCVReturnSuccess, let pixelBuffer else {
          ok = false
          break
        }
        buffers.append(pixelBuffer)
      }
      guard ok, !buffers.isEmpty else {
        return failure("failed to allocate source cache pixel buffer")
      }
      maxWidth = max(maxWidth, d.width)
      maxHeight = max(maxHeight, d.height)
      built.append(TrackRing(
        slot: d.slot,
        fileId: d.fileId,
        width: d.width,
        height: d.height,
        displayOffsetX: d.displayOffsetX,
        displayOffsetY: d.displayOffsetY,
        invDisplaySizeX: d.invDisplaySizeX,
        invDisplaySizeY: d.invDisplaySizeY,
        viewOffsetUvX: d.viewOffsetUvX,
        viewOffsetUvY: d.viewOffsetUvY,
        buffers: buffers,
        writeIndex: 0,
        publishedIndex: 0
      ))
    }

    guard !built.isEmpty else {
      return failure("no source cache targets")
    }

    rings = built
    self.order = order
    self.topologyRevision = topologyRevision
    self.projection = projection
    bakeTarget = MacOSNativeMetalPresentationTarget(width: maxWidth, height: maxHeight)
    live = true
    baking = false
    pending = false
    hasPublished = false
    lastReusedMask = 0

    // Initial bake into buffer 0 of each ring; publishes on success.
    let result = bakeAndPublishOnQueue(player: player, requestNs: nil)
    if !result.published {
      restorePreviousPublishedState()
    }
    return result
  }

  /// Bakes the current frame into each track's next write buffer and, if any
  /// track drew, publishes the just-baked buffers to the compositor. Runs on the
  /// ring queue. The bake is synchronous (waits for GPU completion), so the buffer
  /// is fully written before it is published.
  private func recordRefreshRequest(requestedNs: UInt64) {
    let queueStartNs = DispatchTime.now().uptimeNanoseconds
    refreshQueueWaitDuration.record(
      queueStartNs >= requestedNs ? queueStartNs - requestedNs : 0
    )
    refreshRequestCount += 1
    refreshRequestRate.record(nowNs: queueStartNs)
  }

  private func bakeAndPublishOnQueue(
    player: MacOSNativePlayerSession,
    requestNs: UInt64?
  ) -> MacOSNativeCompositorSourceRefreshResult {
    guard live, let bakeTarget, !rings.isEmpty else {
      return MacOSNativeCompositorSourceRefreshResult(
        published: false,
        publishCount: publishCount,
        drawnMask: 0,
        reusedMask: 0,
        missingMask: lastRequiredMask,
        ptsUs: lastPublishedPtsUs,
        error: "source ring is not ready"
      )
    }
    let startNs = DispatchTime.now().uptimeNanoseconds

    // Advance each ring to the next write buffer (away from the published one).
    for i in rings.indices {
      rings[i].writeIndex = (rings[i].publishedIndex + 1) % rings[i].buffers.count
    }

    var targets: [VPMacOSNativeSourceFrameBakeTarget] = []
    targets.reserveCapacity(rings.count)
    for ring in rings {
      let buffer = ring.buffers[ring.writeIndex]
      var target = VPMacOSNativeSourceFrameBakeTarget()
      target.pixel_buffer = UnsafeMutableRawPointer(
        Unmanaged.passUnretained(buffer).toOpaque()
      )
      target.source_slot = Int32(ring.slot)
      target.source_file_id = Int32(ring.fileId)
      target.width = Int32(ring.width)
      target.height = Int32(ring.height)
      target.drawn = 0
      VPMacOSNativeFrameInfoInit(&target.frame_info)
      targets.append(target)
    }

    let result = targets.withUnsafeMutableBufferPointer { buffer in
      bakeTarget.bakeCurrentFrameSources(player: player, targets: buffer)
    }
    let finishNs = DispatchTime.now().uptimeNanoseconds
    bakeCount += 1
    bakeRate.record(nowNs: finishNs)
    bakeDuration.record(finishNs >= startNs ? finishNs - startNs : 0)
    lastBakeDrawnCount = result.drawnCount
    lastBakeError = result.error
    guard result.drawnCount > 0 else {
      publishMissCount += 1
      lastRequiredMask = requiredMask(for: targets.count)
      lastDrawnMask = 0
      lastReusedMask = 0
      lastMissingMask = lastRequiredMask
      lastIncompleteReason = result.error.isEmpty
        ? "source frame bake produced no frames"
        : result.error
      return MacOSNativeCompositorSourceRefreshResult(
        published: false,
        publishCount: publishCount,
        drawnMask: 0,
        reusedMask: 0,
        missingMask: lastMissingMask,
        ptsUs: lastPublishedPtsUs,
        error: lastIncompleteReason
      )
    }

    let requiredMask = requiredMask(for: targets.count)
    var drawnMask: UInt64 = 0
    for i in targets.indices where targets[i].drawn != 0 {
      drawnMask |= UInt64(1) << UInt64(i)
    }
    let missingMask = requiredMask & ~drawnMask
    lastRequiredMask = requiredMask
    lastDrawnMask = drawnMask
    lastReusedMask = 0
    guard missingMask == 0 else {
      lastMissingMask = missingMask
      incompletePublishSuppressedCount += 1
      lastIncompleteReason =
        "source package incomplete requiredMask=\(requiredMask) drawnMask=\(drawnMask) missingMask=\(missingMask)"
      return MacOSNativeCompositorSourceRefreshResult(
        published: false,
        publishCount: publishCount,
        drawnMask: drawnMask,
        reusedMask: 0,
        missingMask: missingMask,
        ptsUs: lastPublishedPtsUs,
        error: lastIncompleteReason
      )
    }
    lastMissingMask = 0
    lastIncompleteReason = ""
    guard let projection else {
      lastIncompleteReason = "source package projection is not ready"
      return MacOSNativeCompositorSourceRefreshResult(
        published: false,
        publishCount: publishCount,
        drawnMask: drawnMask,
        reusedMask: 0,
        missingMask: 0,
        ptsUs: lastPublishedPtsUs,
        error: lastIncompleteReason
      )
    }

    var published: [MacOSNativeCompositorSourceTexture] = []
    published.reserveCapacity(rings.count)
    var actualFileIds: [Int] = []
    var publishPtsUs: Int64 = -1
    var publishDurationUs: Int64 = 0
    for i in rings.indices {
      // A target is at the same array order as `rings`; mark drawn ones published.
      if targets[i].drawn != 0 {
        rings[i].publishedIndex = rings[i].writeIndex
        if publishPtsUs < 0 {
          publishPtsUs = Int64(targets[i].frame_info.pts_us)
          publishDurationUs = Int64(targets[i].frame_info.duration_us)
        }
      }
      let ring = rings[i]
      actualFileIds.append(Int(targets[i].source_file_id))
      published.append(MacOSNativeCompositorSourceTexture(
        pixelBuffer: ring.buffers[ring.publishedIndex],
        sourceSlot: ring.slot,
        fileId: ring.fileId,
        width: ring.width,
        height: ring.height
      ))
    }
    hasPublished = true
    lastPublishedTopologyRevision = topologyRevision
    let publishedIdentity = sourceIdentity(from: published)
    lastPublishedSlotSignature = publishedIdentity.slotSignature
    lastPublishedFileIdSignature = publishedIdentity.fileIdSignature
    lastPublishedActualFileIdSignature = actualFileIds.map(String.init).joined(separator: ",")
    lastPublishedBufferSignature = publishedIdentity.bufferSignature
    lastPublishedDuplicateSlotCount = publishedIdentity.duplicateSlotCount
    lastPublishedDuplicateFileIdCount = publishedIdentity.duplicateFileIdCount
    lastPublishedDuplicateActualFileIdCount = max(0, actualFileIds.count - Set(actualFileIds).count)
    lastPublishedDuplicateBufferCount = publishedIdentity.duplicateBufferCount
    publishCount += 1
    let publishNs = DispatchTime.now().uptimeNanoseconds
    publishRate.record(nowNs: publishNs)
    if let requestNs {
      requestToPublishDuration.record(publishNs >= requestNs ? publishNs - requestNs : 0)
    }
    if publishPtsUs >= 0 {
      if lastPublishedPtsUs >= 0 {
        let stepUs = publishPtsUs - lastPublishedPtsUs
        if stepUs < 0 {
          publishedPtsRegressionCount += 1
        } else {
          if stepUs == 0 {
            publishedPtsDuplicateCount += 1
          } else if stepUs > 50_000 {
            publishedPtsLargeStepCount += 1
          }
          publishedPtsStepDuration.record(UInt64(stepUs) * 1_000)
        }
      }
      lastPublishedPtsUs = publishPtsUs
      lastPublishedDurationUs = publishDurationUs
    }
    compositor?.setSourcePackage(
      textures: published,
      projection: projection,
      overlay: player.currentOverlayPrimitives()
    )
    return MacOSNativeCompositorSourceRefreshResult(
      published: true,
      publishCount: publishCount,
      drawnMask: drawnMask,
      reusedMask: 0,
      missingMask: 0,
      ptsUs: lastPublishedPtsUs,
      error: ""
    )
  }

  private func requiredMask(for count: Int) -> UInt64 {
    guard count > 0 else { return 0 }
    let clamped = min(count, 63)
    return (UInt64(1) << UInt64(clamped)) - 1
  }

  private func sourceIdentity(
    from textures: [MacOSNativeCompositorSourceTexture]
  ) -> (
    slotSignature: String,
    fileIdSignature: String,
    bufferSignature: String,
    duplicateSlotCount: Int,
    duplicateFileIdCount: Int,
    duplicateBufferCount: Int
  ) {
    let sorted = textures.sorted { lhs, rhs in
      if lhs.sourceSlot != rhs.sourceSlot {
        return lhs.sourceSlot < rhs.sourceSlot
      }
      return lhs.fileId < rhs.fileId
    }
    var fileIds: [Int] = []
    var slots: [Int] = []
    var bufferIds: [UInt] = []
    var slotParts: [String] = []
    for texture in sorted {
      let bufferId = UInt(bitPattern: Unmanaged.passUnretained(texture.pixelBuffer).toOpaque())
      slots.append(texture.sourceSlot)
      fileIds.append(texture.fileId)
      bufferIds.append(bufferId)
      slotParts.append(
        "s\(texture.sourceSlot):f\(texture.fileId):b\(String(bufferId, radix: 16))"
      )
    }
    let duplicateSlotCount = max(0, slots.count - Set(slots).count)
    let duplicateFileIdCount = max(0, fileIds.count - Set(fileIds).count)
    let duplicateBufferCount = max(0, bufferIds.count - Set(bufferIds).count)
    return (
      slotSignature: slotParts.joined(separator: "|"),
      fileIdSignature: fileIds.map(String.init).joined(separator: ","),
      bufferSignature: bufferIds.map { String($0, radix: 16) }.joined(separator: ","),
      duplicateSlotCount: duplicateSlotCount,
      duplicateFileIdCount: duplicateFileIdCount,
      duplicateBufferCount: duplicateBufferCount
    )
  }
}
