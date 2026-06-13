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
  private var bakeTarget: MacOSNativeMetalPresentationTarget?
  private var live = false
  private var baking = false
  private var pending = false
  private var hasPublished = false
  private var depth = MacOSNativeCompositorSourceRing.ringDepth

  init(compositor: MacOSNativeCompositorView) {
    self.compositor = compositor
  }

  /// Sets up the ring for an interaction. Allocates per-track buffers, bakes the
  /// initial frame, and publishes it. Returns false (and reports the error to the
  /// compositor) when allocation/bake fails so the bridge can clear cleanly.
  func subscribe(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    edrOutputEnabled: Bool
  ) {
    ringQueue.async { [weak self] in
      self?.subscribeOnQueue(
        player: player,
        descriptors: descriptors,
        order: order,
        edrOutputEnabled: edrOutputEnabled
      )
    }
  }

  /// Requests a re-bake of the current frame into the next ring buffers. Coalesced
  /// on the ring queue: a burst of frame callbacks collapses into baking the
  /// latest available frame. No-op when not subscribed.
  func requestRefresh(player: MacOSNativePlayerSession) {
    ringQueue.async { [weak self] in
      guard let self, self.live, !self.rings.isEmpty else { return }
      // Frozen-snapshot fallback (depth == 1) never refreshes: the source is too
      // large for a live ring, so it intentionally behaves like the old paused
      // one-shot.
      if self.depth <= 1 { return }
      if self.baking {
        self.pending = true
        return
      }
      self.baking = true
      repeat {
        self.pending = false
        self.bakeAndPublishOnQueue(player: player)
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
      self.compositor?.clearSource(reason: reason)
    }
  }

  // MARK: - Ring queue internals

  private func subscribeOnQueue(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    edrOutputEnabled: Bool
  ) {
    live = false
    rings = []
    self.order = order

    guard !descriptors.isEmpty else {
      compositor?.setSourceBuffers(textures: [], error: "no source tracks")
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
      compositor?.setSourceBuffers(textures: [], error: "no source cache targets")
      return
    }
    var chosenDepth = Self.ringDepth
    if perFrameBytes * chosenDepth > Self.ringBudgetBytes {
      chosenDepth = 1
    }
    if perFrameBytes * chosenDepth > Self.ringBudgetBytes {
      compositor?.setSourceBuffers(
        textures: [],
        error: "source cache memory cap exceeded"
      )
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
          error: "failed to allocate source cache pixel buffer"
        )
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
      compositor?.setSourceBuffers(textures: [], error: "no source cache targets")
      return
    }

    rings = built
    bakeTarget = MacOSNativeMetalPresentationTarget(width: maxWidth, height: maxHeight)
    live = true
    baking = false
    pending = false
    hasPublished = false

    // Initial bake into buffer 0 of each ring; publishes on success.
    bakeAndPublishOnQueue(player: player)
  }

  /// Bakes the current frame into each track's next write buffer and, if any
  /// track drew, publishes the just-baked buffers to the compositor. Runs on the
  /// ring queue. The bake is synchronous (waits for GPU completion), so the buffer
  /// is fully written before it is published.
  private func bakeAndPublishOnQueue(player: MacOSNativePlayerSession) {
    guard live, let bakeTarget, !rings.isEmpty else { return }

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
    guard result.drawnCount > 0 else {
      // A transient bake miss (e.g. between seeks) should not blank the viewport
      // once we have a good frame to hold. Only surface the error before the very
      // first successful publish.
      if !hasPublished {
        compositor?.setSourceBuffers(textures: [], error: result.error)
      }
      return
    }

    var published: [MacOSNativeCompositorSourceTexture] = []
    for i in rings.indices {
      // A target is at the same array order as `rings`; mark drawn ones published.
      if targets[i].drawn != 0 {
        rings[i].publishedIndex = rings[i].writeIndex
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
    hasPublished = true
    compositor?.setSourceBuffers(textures: published)
  }
}
