import CoreVideo
import Foundation

protocol MacOSNativeCompositorSourceRingDelegate: AnyObject {
  func sourceRingInitialPublishCompleted(signature: String, success: Bool)
}

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

/// Owns the live source-cache ring used during a viewport pan/zoom interaction.
///
/// Paused and playing share one mechanism: at interaction start the bridge
/// subscribes; the ring renders each track's current frame at source resolution
/// (identity layout) into a per-track triple-buffered ring and publishes the
/// just-baked buffer to the compositor via `setSourceBuffers`. While playing, the
/// bridge calls `requestRefresh` on every presented frame so the ring mirrors the
/// live frame — that is what lets playing-state pan reveal slid-to pixels
/// immediately, exactly like paused. While paused, frames do not advance, so the
/// single initial bake stays valid and no refresh fires.
///
/// The compositor owns projection separately; the ring only swaps in the
/// freshly-baked published buffer each frame. Triple-buffering plus the ring
/// holding strong refs means the compositor never reads a buffer the ring is
/// mid-writing and buffers are never freed mid-read.
final class MacOSNativeCompositorSourceRing: MacOSNativeCompositorSourceReadyCompletionTarget {
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
  weak var delegate: MacOSNativeCompositorSourceRingDelegate?
  private let ringQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.source-ring",
    qos: .userInteractive
  )

  // All of the following are confined to `ringQueue`.
  private var rings: [TrackRing] = []
  private var order: [Int] = []
  private var bakeTarget: MacOSNativeMetalPresentationTarget?
  private var live = false
  private var baking = false
  private var pending = false
  private var hasPublished = false
  private var pendingInitialProjection: MacOSNativeCompositorSourceProjection?
  private var pendingInitialSignature = ""
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
  private var lastRequiredMask: UInt64 = 0
  private var lastDrawnMask: UInt64 = 0
  private var lastMissingMask: UInt64 = 0
  private var incompletePublishSuppressedCount = 0
  private var lastIncompleteReason = ""
  private var lastPublishedPtsUs: Int64 = -1
  private var lastPublishedDurationUs: Int64 = 0
  private var publishedPtsDuplicateCount = 0
  private var publishedPtsLargeStepCount = 0
  private var publishedPtsRegressionCount = 0

  init(
    compositor: MacOSNativeCompositorView,
    delegate: MacOSNativeCompositorSourceRingDelegate? = nil
  ) {
    self.compositor = compositor
    self.delegate = delegate
  }

  /// Sets up the ring for an interaction. Allocates per-track buffers, bakes the
  /// initial frame, and publishes it. Returns false (and reports the error to the
  /// compositor) when allocation/bake fails so the bridge can clear cleanly.
  func subscribe(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    edrOutputEnabled: Bool,
    projection: MacOSNativeCompositorSourceProjection? = nil,
    topologySignature: String = ""
  ) {
    ringQueue.async { [weak self] in
      self?.subscribeOnQueue(
        player: player,
        descriptors: descriptors,
        order: order,
        edrOutputEnabled: edrOutputEnabled,
        projection: projection,
        topologySignature: topologySignature
      )
    }
  }

  func nativeCompositorSourceReadyCompletion(success: Bool) {
    ringQueue.async { [weak self] in
      guard let self else { return }
      let signature = self.pendingInitialSignature
      guard !signature.isEmpty else {
        if success {
          self.hasPublished = true
        }
        return
      }
      if success {
        self.hasPublished = true
      }
      self.pendingInitialProjection = nil
      self.pendingInitialSignature = ""
      DispatchQueue.main.async { [weak self] in
        self?.delegate?.sourceRingInitialPublishCompleted(
          signature: signature,
          success: success
        )
      }
    }
  }

  func updatePendingInitialProjection(_ projection: MacOSNativeCompositorSourceProjection) {
    ringQueue.async { [weak self] in
      guard let self, self.live, !self.hasPublished else { return }
      self.pendingInitialProjection = projection
    }
  }

  /// Requests a re-bake of the current frame into the next ring buffers. Coalesced
  /// on the ring queue: a burst of frame callbacks collapses into baking the
  /// latest available frame. No-op when not subscribed.
  func requestRefresh(player: MacOSNativePlayerSession) {
    let requestedNs = DispatchTime.now().uptimeNanoseconds
    ringQueue.async { [weak self] in
      guard let self, self.live, !self.rings.isEmpty else { return }
      let queueStartNs = DispatchTime.now().uptimeNanoseconds
      self.refreshQueueWaitDuration.record(
        queueStartNs >= requestedNs ? queueStartNs - requestedNs : 0
      )
      self.refreshRequestCount += 1
      self.refreshRequestRate.record(nowNs: queueStartNs)
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
        self.bakeAndPublishOnQueue(
          player: player,
          requestNs: requestedNs,
          projection: self.hasPublished ? nil : self.pendingInitialProjection
        )
      } while self.pending && self.live
      self.baking = false
    }
  }

  /// Tears down the ring and clears the compositor source cache.
  func unsubscribe(reason: String) {
    ringQueue.async { [weak self] in
      guard let self else { return }
      self.live = false
      self.rings = []
      self.order = []
      self.bakeTarget = nil
      self.pendingInitialProjection = nil
      self.pendingInitialSignature = ""
      self.compositor?.clearSource(reason: reason)
    }
  }

  func diagnostics() -> [String: Any] {
    ringQueue.sync {
      [
        "sourceRingLive": live,
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
        "sourceRingRequiredMask": Int64(min(lastRequiredMask, UInt64(Int64.max))),
        "sourceRingDrawnMask": Int64(min(lastDrawnMask, UInt64(Int64.max))),
        "sourceRingMissingMask": Int64(min(lastMissingMask, UInt64(Int64.max))),
        "sourceRingIncompletePublishSuppressedCount": incompletePublishSuppressedCount,
        "sourceRingLastIncompleteReason": lastIncompleteReason,
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
    projection: MacOSNativeCompositorSourceProjection?,
    topologySignature: String
  ) {
    live = false
    rings = []
    self.order = order
    pendingInitialProjection = projection
    pendingInitialSignature = topologySignature

    guard !descriptors.isEmpty else {
      compositor?.setSourceBuffers(textures: [], overlay: .empty, error: "no source tracks")
      completeInitialPublishOnQueue(success: false)
      return
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
      compositor?.setSourceBuffers(textures: [], overlay: .empty, error: "no source cache targets")
      completeInitialPublishOnQueue(success: false)
      return
    }
    var chosenDepth = Self.ringDepth
    if perFrameBytes * chosenDepth > Self.ringBudgetBytes {
      chosenDepth = 1
    }
    if perFrameBytes * chosenDepth > Self.ringBudgetBytes {
      compositor?.setSourceBuffers(
        textures: [],
        overlay: .empty,
        error: "source cache memory cap exceeded"
      )
      completeInitialPublishOnQueue(success: false)
      return
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
        compositor?.setSourceBuffers(
          textures: [],
          overlay: .empty,
          error: "failed to allocate source cache pixel buffer"
        )
        completeInitialPublishOnQueue(success: false)
        return
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
      compositor?.setSourceBuffers(textures: [], overlay: .empty, error: "no source cache targets")
      completeInitialPublishOnQueue(success: false)
      return
    }

    rings = built
    self.order = order
    bakeTarget = MacOSNativeMetalPresentationTarget(width: maxWidth, height: maxHeight)
    live = true
    baking = false
    pending = false
    hasPublished = false

    // Initial bake into buffer 0 of each ring; publishes on success.
    bakeAndPublishOnQueue(
      player: player,
      requestNs: nil,
      projection: pendingInitialProjection
    )
  }

  /// Bakes the current frame into each track's next write buffer and, if any
  /// track drew, publishes the just-baked buffers to the compositor. Runs on the
  /// ring queue. The bake is synchronous (waits for GPU completion), so the buffer
  /// is fully written before it is published.
  private func bakeAndPublishOnQueue(
    player: MacOSNativePlayerSession,
    requestNs: UInt64?,
    projection: MacOSNativeCompositorSourceProjection? = nil
  ) {
    guard live, let bakeTarget, !rings.isEmpty else {
      completeInitialPublishOnQueue(success: false)
      return
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
      lastMissingMask = lastRequiredMask
      lastIncompleteReason = result.error.isEmpty
        ? "source frame bake produced no frames"
        : result.error
      return
    }

    let requiredMask = requiredMask(for: targets.count)
    var drawnMask: UInt64 = 0
    for i in targets.indices where targets[i].drawn != 0 {
      drawnMask |= UInt64(1) << UInt64(i)
    }
    let missingMask = requiredMask & ~drawnMask
    lastRequiredMask = requiredMask
    lastDrawnMask = drawnMask
    lastMissingMask = missingMask
    guard missingMask == 0 else {
      incompletePublishSuppressedCount += 1
      lastIncompleteReason =
        "source package incomplete requiredMask=\(requiredMask) drawnMask=\(drawnMask) missingMask=\(missingMask)"
      return
    }
    lastIncompleteReason = ""

    var published: [MacOSNativeCompositorSourceTexture] = []
    published.reserveCapacity(rings.count)
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
      published.append(MacOSNativeCompositorSourceTexture(
        pixelBuffer: ring.buffers[ring.publishedIndex],
        sourceSlot: ring.slot,
        fileId: ring.fileId,
        width: ring.width,
        height: ring.height
      ))
    }
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
    compositor?.setSourceBuffers(
      textures: published,
      overlay: player.currentOverlayPrimitives(),
      projection: projection,
      completionTarget: pendingInitialSignature.isEmpty ? nil : self
    )
    if pendingInitialSignature.isEmpty {
      hasPublished = true
    }
  }

  private func requiredMask(for count: Int) -> UInt64 {
    guard count > 0 else { return 0 }
    let clamped = min(count, 63)
    return (UInt64(1) << UInt64(clamped)) - 1
  }

  private func completeInitialPublishOnQueue(success: Bool) {
    let signature = pendingInitialSignature
    guard !signature.isEmpty else { return }
    pendingInitialProjection = nil
    pendingInitialSignature = ""
    DispatchQueue.main.async { [weak self] in
      self?.delegate?.sourceRingInitialPublishCompleted(
        signature: signature,
        success: success
      )
    }
  }
}
