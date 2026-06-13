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
  private let pipeline: MTLRenderPipelineState
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
  private let compositeRate = MacOSRateWindow()
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
          let pipeline = Self.makePipeline(
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
    self.pipeline = pipeline
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

  /// Publishes the latest live source buffers (from the source ring). Projection
  /// is set separately via `setSourceProjection`; buffers and projection update
  /// independently (buffers per frame, projection per layout change).
  func setSourceBuffers(
    textures: [MacOSNativeCompositorSourceTexture],
    error: String = ""
  ) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      sourceCacheTextures = textures
      sourceCacheLastError = error
      sourceCacheGeneration &+= 1
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
      sourceProjectionSet = false
      sourceCacheLastError = reason
      sourceCacheGeneration &+= 1
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
      "nativeCompositorViewportTransformEnabled":
        sourceProjectionSet && !sourceCacheTextures.isEmpty,
      "nativeCompositorViewportTransformRequestedEnabled": sourceProjectionSet,
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
    ]) { _, next in next }
    return result
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
      var colorFlags = SIMD4<Float>(
        outputPixelFormat == .rgba16Float ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: video) ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: flutter) ? 1.0 : 0.0,
        sourceCacheActive ? 1.0 : 0.0
      )
      var sourcePresentFlags = sourceCache.presentFlags
      var sourceOrder = sourceCache.order
      var sourceDisplayOffsetX = sourceCache.displayOffsetX
      var sourceDisplayOffsetY = sourceCache.displayOffsetY
      var sourceInvDisplaySizeX = sourceCache.invDisplaySizeX
      var sourceInvDisplaySizeY = sourceCache.invDisplaySizeY
      var sourceViewOffsetUvX = sourceCache.viewOffsetUvX
      var sourceViewOffsetUvY = sourceCache.viewOffsetUvY
      guard inFlightSemaphore.wait(timeout: .now()) == .success else {
        skippedInFlightFrames += 1
        return
      }
      guard let drawable = metalLayer.nextDrawable() else {
        inFlightSemaphore.signal()
        recordFailure("no drawable")
        return
      }
      guard let commandBuffer = commandQueue.makeCommandBuffer(),
            let encoder = commandBuffer.makeRenderCommandEncoder(
              descriptor: renderPassDescriptor(drawable: drawable)
            ) else {
        inFlightSemaphore.signal()
        recordFailure("failed to create command encoder")
        return
      }
      let inFlightSemaphore = self.inFlightSemaphore

      encoder.setRenderPipelineState(pipeline)
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

  private func renderPassDescriptor(drawable: CAMetalDrawable) -> MTLRenderPassDescriptor {
    let descriptor = MTLRenderPassDescriptor()
    descriptor.colorAttachments[0].texture = drawable.texture
    descriptor.colorAttachments[0].loadAction = .clear
    descriptor.colorAttachments[0].storeAction = .store
    descriptor.colorAttachments[0].clearColor = MTLClearColor(
      red: 0.02,
      green: 0.02,
      blue: 0.02,
      alpha: 1.0
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

  private static func makePipeline(
    device: MTLDevice,
    outputPixelFormat: MTLPixelFormat
  ) -> MTLRenderPipelineState? {
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
          return float4(0.0);
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
          return float4(0.0);
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

      fragment float4 fs_main(
        VertexOut in [[stage_in]],
        texture2d<float> videoTexture [[texture(0)]],
        texture2d<float> flutterTexture [[texture(1)]],
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
        constant float4& sourceViewOffsetUvY [[buffer(10)]]
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
        float4 video = float4(0.0);
        if (insideHole) {
          // When the source cache is active (colorFlags.w) the compositor owns
          // the full-layout projection from the source-resolution textures. Else
          // it falls back to the renderer-owned viewport target, which already
          // carries the full layout, so it is sampled directly.
          video = colorFlags.w > 0.5
            ? sampleSourceCacheVideo(
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
                sourceTexture0,
                sourceTexture1,
                sourceTexture2,
                sourceTexture3,
                s)
            : videoTexture.sample(s, videoUv);
        }
        float4 flutter = flutterTexture.sample(s, uv);
        float alpha = clamp(flutter.a, 0.0, 1.0);
        float3 videoRgb = colorFlags.y > 0.5
          ? srgbToLinear(video.rgb)
          : video.rgb;
        float3 flutterRgb = colorFlags.z > 0.5
          ? premultipliedSRGBToLinear(flutter.rgb, alpha)
          : flutter.rgb;
        float3 rgb = flutterRgb + videoRgb * (1.0 - alpha);
        return float4(rgb, 1.0);
      }
      """
    guard let library = try? device.makeLibrary(source: source, options: nil),
          let vertex = library.makeFunction(name: "vp_main"),
          let fragment = library.makeFunction(name: "fs_main") else {
      return nil
    }
    let descriptor = MTLRenderPipelineDescriptor()
    descriptor.vertexFunction = vertex
    descriptor.fragmentFunction = fragment
    descriptor.colorAttachments[0].pixelFormat = outputPixelFormat
    return try? device.makeRenderPipelineState(descriptor: descriptor)
  }
}
