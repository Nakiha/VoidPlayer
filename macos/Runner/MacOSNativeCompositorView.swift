import Cocoa
import CoreVideo
import FlutterMacOS
import IOSurface
import Metal
import QuartzCore

struct MacOSNativeCompositorSourceTexture {
  let pixelBuffer: CVPixelBuffer
  let sourceSlot: Int
  let fileId: Int
  let width: Int
  let height: Int
}

final class MacOSNativeCompositorView: NSView {
  private let device: MTLDevice
  private let commandQueue: MTLCommandQueue
  private let videoPipeline: MTLRenderPipelineState
  private let flutterPipeline: MTLRenderPipelineState
  private let overlayPipeline: MTLRenderPipelineState
  private let configuration: MacOSPresentationConfiguration
  private let outputPixelFormat: MTLPixelFormat
  private let outputMode: String
  private let textureCache: CVMetalTextureCache
  private weak var engine: FlutterEngine?
  private weak var videoTexture: MacOSVideoTexture?
  private let metalLayer = CAMetalLayer()
  private let compositorQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.native-compositor",
    qos: .userInteractive
  )
  private let compositorQueueKey = DispatchSpecificKey<Bool>()
  private var displayLink: MacOSViewportDisplayLink?
  private var frameCount = 0
  private var lastVideoTextureAvailable = false
  private var lastFlutterTextureAvailable = false
  private var lastCompositeSucceeded = false
  private var lastFailure = "not drawn"
  private var lastFlutterAlphaAverageX1000 = -1
  private var lastFlutterTransparentRatioX1000 = -1
  private var lastVideoPixelFormat = "unknown"
  private var lastEDRVideoSampleCount = 0
  private var lastEDRVideoMaxRGBX1000 = 0
  private var lastEDRVideoPixelsOver1X1000 = 0
  private var lastVideoSRGBToLinearEnabled = false
  private var lastFlutterSRGBToLinearEnabled = false
  private var lastVideoMetricsSampleNs: UInt64 = 0
  private var lastFlutterAlphaMetricsSampleNs: UInt64 = 0
  private var skippedInFlightFrames = 0
  private var skippedStaticFrames = 0
  private var compositorDirty = true
  private var displayLinkWarmUntilNs: UInt64 = 0
  private var lastPresentedVideoSourceKey: UInt64 = 0
  private var lastPresentedFlutterSourceKey: UInt64 = 0
  private var explicitHoleRect: SIMD4<Float>?
  private var lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
  private var displayedLayoutRevision: UInt64 = 0
  // Live source buffers come from the source ring; the projection that maps the
  // viewport onto those source textures comes from Dart (the full current
  // layout, recomputed per layout change). One projection path: no residual
  // transform, no revision anchoring, no subscribe/unsubscribe handoff.
  private var sourceCacheTextures: [MacOSNativeCompositorSourceTexture] = []
  private var overlayPrimitives = MacOSNativeOverlayPrimitives.empty
  private var lastOverlayDiagnosticSignature = ""
  private var sourceCacheGeneration: UInt64 = 0
  private var sourceCacheLastError = ""
  private var sourceProjectionSet = false
  private var sourceLayoutFlags = SIMD4<Float>(0, 0, 0.5, 1)
  private var sourceOrder = SIMD4<Float>(0, 1, 2, 3)
  private var sourceProjDisplayOffsetX = SIMD4<Float>(0, 0, 0, 0)
  private var sourceProjDisplayOffsetY = SIMD4<Float>(0, 0, 0, 0)
  private var sourceProjInvDisplaySizeX = SIMD4<Float>(0, 0, 0, 0)
  private var sourceProjInvDisplaySizeY = SIMD4<Float>(0, 0, 0, 0)
  private var sourceProjViewOffsetUvX = SIMD4<Float>(0, 0, 0, 0)
  private var sourceProjViewOffsetUvY = SIMD4<Float>(0, 0, 0, 0)
  private var viewportBackgroundColor = SIMD4<Float>(0, 0, 0, 1)
  private let compositeRate = MacOSRateWindow()
  private let sourceCachePublishRate = MacOSRateWindow()
  private let sourceProjectionRate = MacOSRateWindow()
  private let inFlightSemaphore = DispatchSemaphore(value: 2)

  private static let metricsSampleIntervalNs: UInt64 = 1_000_000_000
  private static let displayLinkWarmGraceNs: UInt64 = 250_000_000

  static var isEnabled: Bool {
    MacOSPresentationConfiguration.current.nativeCompositorEnabled
  }

  init?(engine: FlutterEngine) {
    let configuration = MacOSPresentationConfiguration.current
    let outputPixelFormat = configuration.compositorPixelFormat
    guard let device = MTLCreateSystemDefaultDevice(),
          let commandQueue = device.makeCommandQueue(),
          let pipelines = Self.makePipelines(
            device: device,
            outputPixelFormat: outputPixelFormat
          ) else {
      return nil
    }

    var cache: CVMetalTextureCache?
    guard CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache) == kCVReturnSuccess,
          let cache else {
      return nil
    }

    self.device = device
    self.commandQueue = commandQueue
    self.videoPipeline = pipelines.video
    self.flutterPipeline = pipelines.flutter
    self.overlayPipeline = pipelines.overlay
    self.configuration = configuration
    self.outputPixelFormat = outputPixelFormat
    self.outputMode = configuration.compositorOutputMode
    self.textureCache = cache
    self.engine = engine
    super.init(frame: .zero)
    compositorQueue.setSpecific(key: compositorQueueKey, value: true)

    wantsLayer = true
    layer = metalLayer
    metalLayer.device = device
    metalLayer.pixelFormat = outputPixelFormat
    metalLayer.framebufferOnly = true
    metalLayer.isOpaque = true
    if outputPixelFormat == .rgba16Float {
      metalLayer.wantsExtendedDynamicRangeContent = true
      metalLayer.colorspace = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3)
    }
    metalLayer.contentsScale = NSScreen.main?.backingScaleFactor ?? 2.0
    autoresizingMask = [.width, .height]
  }

  required init?(coder: NSCoder) {
    return nil
  }

  override func hitTest(_ point: NSPoint) -> NSView? {
    return nil
  }

  override func layout() {
    super.layout()
    let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2.0
    metalLayer.contentsScale = scale
    metalLayer.drawableSize = CGSize(
      width: max(1.0, bounds.width * scale),
      height: max(1.0, bounds.height * scale)
    )
    compositorQueue.async { [weak self] in
      self?.markCompositorDirty()
    }
  }

  func attach(to parent: NSView) {
    frame = parent.bounds
    parent.addSubview(self, positioned: .above, relativeTo: nil)
    displayLink = MacOSViewportDisplayLink(deliveryQueue: compositorQueue) { [weak self] in
      self?.drawComposite()
    }
    displayLink?.start()
    NSLog("VoidPlayer native compositor: installed")
  }

  func detach() {
    displayLink?.stop()
    displayLink = nil
    removeFromSuperview()
  }

  func setVideoTexture(_ texture: MacOSVideoTexture?) {
    compositorQueue.async { [weak self] in
      self?.videoTexture = texture
      self?.markCompositorDirty()
    }
  }

  func setViewportRect(
    left: Int,
    top: Int,
    width: Int,
    height: Int,
    surfaceWidth: Int,
    surfaceHeight: Int
  ) {
    guard width > 0, height > 0, surfaceWidth > 0, surfaceHeight > 0 else {
      compositorQueue.async { [weak self] in
        self?.explicitHoleRect = nil
        self?.markCompositorDirty()
      }
      return
    }
    let minX = Float(max(0, left)) / Float(surfaceWidth)
    let minY = Float(max(0, top)) / Float(surfaceHeight)
    let maxX = Float(min(surfaceWidth, left + width)) / Float(surfaceWidth)
    let maxY = Float(min(surfaceHeight, top + height)) / Float(surfaceHeight)
    let rect = SIMD4<Float>(minX, minY, maxX, maxY)
    compositorQueue.async { [weak self] in
      guard let self else { return }
      let changed = explicitHoleRect != rect
      explicitHoleRect = rect
      lastHoleRect = rect
      if changed {
        markCompositorDirty()
      }
    }
  }

  func setViewportBackgroundColor(_ color: UInt32) {
    let a = Float((color >> 24) & 0xFF) / 255.0
    let r = Float((color >> 16) & 0xFF) / 255.0
    let g = Float((color >> 8) & 0xFF) / 255.0
    let b = Float(color & 0xFF) / 255.0
    let next = SIMD4<Float>(r, g, b, a)
    compositorQueue.async { [weak self] in
      guard let self else { return }
      if viewportBackgroundColor != next {
        viewportBackgroundColor = next
        markCompositorDirty()
      }
    }
  }

  /// Publishes the latest live source buffers (from the source ring). Projection
  /// is set separately via `setSourceProjection`; buffers and projection update
  /// independently (buffers per frame, projection per layout change).
  func setSourceBuffers(
    textures: [MacOSNativeCompositorSourceTexture],
    overlay: MacOSNativeOverlayPrimitives? = nil,
    error: String = ""
  ) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      sourceCacheTextures = textures
      if let overlay {
        overlayPrimitives = overlay
      }
      sourceCacheLastError = error
      sourceCacheGeneration &+= 1
      sourceCachePublishRate.record()
      logOverlayPrimitivesIfChanged(reason: "source-buffers")
      markCompositorDirty()
    }
  }

  func setOverlayPrimitives(_ overlay: MacOSNativeOverlayPrimitives) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      overlayPrimitives = overlay
      logOverlayPrimitivesIfChanged(reason: "overlay-refresh")
      markCompositorDirty()
    }
  }

  func clearSource(reason: String) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      if sourceCacheTextures.isEmpty && !sourceProjectionSet {
        return
      }
      sourceCacheTextures = []
      overlayPrimitives = .empty
      sourceProjectionSet = false
      sourceCacheLastError = reason
      sourceCacheGeneration &+= 1
      logOverlayPrimitivesIfChanged(reason: "source-clear")
      markCompositorDirty()
      MacOSProfilerLog.traceEvent(String(
        format: "VoidPlayer viewport trace swift event=source-clear generation=%llu reason=%@",
        sourceCacheGeneration,
        reason
      ))
    }
  }

  /// Updates the full-layout projection the compositor applies to the source
  /// textures. Called by Dart on every layout change (pan/zoom/split/mode/resize)
  /// with the current layout's per-slot projection params. This is the single
  /// projection path — the source view always reflects the current layout.
  func setSourceProjection(
    mode: Int,
    splitPos: Double,
    activeTrackCount: Int,
    order: [Int],
    displayOffsetX: [Double],
    displayOffsetY: [Double],
    invDisplaySizeX: [Double],
    invDisplaySizeY: [Double],
    viewOffsetUvX: [Double],
    viewOffsetUvY: [Double]
  ) {
    let safeSplit = splitPos.isFinite ? Float(min(1.0, max(0.0, splitPos))) : 0.5
    let flags = SIMD4<Float>(0, Float(mode), safeSplit, Float(max(1, activeTrackCount)))
    let orderVec = SIMD4<Float>(
      Float(order.indices.contains(0) ? order[0] : 0),
      Float(order.indices.contains(1) ? order[1] : 1),
      Float(order.indices.contains(2) ? order[2] : 2),
      Float(order.indices.contains(3) ? order[3] : 3)
    )
    func vec(_ values: [Double]) -> SIMD4<Float> {
      SIMD4<Float>(
        Float(values.indices.contains(0) ? values[0] : 0),
        Float(values.indices.contains(1) ? values[1] : 0),
        Float(values.indices.contains(2) ? values[2] : 0),
        Float(values.indices.contains(3) ? values[3] : 0)
      )
    }
    let dox = vec(displayOffsetX)
    let doy = vec(displayOffsetY)
    let idsx = vec(invDisplaySizeX)
    let idsy = vec(invDisplaySizeY)
    let voux = vec(viewOffsetUvX)
    let vouy = vec(viewOffsetUvY)
    compositorQueue.async { [weak self] in
      guard let self else { return }
      sourceLayoutFlags = flags
      sourceOrder = orderVec
      sourceProjDisplayOffsetX = dox
      sourceProjDisplayOffsetY = doy
      sourceProjInvDisplaySizeX = idsx
      sourceProjInvDisplaySizeY = idsy
      sourceProjViewOffsetUvX = voux
      sourceProjViewOffsetUvY = vouy
      sourceProjectionSet = true
      sourceProjectionRate.record()
      markCompositorDirty()
    }
  }

  func diagnostics() -> [String: Any] {
    if DispatchQueue.getSpecific(key: compositorQueueKey) == true {
      return diagnosticsOnCompositorQueue()
    }
    return compositorQueue.sync {
      diagnosticsOnCompositorQueue()
    }
  }

  private func diagnosticsOnCompositorQueue() -> [String: Any] {
    var result = configuration.diagnostics
    result.merge([
      "nativeCompositorEnabled": true,
      "nativeCompositorSpikeEnabled": true,
      "nativeCompositorFrames": frameCount,
      "nativeCompositorCompositeHz": compositeRate.rateHz(),
      "nativeCompositorCompositeHzX1000": Int(compositeRate.rateHz() * 1000.0),
      "nativeCompositorVideoTextureAvailable": lastVideoTextureAvailable,
      "nativeCompositorFlutterTextureAvailable": lastFlutterTextureAvailable,
      "nativeCompositorLastCompositeSucceeded": lastCompositeSucceeded,
      "nativeCompositorLastFailure": lastFailure,
      "nativeCompositorFlutterAlphaAverageX1000": lastFlutterAlphaAverageX1000,
      "nativeCompositorFlutterTransparentRatioX1000": lastFlutterTransparentRatioX1000,
      "nativeCompositorHoleLeftX1000": Int(lastHoleRect.x * 1000.0),
      "nativeCompositorHoleTopX1000": Int(lastHoleRect.y * 1000.0),
      "nativeCompositorHoleRightX1000": Int(lastHoleRect.z * 1000.0),
      "nativeCompositorHoleBottomX1000": Int(lastHoleRect.w * 1000.0),
      "nativeCompositorDrawableWidth": Int(metalLayer.drawableSize.width),
      "nativeCompositorDrawableHeight": Int(metalLayer.drawableSize.height),
      "nativeCompositorOutputMode": outputMode,
      "nativeCompositorOutputPixelFormat": String(describing: outputPixelFormat),
      "nativeCompositorEDREnabled": outputPixelFormat == .rgba16Float,
      "nativeCompositorEDRWantsExtendedDynamicRangeContent":
        metalLayer.wantsExtendedDynamicRangeContent,
      "nativeCompositorVideoPixelFormat": lastVideoPixelFormat,
      "nativeCompositorEDRVideoSampleCount": lastEDRVideoSampleCount,
      "nativeCompositorEDRVideoMaxRGBX1000": lastEDRVideoMaxRGBX1000,
      "nativeCompositorEDRVideoPixelsOver1X1000": lastEDRVideoPixelsOver1X1000,
      "nativeCompositorVideoSRGBToLinearEnabled": lastVideoSRGBToLinearEnabled,
      "nativeCompositorFlutterSRGBToLinearEnabled": lastFlutterSRGBToLinearEnabled,
      "nativeCompositorSkippedInFlightFrames": skippedInFlightFrames,
      "nativeCompositorSkippedStaticFrames": skippedStaticFrames,
      "nativeCompositorViewportTransformEnabled": false,
      "nativeCompositorViewportTransformRequestedEnabled": false,
      "nativeCompositorViewportTransformGeneration": Int(
        min(sourceCacheGeneration, UInt64(Int.max))
      ),
      "nativeCompositorDisplayedLayoutRevision": Int(
        min(displayedLayoutRevision, UInt64(Int.max))
      ),
      "nativeCompositorViewportTransformBaseDisplayedLayoutRevision": Int(
        min(displayedLayoutRevision, UInt64(Int.max))
      ),
      "nativeCompositorViewportTransformScaleXX1000": 1000,
      "nativeCompositorViewportTransformScaleYX1000": 1000,
      "nativeCompositorViewportTransformTranslateXX1000": 0,
      "nativeCompositorViewportTransformTranslateYX1000": 0,
      "nativeCompositorSourceProjectionEnabled": sourceProjectionSet,
      "nativeCompositorSourceCacheActive":
        sourceProjectionSet && !sourceCacheTextures.isEmpty,
      "nativeCompositorSourceCacheTextureCount": sourceCacheTextures.count,
      "nativeCompositorSourceCacheGeneration": Int(
        min(sourceCacheGeneration, UInt64(Int.max))
      ),
      "nativeCompositorSourceCacheBytes": sourceCacheTextures.reduce(0) { total, entry in
        total + CVPixelBufferGetBytesPerRow(entry.pixelBuffer) *
          CVPixelBufferGetHeight(entry.pixelBuffer)
      },
      "nativeCompositorSourceCacheLastError": sourceCacheLastError,
      "nativeCompositorOverlayGeneration": Int(
        min(overlayPrimitives.generation, UInt64(Int.max))
      ),
      "nativeCompositorOverlayFillRectCount": overlayPrimitives.fillRects.count,
      "nativeCompositorOverlayLineRectCount": overlayPrimitives.lineRects.count,
      "nativeCompositorOverlayMotionLineCount": overlayPrimitives.motionLines.count,
      "nativeCompositorSourceBakedOverlayDisabled":
        overlayPrimitives.sourceBakedOverlayDisabled,
    ]) { _, next in next }
    result["nativeCompositorOverlayTrackCount"] = Int(
      min(overlayPrimitives.overlayTrackCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayMatchedTrackCount"] = Int(
      min(overlayPrimitives.matchedTrackCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayMissingTrackSlotCount"] = Int(
      min(overlayPrimitives.missingTrackSlotCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayMissingPresentedFrameCount"] = Int(
      min(overlayPrimitives.missingPresentedFrameCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayMissingFrameIndexCount"] = Int(
      min(overlayPrimitives.missingFrameIndexCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayInvalidVideoSizeCount"] = Int(
      min(overlayPrimitives.invalidVideoSizeCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayFrameMissingCount"] = Int(
      min(overlayPrimitives.overlayFrameMissingCount, UInt64(Int.max))
    )
    result["nativeCompositorOverlayHeatmapMissingFeatureTrackCount"] = Int(
      min(overlayPrimitives.heatmapMissingFeatureTrackCount, UInt64(Int.max))
    )
    result["nativeCompositorSourceCacheHz"] = sourceCachePublishRate.rateHz()
    result["nativeCompositorSourceCacheHzX1000"] = Int(
      sourceCachePublishRate.rateHz() * 1000.0
    )
    result["nativeCompositorSourceProjectionHz"] = sourceProjectionRate.rateHz()
    result["nativeCompositorSourceProjectionHzX1000"] = Int(
      sourceProjectionRate.rateHz() * 1000.0
    )
    return result
  }

  private func logOverlayPrimitivesIfChanged(reason: String) {
    let signature = "\(overlayPrimitives.generation):\(overlayPrimitives.fillRects.count):\(overlayPrimitives.lineRects.count):\(overlayPrimitives.motionLines.count):\(overlayPrimitives.sourceBakedOverlayDisabled):\(overlayPrimitives.overlayTrackCount):\(overlayPrimitives.matchedTrackCount):\(overlayPrimitives.missingTrackSlotCount):\(overlayPrimitives.missingPresentedFrameCount):\(overlayPrimitives.missingFrameIndexCount):\(overlayPrimitives.overlayFrameMissingCount)"
    guard signature != lastOverlayDiagnosticSignature else { return }
    lastOverlayDiagnosticSignature = signature
    String(
      format:
        "NativeCompositorOverlay reason=%@ generation=%llu fill=%d line=%d motion=%d sourceBakedDisabled=%@ tracks=%llu matched=%llu missingSlot=%llu missingPresented=%llu missingFrameIndex=%llu missingOverlayFrame=%llu missingFeature=%llu",
      reason,
      overlayPrimitives.generation,
      overlayPrimitives.fillRects.count,
      overlayPrimitives.lineRects.count,
      overlayPrimitives.motionLines.count,
      overlayPrimitives.sourceBakedOverlayDisabled ? "true" : "false",
      overlayPrimitives.overlayTrackCount,
      overlayPrimitives.matchedTrackCount,
      overlayPrimitives.missingTrackSlotCount,
      overlayPrimitives.missingPresentedFrameCount,
      overlayPrimitives.missingFrameIndexCount,
      overlayPrimitives.overlayFrameMissingCount,
      overlayPrimitives.heatmapMissingFeatureTrackCount
    ).withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
  }

  private func drawComposite() {
    autoreleasepool {
      guard let videoSnapshot = currentVideoMetalTexture() else {
        setHiddenOnMain(true)
        recordFailure("no video texture")
        return
      }
      guard let flutterSnapshot = currentFlutterMetalTexture() else {
        recordFailure("no Flutter texture")
        return
      }
      let nowNs = DispatchTime.now().uptimeNanoseconds
      let sourceChanged =
        videoSnapshot.sourceKey != lastPresentedVideoSourceKey ||
        flutterSnapshot.sourceKey != lastPresentedFlutterSourceKey
      if !compositorDirty && !sourceChanged && nowNs >= displayLinkWarmUntilNs {
        skippedStaticFrames += 1
        return
      }
      let video = videoSnapshot.texture
      let flutter = flutterSnapshot.texture
      let sourceCache = currentSourceCacheMetalTextures()
      displayedLayoutRevision = max(displayedLayoutRevision, videoSnapshot.layoutRevision)
      let sourceCacheActive = sourceProjectionSet && !sourceCache.textures.isEmpty
      var holeRect = explicitHoleRect ?? lastHoleRect
      var layoutFlags = sourceLayoutFlags
      let sourceProjectionEnabled = sourceProjectionSet
      var colorFlags = SIMD4<Float>(
        outputPixelFormat == .rgba16Float ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: video) ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: flutter) ? 1.0 : 0.0,
        sourceCacheActive ? 1.0 : 0.0
      )
      var compositorFlags = SIMD4<Float>(
        sourceProjectionEnabled ? 1.0 : 0.0,
        0.0,
        0.0,
        0.0
      )
      var sourcePresentFlags = sourceCache.presentFlags
      var sourceOrder = sourceCache.order
      var sourceDisplayOffsetX = sourceCache.displayOffsetX
      var sourceDisplayOffsetY = sourceCache.displayOffsetY
      var sourceInvDisplaySizeX = sourceCache.invDisplaySizeX
      var sourceInvDisplaySizeY = sourceCache.invDisplaySizeY
      var sourceViewOffsetUvX = sourceCache.viewOffsetUvX
      var sourceViewOffsetUvY = sourceCache.viewOffsetUvY
      var backgroundColor = viewportBackgroundColor
      guard inFlightSemaphore.wait(timeout: .now()) == .success else {
        skippedInFlightFrames += 1
        return
      }
      guard let drawable = metalLayer.nextDrawable() else {
        inFlightSemaphore.signal()
        recordFailure("no drawable")
        return
      }
      let overlayVertices = sourceCacheActive
        ? buildOverlayVertices(
            primitives: overlayPrimitives,
            holeRect: holeRect,
            drawableSize: metalLayer.drawableSize,
            outputEDR: outputPixelFormat == .rgba16Float
          )
        : []
      let overlayBuffer = overlayVertices.isEmpty
        ? nil
        : overlayVertices.withUnsafeBytes { bytes in
            device.makeBuffer(bytes: bytes.baseAddress!, length: bytes.count, options: [])
          }
      guard let commandBuffer = commandQueue.makeCommandBuffer(),
            let encoder = commandBuffer.makeRenderCommandEncoder(
              descriptor: renderPassDescriptor(drawable: drawable, backgroundColor: backgroundColor)
            ) else {
        inFlightSemaphore.signal()
        recordFailure("failed to create command encoder")
        return
      }
      let inFlightSemaphore = self.inFlightSemaphore

      encoder.setRenderPipelineState(videoPipeline)
      encoder.setFragmentTexture(video, index: 0)
      encoder.setFragmentTexture(flutter, index: 1)
      encoder.setFragmentTexture(sourceCache.textures[0] ?? video, index: 2)
      encoder.setFragmentTexture(sourceCache.textures[1] ?? video, index: 3)
      encoder.setFragmentTexture(sourceCache.textures[2] ?? video, index: 4)
      encoder.setFragmentTexture(sourceCache.textures[3] ?? video, index: 5)
      encoder.setFragmentBytes(
        &holeRect,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 0
      )
      encoder.setFragmentBytes(
        &colorFlags,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 1
      )
      encoder.setFragmentBytes(
        &layoutFlags,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 2
      )
      encoder.setFragmentBytes(
        &sourcePresentFlags,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 3
      )
      encoder.setFragmentBytes(
        &sourceOrder,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 4
      )
      encoder.setFragmentBytes(
        &sourceDisplayOffsetX,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 5
      )
      encoder.setFragmentBytes(
        &sourceDisplayOffsetY,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 6
      )
      encoder.setFragmentBytes(
        &sourceInvDisplaySizeX,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 7
      )
      encoder.setFragmentBytes(
        &sourceInvDisplaySizeY,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 8
      )
      encoder.setFragmentBytes(
        &sourceViewOffsetUvX,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 9
      )
      encoder.setFragmentBytes(
        &sourceViewOffsetUvY,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 10
      )
      encoder.setFragmentBytes(
        &backgroundColor,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 11
      )
      encoder.setFragmentBytes(
        &compositorFlags,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 12
      )
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)

      if let overlayBuffer, !overlayVertices.isEmpty {
        encoder.setRenderPipelineState(overlayPipeline)
        encoder.setVertexBuffer(overlayBuffer, offset: 0, index: 0)
        encoder.drawPrimitives(
          type: .triangle,
          vertexStart: 0,
          vertexCount: overlayVertices.count
        )
      }

      encoder.setRenderPipelineState(flutterPipeline)
      encoder.setFragmentTexture(flutter, index: 0)
      encoder.setFragmentBytes(
        &colorFlags,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 0
      )
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
      encoder.endEncoding()
      commandBuffer.addCompletedHandler { _ in
        inFlightSemaphore.signal()
      }
      commandBuffer.present(drawable)
      commandBuffer.commit()

      setHiddenOnMain(false)
      frameCount += 1
      compositeRate.record()
      lastVideoTextureAvailable = true
      lastFlutterTextureAvailable = true
      lastCompositeSucceeded = true
      lastFailure = ""
      lastVideoSRGBToLinearEnabled = colorFlags.y > 0.5
      lastFlutterSRGBToLinearEnabled = colorFlags.z > 0.5
      lastPresentedVideoSourceKey = videoSnapshot.sourceKey
      lastPresentedFlutterSourceKey = flutterSnapshot.sourceKey
      compositorDirty = false
      if frameCount == 1 || frameCount % 120 == 0 {
        let compositeSummary = String(
          format:
            "NativeCompositorComposite frame=%d mode=%@ video=%dx%d flutter=%dx%d drawable=%dx%d layoutRevision=%llu sourceProjection=%d sourceProjectionEnabled=%d hole=%.4f,%.4f,%.4f,%.4f source0Offset=%.4f,%.4f source0InvSize=%.4f,%.4f bg=%.3f,%.3f,%.3f,%.3f",
          frameCount,
          outputMode,
          video.width,
          video.height,
          flutter.width,
          flutter.height,
          Int(metalLayer.drawableSize.width),
          Int(metalLayer.drawableSize.height),
          displayedLayoutRevision,
          sourceCacheActive ? 1 : 0,
          sourceProjectionEnabled ? 1 : 0,
          holeRect.x,
          holeRect.y,
          holeRect.z,
          holeRect.w,
          sourceDisplayOffsetX.x,
          sourceDisplayOffsetY.x,
          sourceInvDisplaySizeX.x,
          sourceInvDisplaySizeY.x,
          backgroundColor.x,
          backgroundColor.y,
          backgroundColor.z,
          backgroundColor.w
        )
        compositeSummary.withCString { pointer in
          VPMacOSLogProfilerSummary(pointer)
        }
        NSLog(
          "VoidPlayer native compositor: composite frame=%d mode=%@ video=%dx%d flutter=%dx%d drawable=%dx%d layoutRevision=%llu sourceProjection=%d",
          frameCount,
          outputMode,
          video.width,
          video.height,
          flutter.width,
          flutter.height,
          Int(metalLayer.drawableSize.width),
          Int(metalLayer.drawableSize.height),
          displayedLayoutRevision,
          sourceCacheActive ? 1 : 0
        )
      }
    }
  }

  private func markCompositorDirty() {
    compositorDirty = true
    displayLinkWarmUntilNs = DispatchTime.now().uptimeNanoseconds + Self.displayLinkWarmGraceNs
  }

  private func setHiddenOnMain(_ hidden: Bool) {
    DispatchQueue.main.async { [weak self] in
      self?.isHidden = hidden
    }
  }

  private func shouldConvertSRGBToLinear(texture: MTLTexture) -> Bool {
    guard outputPixelFormat == .rgba16Float else {
      return false
    }
    switch texture.pixelFormat {
    case .bgra8Unorm, .rgba8Unorm:
      return true
    default:
      return false
    }
  }

  private struct VideoTextureSnapshot {
    let texture: MTLTexture
    let sourceKey: UInt64
    let layoutRevision: UInt64
  }

  private struct FlutterTextureSnapshot {
    let texture: MTLTexture
    let sourceKey: UInt64
  }

  private struct SourceCacheTextureSnapshot {
    let textures: [Int: MTLTexture]
    let presentFlags: SIMD4<Float>
    let order: SIMD4<Float>
    let displayOffsetX: SIMD4<Float>
    let displayOffsetY: SIMD4<Float>
    let invDisplaySizeX: SIMD4<Float>
    let invDisplaySizeY: SIMD4<Float>
    let viewOffsetUvX: SIMD4<Float>
    let viewOffsetUvY: SIMD4<Float>
  }

  private struct OverlayVertex {
    var position: SIMD2<Float>
    var color: SIMD4<Float>
  }

  private func currentVideoMetalTexture() -> VideoTextureSnapshot? {
    guard let videoTexture,
          let presentationSnapshot = videoTexture.presentationSnapshot() else {
      lastVideoTextureAvailable = false
      return nil
    }
    let pixelBuffer = presentationSnapshot.pixelBuffer.takeRetainedValue()
    let generation = presentationSnapshot.generation
    let sourceKey = generation > 0
      ? UInt64(generation)
      : UInt64(UInt(bitPattern: Unmanaged.passUnretained(pixelBuffer).toOpaque()))
    let width = CVPixelBufferGetWidth(pixelBuffer)
    let height = CVPixelBufferGetHeight(pixelBuffer)
    maybeUpdateVideoEDRMetrics(pixelBuffer: pixelBuffer)
    let metalPixelFormat: MTLPixelFormat =
      CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_64RGBAHalf
      ? .rgba16Float
      : .bgra8Unorm
    var cvTexture: CVMetalTexture?
    let status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      textureCache,
      pixelBuffer,
      nil,
      metalPixelFormat,
      width,
      height,
      0,
      &cvTexture
    )
    guard status == kCVReturnSuccess, let cvTexture,
          let texture = CVMetalTextureGetTexture(cvTexture) else {
      lastVideoTextureAvailable = false
      return nil
    }
    return VideoTextureSnapshot(
      texture: texture,
      sourceKey: sourceKey,
      layoutRevision: presentationSnapshot.layoutRevision
    )
  }

  private func currentSourceCacheMetalTextures() -> SourceCacheTextureSnapshot {
    var textures: [Int: MTLTexture] = [:]
    var presentFlags = SIMD4<Float>(0, 0, 0, 0)

    for entry in sourceCacheTextures {
      let slot = entry.sourceSlot
      guard slot >= 0 && slot < 4 else { continue }
      let pixelBuffer = entry.pixelBuffer
      let width = CVPixelBufferGetWidth(pixelBuffer)
      let height = CVPixelBufferGetHeight(pixelBuffer)
      let metalPixelFormat: MTLPixelFormat =
        CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_64RGBAHalf
        ? .rgba16Float
        : .bgra8Unorm
      var cvTexture: CVMetalTexture?
      let status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        textureCache,
        pixelBuffer,
        nil,
        metalPixelFormat,
        width,
        height,
        0,
        &cvTexture
      )
      guard status == kCVReturnSuccess, let cvTexture,
            let texture = CVMetalTextureGetTexture(cvTexture) else {
        continue
      }
      textures[slot] = texture
      switch slot {
      case 0:
        presentFlags.x = 1
      case 1:
        presentFlags.y = 1
      case 2:
        presentFlags.z = 1
      default:
        presentFlags.w = 1
      }
    }
    return SourceCacheTextureSnapshot(
      textures: textures,
      presentFlags: presentFlags,
      order: sourceOrder,
      displayOffsetX: sourceProjDisplayOffsetX,
      displayOffsetY: sourceProjDisplayOffsetY,
      invDisplaySizeX: sourceProjInvDisplaySizeX,
      invDisplaySizeY: sourceProjInvDisplaySizeY,
      viewOffsetUvX: sourceProjViewOffsetUvX,
      viewOffsetUvY: sourceProjViewOffsetUvY
    )
  }

  private func buildOverlayVertices(
    primitives: MacOSNativeOverlayPrimitives,
    holeRect: SIMD4<Float>,
    drawableSize: CGSize,
    outputEDR: Bool
  ) -> [OverlayVertex] {
    guard !primitives.isEmpty,
          holeRect.z > holeRect.x,
          holeRect.w > holeRect.y,
          drawableSize.width > 0,
          drawableSize.height > 0 else {
      return []
    }
    var vertices: [OverlayVertex] = []
    vertices.reserveCapacity(
      primitives.fillRects.count * 6 +
      primitives.lineRects.count * 48 +
      primitives.motionLines.count * 6
    )

    let drawableWidth = Float(drawableSize.width)
    let drawableHeight = Float(drawableSize.height)
    let holeMinX = holeRect.x * drawableWidth
    let holeMinY = holeRect.y * drawableHeight
    let holeMaxX = holeRect.z * drawableWidth
    let holeMaxY = holeRect.w * drawableHeight
    let holeWidth = max(1, holeMaxX - holeMinX)
    let holeHeight = max(1, holeMaxY - holeMinY)

    func appendRect(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float, _ color: SIMD4<Float>) {
      let left = max(holeMinX, min(holeMaxX, min(x0, x1)))
      let right = max(holeMinX, min(holeMaxX, max(x0, x1)))
      let top = max(holeMinY, min(holeMaxY, min(y0, y1)))
      let bottom = max(holeMinY, min(holeMaxY, max(y0, y1)))
      guard right > left, bottom > top, color.w > 0.0001 else { return }
      func clip(_ x: Float, _ y: Float) -> SIMD2<Float> {
        SIMD2<Float>(
          x / drawableWidth * 2.0 - 1.0,
          1.0 - y / drawableHeight * 2.0
        )
      }
      let p0 = clip(left, top)
      let p1 = clip(right, top)
      let p2 = clip(left, bottom)
      let p3 = clip(right, bottom)
      vertices.append(OverlayVertex(position: p0, color: color))
      vertices.append(OverlayVertex(position: p2, color: color))
      vertices.append(OverlayVertex(position: p1, color: color))
      vertices.append(OverlayVertex(position: p1, color: color))
      vertices.append(OverlayVertex(position: p2, color: color))
      vertices.append(OverlayVertex(position: p3, color: color))
    }

    func valueAt(_ values: SIMD4<Float>, _ index: Int) -> Float {
      if index == 0 { return values.x }
      if index == 1 { return values.y }
      if index == 2 { return values.z }
      return values.w
    }

    func displaySlot(for sourceSlot: Int) -> Int? {
      let count = max(1, min(4, Int(sourceLayoutFlags.w.rounded())))
      for slot in 0..<count where Int(valueAt(sourceOrder, slot).rounded()) == sourceSlot {
        return slot
      }
      return nil
    }

    func unpackUV16(_ packed: UInt32) -> SIMD2<Float> {
      SIMD2<Float>(
        Float(packed & 0xffff) / 65535.0,
        Float((packed >> 16) & 0xffff) / 65535.0
      )
    }

    func projectedRect(_ rect: VPMacOSNativeOverlayGpuRect) -> SIMD4<Float>? {
      let sourceSlot = max(0, min(3, Int(rect.track_idx & 0xff)))
      guard let displaySlot = displaySlot(for: sourceSlot) else { return nil }
      let mode = Int(sourceLayoutFlags.y.rounded())
      let count = max(1, min(4, Int(sourceLayoutFlags.w.rounded())))
      let uv0 = unpackUV16(rect.rect_uv0)
      let uv1 = unpackUV16(rect.rect_uv1)
      let minUv = SIMD2<Float>(min(uv0.x, uv1.x), min(uv0.y, uv1.y))
      let maxUv = SIMD2<Float>(max(uv0.x, uv1.x), max(uv0.y, uv1.y))
      let displayOffset = SIMD2<Float>(
        valueAt(sourceProjDisplayOffsetX, sourceSlot),
        valueAt(sourceProjDisplayOffsetY, sourceSlot)
      )
      let invDisplaySize = SIMD2<Float>(
        valueAt(sourceProjInvDisplaySizeX, sourceSlot),
        valueAt(sourceProjInvDisplaySizeY, sourceSlot)
      )
      guard abs(invDisplaySize.x) > 0.00001, abs(invDisplaySize.y) > 0.00001 else {
        return nil
      }
      let viewOffset = SIMD2<Float>(
        valueAt(sourceProjViewOffsetUvX, sourceSlot),
        valueAt(sourceProjViewOffsetUvY, sourceSlot)
      )
      let displaySize = SIMD2<Float>(1.0 / invDisplaySize.x, 1.0 / invDisplaySize.y)
      var localMin = displayOffset + (minUv + viewOffset) * displaySize
      var localMax = displayOffset + (maxUv + viewOffset) * displaySize
      let sortedMin = SIMD2<Float>(min(localMin.x, localMax.x), min(localMin.y, localMax.y))
      let sortedMax = SIMD2<Float>(max(localMin.x, localMax.x), max(localMin.y, localMax.y))
      localMin = sortedMin
      localMax = sortedMax

      let globalMin: SIMD2<Float>
      let globalMax: SIMD2<Float>
      if mode == 0 && count > 1 {
        let slotOffset = Float(displaySlot)
        let divisor = Float(count)
        globalMin = SIMD2<Float>((slotOffset + localMin.x) / divisor, localMin.y)
        globalMax = SIMD2<Float>((slotOffset + localMax.x) / divisor, localMax.y)
      } else {
        globalMin = localMin
        globalMax = localMax
      }
      return SIMD4<Float>(
        holeMinX + globalMin.x * holeWidth,
        holeMinY + globalMin.y * holeHeight,
        holeMinX + globalMax.x * holeWidth,
        holeMinY + globalMax.y * holeHeight
      )
    }

    func linearChannel(_ value: Float) -> Float {
      let c = max(0, min(1, value))
      return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
    }

    func outputColor(r: Float, g: Float, b: Float, a: Float) -> SIMD4<Float> {
      let srgb = SIMD3<Float>(max(0, min(1, r)), max(0, min(1, g)), max(0, min(1, b)))
      guard outputEDR else {
        return SIMD4<Float>(srgb.x, srgb.y, srgb.z, max(0, min(1, a)))
      }
      let linear = SIMD3<Float>(
        linearChannel(srgb.x),
        linearChannel(srgb.y),
        linearChannel(srgb.z)
      )
      let p3 = SIMD3<Float>(
        0.8224619687 * linear.x + 0.1775380313 * linear.y,
        0.0331941989 * linear.x + 0.9668058011 * linear.y,
        0.0170826307 * linear.x + 0.0723974407 * linear.y + 0.9105199286 * linear.z
      )
      return SIMD4<Float>(p3.x, p3.y, p3.z, max(0, min(1, a)))
    }

    func colorFromBGRA(_ bgra: UInt32) -> SIMD4<Float> {
      outputColor(
        r: Float((bgra >> 16) & 0xff) / 255.0,
        g: Float((bgra >> 8) & 0xff) / 255.0,
        b: Float(bgra & 0xff) / 255.0,
        a: Float((bgra >> 24) & 0xff) / 255.0
      )
    }

    func lineStrength(_ rect: VPMacOSNativeOverlayGpuRect) -> Float {
      Float((rect.track_idx >> 8) & 0xff) / 255.0
    }

    func appendVerticalLine(x: Float, y0: Float, y1: Float, width: Float, color: SIMD4<Float>) {
      let center = floor(x + 0.001) + 0.5
      let half = max(0.5, width * 0.5)
      appendRect(center - half, y0, center + half, y1, color)
    }

    func appendHorizontalLine(y: Float, x0: Float, x1: Float, width: Float, color: SIMD4<Float>) {
      let center = floor(y + 0.001) + 0.5
      let half = max(0.5, width * 0.5)
      appendRect(x0, center - half, x1, center + half, color)
    }

    func appendLineSegment(_ rect: VPMacOSNativeOverlayGpuRect, width: Float) {
      guard let projected = projectedRect(rect) else { return }
      let color = colorFromBGRA(rect.color_bgra)
      let dx = projected.z - projected.x
      let dy = projected.w - projected.y
      let length = max(0.0001, sqrt(dx * dx + dy * dy))
      let nx = -dy / length * width * 0.5
      let ny = dx / length * width * 0.5
      let points = [
        SIMD2<Float>(projected.x + nx, projected.y + ny),
        SIMD2<Float>(projected.x - nx, projected.y - ny),
        SIMD2<Float>(projected.z + nx, projected.w + ny),
        SIMD2<Float>(projected.z - nx, projected.w - ny),
      ]
      func clip(_ p: SIMD2<Float>) -> SIMD2<Float> {
        SIMD2<Float>(
          p.x / drawableWidth * 2.0 - 1.0,
          1.0 - p.y / drawableHeight * 2.0
        )
      }
      let p0 = clip(points[0])
      let p1 = clip(points[1])
      let p2 = clip(points[2])
      let p3 = clip(points[3])
      vertices.append(OverlayVertex(position: p0, color: color))
      vertices.append(OverlayVertex(position: p1, color: color))
      vertices.append(OverlayVertex(position: p2, color: color))
      vertices.append(OverlayVertex(position: p2, color: color))
      vertices.append(OverlayVertex(position: p1, color: color))
      vertices.append(OverlayVertex(position: p3, color: color))
    }

    for rect in primitives.fillRects {
      guard let projected = projectedRect(rect) else { continue }
      appendRect(projected.x, projected.y, projected.z, projected.w, colorFromBGRA(rect.color_bgra))
    }
    for rect in primitives.lineRects {
      guard let projected = projectedRect(rect) else { continue }
      let strength = lineStrength(rect)
      guard strength > 0 else { continue }
      let halo = outputColor(r: 0, g: 0, b: 0, a: 0.85 * strength)
      let center = outputColor(r: 1, g: 1, b: 1, a: 0.95 * strength)
      appendVerticalLine(x: projected.x, y0: projected.y, y1: projected.w, width: 3, color: halo)
      appendVerticalLine(x: projected.z, y0: projected.y, y1: projected.w, width: 3, color: halo)
      appendHorizontalLine(y: projected.y, x0: projected.x, x1: projected.z, width: 3, color: halo)
      appendHorizontalLine(y: projected.w, x0: projected.x, x1: projected.z, width: 3, color: halo)
      appendVerticalLine(x: projected.x, y0: projected.y, y1: projected.w, width: 1, color: center)
      appendVerticalLine(x: projected.z, y0: projected.y, y1: projected.w, width: 1, color: center)
      appendHorizontalLine(y: projected.y, x0: projected.x, x1: projected.z, width: 1, color: center)
      appendHorizontalLine(y: projected.w, x0: projected.x, x1: projected.z, width: 1, color: center)
    }
    for line in primitives.motionLines {
      appendLineSegment(line, width: 2)
    }
    return vertices
  }

  private func maybeUpdateVideoEDRMetrics(pixelBuffer: CVPixelBuffer) {
    let format = CVPixelBufferGetPixelFormatType(pixelBuffer)
    lastVideoPixelFormat = Self.pixelFormatName(format)
    guard format == kCVPixelFormatType_64RGBAHalf else {
      lastEDRVideoSampleCount = 0
      lastEDRVideoMaxRGBX1000 = 0
      lastEDRVideoPixelsOver1X1000 = 0
      return
    }
    let nowNs = DispatchTime.now().uptimeNanoseconds
    if lastVideoMetricsSampleNs > 0 &&
        nowNs - lastVideoMetricsSampleNs < Self.metricsSampleIntervalNs {
      return
    }
    lastVideoMetricsSampleNs = nowNs
    guard CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly) == kCVReturnSuccess else {
      lastEDRVideoSampleCount = 0
      lastEDRVideoMaxRGBX1000 = 0
      lastEDRVideoPixelsOver1X1000 = 0
      return
    }
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
    guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else {
      lastEDRVideoSampleCount = 0
      lastEDRVideoMaxRGBX1000 = 0
      lastEDRVideoPixelsOver1X1000 = 0
      return
    }

    let width = CVPixelBufferGetWidth(pixelBuffer)
    let height = CVPixelBufferGetHeight(pixelBuffer)
    let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let words = baseAddress.assumingMemoryBound(to: UInt16.self)
    let sampleColumns = min(96, max(1, width))
    let sampleRows = min(96, max(1, height))
    var sampleCount = 0
    var overOneCount = 0
    var maxRGB: Float = 0.0

    for row in 0..<sampleRows {
      let y = sampleRows == 1 ? 0 : row * (height - 1) / (sampleRows - 1)
      let rowOffset = y * bytesPerRow / MemoryLayout<UInt16>.stride
      for column in 0..<sampleColumns {
        let x = sampleColumns == 1 ? 0 : column * (width - 1) / (sampleColumns - 1)
        let pixelOffset = rowOffset + x * 4
        let r = Self.floatFromHalf(words[pixelOffset])
        let g = Self.floatFromHalf(words[pixelOffset + 1])
        let b = Self.floatFromHalf(words[pixelOffset + 2])
        let pixelMax = max(r, max(g, b))
        maxRGB = max(maxRGB, pixelMax)
        if pixelMax > 1.0 {
          overOneCount += 1
        }
        sampleCount += 1
      }
    }

    lastEDRVideoSampleCount = sampleCount
    lastEDRVideoMaxRGBX1000 = Int((maxRGB * 1000.0).rounded())
    lastEDRVideoPixelsOver1X1000 = sampleCount > 0
      ? overOneCount * 1000 / sampleCount
      : 0
  }

  private func currentFlutterMetalTexture() -> FlutterTextureSnapshot? {
    guard let info = engine?.voidPlayerHDRCurrentFlutterSurfaceInfos().first,
          let texture = info["texture"] as? MTLTexture else {
      lastFlutterTextureAvailable = false
      return nil
    }
    if explicitHoleRect == nil {
      maybeUpdateFlutterAlphaMetrics(info: info)
    }
    return FlutterTextureSnapshot(
      texture: texture,
      sourceKey: flutterSurfaceSourceKey(info: info, texture: texture)
    )
  }

  private func flutterSurfaceSourceKey(info: [String: Any], texture: MTLTexture) -> UInt64 {
    if let ioSurfaceId = info["ioSurfaceId"] as? UInt64 {
      return ioSurfaceId
    }
    if let ioSurfaceId = info["ioSurfaceId"] as? Int {
      return UInt64(max(0, ioSurfaceId))
    }
    if let texturePointer = info["texturePointer"] as? UInt64 {
      return texturePointer
    }
    if let texturePointer = info["texturePointer"] as? Int {
      return UInt64(max(0, texturePointer))
    }
    return UInt64(UInt(bitPattern: Unmanaged.passUnretained(texture as AnyObject).toOpaque()))
  }

  private func maybeUpdateFlutterAlphaMetrics(info: [String: Any]) {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    if lastFlutterAlphaMetricsSampleNs > 0 &&
        nowNs - lastFlutterAlphaMetricsSampleNs < Self.metricsSampleIntervalNs {
      return
    }
    lastFlutterAlphaMetricsSampleNs = nowNs
    guard let rawSurface = info["ioSurface"] else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      return
    }
    let ioSurface = rawSurface as! IOSurfaceRef
    let pixelFormat = IOSurfaceGetPixelFormat(ioSurface)
    guard pixelFormat == kCVPixelFormatType_32BGRA else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      return
    }

    IOSurfaceLock(ioSurface, .readOnly, nil)
    defer { IOSurfaceUnlock(ioSurface, .readOnly, nil) }
    let baseAddress = IOSurfaceGetBaseAddress(ioSurface)

    let width = IOSurfaceGetWidth(ioSurface)
    let height = IOSurfaceGetHeight(ioSurface)
    let bytesPerRow = IOSurfaceGetBytesPerRow(ioSurface)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    let sampleColumns = min(96, max(1, width))
    let sampleRows = min(96, max(1, height))
    var alphaSum = 0
    var transparentCount = 0
    var sampleCount = 0
    var minTransparentX = width
    var minTransparentY = height
    var maxTransparentX = 0
    var maxTransparentY = 0

    for row in 0..<sampleRows {
      let y = sampleRows == 1 ? 0 : row * (height - 1) / (sampleRows - 1)
      for column in 0..<sampleColumns {
        let x = sampleColumns == 1 ? 0 : column * (width - 1) / (sampleColumns - 1)
        let alpha = Int(pixels[y * bytesPerRow + x * 4 + 3])
        alphaSum += alpha
        if alpha < 8 {
          transparentCount += 1
          minTransparentX = min(minTransparentX, x)
          minTransparentY = min(minTransparentY, y)
          maxTransparentX = max(maxTransparentX, x)
          maxTransparentY = max(maxTransparentY, y)
        }
        sampleCount += 1
      }
    }

    guard sampleCount > 0 else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      return
    }
    lastFlutterAlphaAverageX1000 = alphaSum * 1000 / (sampleCount * 255)
    lastFlutterTransparentRatioX1000 = transparentCount * 1000 / sampleCount
    if explicitHoleRect != nil {
      return
    }
    if transparentCount > 0 {
      let denomX = Float(max(1, width - 1))
      let denomY = Float(max(1, height - 1))
      lastHoleRect = SIMD4<Float>(
        Float(minTransparentX) / denomX,
        Float(minTransparentY) / denomY,
        Float(maxTransparentX) / denomX,
        Float(maxTransparentY) / denomY
      )
    } else {
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
    }
  }

  private func renderPassDescriptor(
    drawable: CAMetalDrawable,
    backgroundColor: SIMD4<Float>
  ) -> MTLRenderPassDescriptor {
    let descriptor = MTLRenderPassDescriptor()
    descriptor.colorAttachments[0].texture = drawable.texture
    descriptor.colorAttachments[0].loadAction = .clear
    descriptor.colorAttachments[0].storeAction = .store
    descriptor.colorAttachments[0].clearColor = MTLClearColor(
      red: Double(backgroundColor.x),
      green: Double(backgroundColor.y),
      blue: Double(backgroundColor.z),
      alpha: Double(backgroundColor.w)
    )
    return descriptor
  }

  private func recordFailure(_ message: String) {
    frameCount += 1
    lastCompositeSucceeded = false
    lastFailure = message
    if frameCount == 1 || frameCount % 120 == 0 {
      NSLog("VoidPlayer native compositor: \(message)")
    }
  }

  private static func pixelFormatName(_ format: OSType) -> String {
    switch format {
    case kCVPixelFormatType_32BGRA:
      return "32BGRA"
    case kCVPixelFormatType_64RGBAHalf:
      return "64RGBAHalf"
    default:
      return String(format)
    }
  }

  private static func floatFromHalf(_ value: UInt16) -> Float {
    let sign = (UInt32(value & 0x8000)) << 16
    let exponent = Int((value & 0x7C00) >> 10)
    let mantissa = UInt32(value & 0x03FF)
    let bits: UInt32
    if exponent == 0 {
      if mantissa == 0 {
        bits = sign
      } else {
        var normalizedMantissa = mantissa
        var normalizedExponent = -14
        while (normalizedMantissa & 0x0400) == 0 {
          normalizedMantissa <<= 1
          normalizedExponent -= 1
        }
        normalizedMantissa &= 0x03FF
        bits = sign |
          (UInt32(normalizedExponent + 127) << 23) |
          (normalizedMantissa << 13)
      }
    } else if exponent == 0x1F {
      bits = sign | 0x7F800000 | (mantissa << 13)
    } else {
      bits = sign |
        (UInt32(exponent - 15 + 127) << 23) |
        (mantissa << 13)
    }
    return Float(bitPattern: bits)
  }

  private static func makePipelines(
    device: MTLDevice,
    outputPixelFormat: MTLPixelFormat
  ) -> (
    video: MTLRenderPipelineState,
    flutter: MTLRenderPipelineState,
    overlay: MTLRenderPipelineState
  )? {
    let source = """
      #include <metal_stdlib>
      using namespace metal;

      struct VertexOut {
        float4 position [[position]];
        float2 uv;
      };

      vertex VertexOut vp_main(uint vertexID [[vertex_id]]) {
        float2 positions[3] = {
          float2(-1.0, -1.0),
          float2( 3.0, -1.0),
          float2(-1.0,  3.0),
        };
        float2 uvs[3] = {
          float2(0.0, 1.0),
          float2(2.0, 1.0),
          float2(0.0, -1.0),
        };
        VertexOut out;
        out.position = float4(positions[vertexID], 0.0, 1.0);
        out.uv = uvs[vertexID];
        return out;
      }

      struct OverlayVertexIn {
        float2 position;
        float4 color;
      };

      struct OverlayVertexOut {
        float4 position [[position]];
        float4 color;
      };

      vertex OverlayVertexOut overlay_vertex(
        device const OverlayVertexIn* vertices [[buffer(0)]],
        uint vertexID [[vertex_id]]
      ) {
        OverlayVertexOut out;
        out.position = float4(vertices[vertexID].position, 0.0, 1.0);
        out.color = vertices[vertexID].color;
        return out;
      }

      float srgbChannelToLinear(float value) {
        float c = clamp(value, 0.0, 1.0);
        return c <= 0.04045
          ? c / 12.92
          : pow((c + 0.055) / 1.055, 2.4);
      }

      float3 srgbToLinear(float3 color) {
        return float3(
          srgbChannelToLinear(color.r),
          srgbChannelToLinear(color.g),
          srgbChannelToLinear(color.b)
        );
      }

      float3 premultipliedSRGBToLinear(float3 premultipliedColor, float alpha) {
        if (alpha <= 0.0001) {
          return float3(0.0);
        }
        float3 straightSRGB = clamp(premultipliedColor / alpha, 0.0, 1.0);
        return srgbToLinear(straightSRGB) * alpha;
      }

      float3 convertLinearBT709ToDisplayP3(float3 rgb) {
        return float3(
          0.8224619687 * rgb.r + 0.1775380313 * rgb.g,
          0.0331941989 * rgb.r + 0.9668058011 * rgb.g,
          0.0170826307 * rgb.r + 0.0723974407 * rgb.g + 0.9105199286 * rgb.b);
      }

      float4 mapSDRUIToOutput(float4 color, bool outputEDR) {
        color = saturate(color);
        if (!outputEDR) {
          return color;
        }
        return float4(convertLinearBT709ToDisplayP3(srgbToLinear(color.rgb)), color.a);
      }

      float valueAt(float4 values, int index) {
        if (index == 0) return values.x;
        if (index == 1) return values.y;
        if (index == 2) return values.z;
        return values.w;
      }

      float4 sampleSourceCacheTexture(
        int slot,
        float2 uv,
        texture2d<float> source0,
        texture2d<float> source1,
        texture2d<float> source2,
        texture2d<float> source3,
        sampler s
      ) {
        if (slot == 0) return source0.sample(s, uv);
        if (slot == 1) return source1.sample(s, uv);
        if (slot == 2) return source2.sample(s, uv);
        return source3.sample(s, uv);
      }

      float4 sampleSourceCacheVideo(
        float2 videoUv,
        constant float4& layoutFlags,
        constant float4& sourcePresentFlags,
        constant float4& sourceOrder,
        constant float4& sourceDisplayOffsetX,
        constant float4& sourceDisplayOffsetY,
        constant float4& sourceInvDisplaySizeX,
        constant float4& sourceInvDisplaySizeY,
        constant float4& sourceViewOffsetUvX,
        constant float4& sourceViewOffsetUvY,
        float4 backgroundColor,
        texture2d<float> source0,
        texture2d<float> source1,
        texture2d<float> source2,
        texture2d<float> source3,
        sampler s
      ) {
        int mode = int(round(layoutFlags.y));
        int trackCount = max(1, int(round(layoutFlags.w)));
        int displaySlot = 0;
        float2 localUv = videoUv;
        if (mode == 0 && trackCount > 1) {
          float count = float(trackCount);
          float scaledX = clamp(videoUv.x, 0.0, 0.999999) * count;
          displaySlot = clamp(int(floor(scaledX)), 0, trackCount - 1);
          localUv = float2(scaledX - float(displaySlot), videoUv.y);
        } else if (mode == 1 && trackCount > 1) {
          float split = clamp(layoutFlags.z, 0.0001, 0.9999);
          displaySlot = videoUv.x < split ? 0 : 1;
          localUv = videoUv;
        }

        int sourceSlot = clamp(int(round(valueAt(sourceOrder, displaySlot))), 0, 3);
        if (valueAt(sourcePresentFlags, sourceSlot) < 0.5) {
          return backgroundColor;
        }

        // The per-slot projection params already encode the full layout
        // (zoom/pan/offset) for the current frame, so no residual transform is
        // layered on top: sample the source directly with the local UV.
        float2 transformed = localUv;
        float2 displayOffset = float2(
          valueAt(sourceDisplayOffsetX, sourceSlot),
          valueAt(sourceDisplayOffsetY, sourceSlot));
        float2 invDisplaySize = float2(
          valueAt(sourceInvDisplaySizeX, sourceSlot),
          valueAt(sourceInvDisplaySizeY, sourceSlot));
        float2 viewOffsetUv = float2(
          valueAt(sourceViewOffsetUvX, sourceSlot),
          valueAt(sourceViewOffsetUvY, sourceSlot));
        float2 sourceUv = (transformed - displayOffset) * invDisplaySize - viewOffsetUv;
        if (sourceUv.x < 0.0 || sourceUv.x > 1.0 ||
            sourceUv.y < 0.0 || sourceUv.y > 1.0) {
          return backgroundColor;
        }
        return sampleSourceCacheTexture(
          sourceSlot,
          sourceUv,
          source0,
          source1,
          source2,
          source3,
          s);
      }

      fragment float4 fs_video(
        VertexOut in [[stage_in]],
        texture2d<float> videoTexture [[texture(0)]],
        texture2d<float> sourceTexture0 [[texture(2)]],
        texture2d<float> sourceTexture1 [[texture(3)]],
        texture2d<float> sourceTexture2 [[texture(4)]],
        texture2d<float> sourceTexture3 [[texture(5)]],
        constant float4& holeRect [[buffer(0)]],
        constant float4& colorFlags [[buffer(1)]],
        constant float4& layoutFlags [[buffer(2)]],
        constant float4& sourcePresentFlags [[buffer(3)]],
        constant float4& sourceOrder [[buffer(4)]],
        constant float4& sourceDisplayOffsetX [[buffer(5)]],
        constant float4& sourceDisplayOffsetY [[buffer(6)]],
        constant float4& sourceInvDisplaySizeX [[buffer(7)]],
        constant float4& sourceInvDisplaySizeY [[buffer(8)]],
        constant float4& sourceViewOffsetUvX [[buffer(9)]],
        constant float4& sourceViewOffsetUvY [[buffer(10)]],
        constant float4& backgroundColor [[buffer(11)]],
        constant float4& compositorFlags [[buffer(12)]]
      ) {
        constexpr sampler s(address::clamp_to_edge, filter::linear);
        float2 uv = clamp(in.uv, 0.0, 1.0);
        bool validHole = holeRect.z > holeRect.x && holeRect.w > holeRect.y;
        bool insideHole = validHole &&
          uv.x >= holeRect.x && uv.x <= holeRect.z &&
          uv.y >= holeRect.y && uv.y <= holeRect.w;
        float2 videoUv = insideHole
          ? float2(
              (uv.x - holeRect.x) / max(0.0001, holeRect.z - holeRect.x),
              (uv.y - holeRect.y) / max(0.0001, holeRect.w - holeRect.y))
          : float2(0.0, 0.0);
        float4 outputBackground = mapSDRUIToOutput(backgroundColor, colorFlags.x > 0.5);
        float4 video = outputBackground;
        if (insideHole) {
          // When the source cache is active (colorFlags.w) the compositor owns
          // the full-layout projection from the source-resolution textures. Else
          // if Dart has already moved the compositor into source-projection
          // mode but the source ring has not published a texture yet, keep the
          // viewport background visible. Sampling the renderer-owned target in
          // that short clear/reinstall window can expose a freshly-cleared black
          // ring buffer during HDR/SDR topology changes.
          if (colorFlags.w > 0.5) {
            video = sampleSourceCacheVideo(
              videoUv,
              layoutFlags,
              sourcePresentFlags,
              sourceOrder,
              sourceDisplayOffsetX,
              sourceDisplayOffsetY,
              sourceInvDisplaySizeX,
              sourceInvDisplaySizeY,
              sourceViewOffsetUvX,
              sourceViewOffsetUvY,
              outputBackground,
              sourceTexture0,
              sourceTexture1,
              sourceTexture2,
              sourceTexture3,
              s);
          } else if (compositorFlags.x > 0.5) {
            video = outputBackground;
          } else {
            video = videoTexture.sample(s, videoUv);
          }
        }
        return colorFlags.y > 0.5
          ? float4(srgbToLinear(video.rgb), video.a)
          : video;
      }

      fragment float4 fs_flutter(
        VertexOut in [[stage_in]],
        texture2d<float> flutterTexture [[texture(0)]],
        constant float4& colorFlags [[buffer(0)]]
      ) {
        constexpr sampler s(address::clamp_to_edge, filter::linear);
        float2 uv = clamp(in.uv, 0.0, 1.0);
        float4 flutter = flutterTexture.sample(s, uv);
        float alpha = clamp(flutter.a, 0.0, 1.0);
        float3 flutterRgb = colorFlags.z > 0.5
          ? premultipliedSRGBToLinear(flutter.rgb, alpha)
          : flutter.rgb;
        return float4(flutterRgb, alpha);
      }

      fragment float4 fs_overlay(OverlayVertexOut in [[stage_in]]) {
        return in.color;
      }
      """
    guard let library = try? device.makeLibrary(source: source, options: nil),
          let vertex = library.makeFunction(name: "vp_main"),
          let overlayVertex = library.makeFunction(name: "overlay_vertex"),
          let videoFragment = library.makeFunction(name: "fs_video"),
          let flutterFragment = library.makeFunction(name: "fs_flutter"),
          let overlayFragment = library.makeFunction(name: "fs_overlay") else {
      return nil
    }
    func makeDescriptor(
      vertex vertexFunction: MTLFunction,
      fragment fragmentFunction: MTLFunction,
      blending: Bool,
      premultipliedSource: Bool
    ) -> MTLRenderPipelineDescriptor {
      let descriptor = MTLRenderPipelineDescriptor()
      descriptor.vertexFunction = vertexFunction
      descriptor.fragmentFunction = fragmentFunction
      descriptor.colorAttachments[0].pixelFormat = outputPixelFormat
      if blending {
        let attachment = descriptor.colorAttachments[0]!
        attachment.isBlendingEnabled = true
        attachment.rgbBlendOperation = .add
        attachment.alphaBlendOperation = .add
        attachment.sourceRGBBlendFactor = premultipliedSource ? .one : .sourceAlpha
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
      }
      return descriptor
    }
    guard
      let video = try? device.makeRenderPipelineState(
        descriptor: makeDescriptor(
          vertex: vertex,
          fragment: videoFragment,
          blending: false,
          premultipliedSource: false
        )
      ),
      let flutter = try? device.makeRenderPipelineState(
        descriptor: makeDescriptor(
          vertex: vertex,
          fragment: flutterFragment,
          blending: true,
          premultipliedSource: true
        )
      ),
      let overlay = try? device.makeRenderPipelineState(
        descriptor: makeDescriptor(
          vertex: overlayVertex,
          fragment: overlayFragment,
          blending: true,
          premultipliedSource: false
        )
      )
    else {
      return nil
    }
    return (video: video, flutter: flutter, overlay: overlay)
  }
}
