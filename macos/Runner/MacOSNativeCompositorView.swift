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

struct MacOSNativeCompositorSourceProjection {
  let mode: Int
  let splitPos: Double
  let activeTrackCount: Int
  let order: [Int]
  let displayOffsetX: [Double]
  let displayOffsetY: [Double]
  let invDisplaySizeX: [Double]
  let invDisplaySizeY: [Double]
  let viewOffsetUvX: [Double]
  let viewOffsetUvY: [Double]
  let trace: MacOSCompositorLatencyTrace?
}

final class MacOSNativeCompositorView: NSView {
  private let device: MTLDevice
  private let commandQueue: MTLCommandQueue
  private let videoPipeline: MTLRenderPipelineState
  private let flutterPipeline: MTLRenderPipelineState
  private let overlayPipeline: MTLRenderPipelineState
  private let configuration: MacOSPresentationConfiguration
  private let latencyProfiler: MacOSCompositorLatencyProfiler
  private let firstFrameLatency: MacOSFirstFrameLatencyTracker
  private let outputPixelFormat: MTLPixelFormat
  private let outputMode: String
  private let textureCache: CVMetalTextureCache
  private weak var engine: FlutterEngine?
  private weak var videoTexture: MacOSVideoSurface?
  private let metalLayer = CAMetalLayer()
  private let compositorQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.native-compositor",
    qos: .userInteractive
  )
  private let readyStateQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.native-compositor.ready-state",
    qos: .userInteractive
  )
  private let compositorQueueKey = DispatchSpecificKey<Bool>()
  private var displayLink: MacOSViewportDisplayLink?
  private var frameCount = 0
  private var lastVideoTextureAvailable = false
  private var lastFlutterTextureAvailable = false
  private var lastCompositeSucceeded = false
  private var lastFailure = "not drawn"
  private var lastLoggedFailure = ""
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
  private var videoReadySnapshot: VideoTextureSnapshot?
  private var sourceReadyState: SourceCacheReadyState?
  private var videoReadyPublishInFlight = false
  private var videoReadyPublishPending = false
  private var videoReadyToken: UInt64 = 0
  private var sourceReadyToken: UInt64 = 0
  private var videoReadyLastError = ""
  private var sourceReadyLastError = ""
  private var displayTickReuseVideoCount = 0
  private var displayTickReuseSourceCount = 0
  private var displayTickBlockedProducerCount = 0
  private var lastPresentedSourceCacheGeneration: UInt64 = 0
  private let readyVideoAcquireDuration = MacOSDurationWindow()
  private let readySourceAcquireDuration = MacOSDurationWindow()
  private let producerVideoPublishRate = MacOSRateWindow()
  private let producerSourcePublishRate = MacOSRateWindow()
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
  private let displayTickRate = MacOSRateWindow()
  private let staticSkipRate = MacOSRateWindow()
  private let inFlightSkipRate = MacOSRateWindow()
  private let sourceChangeRate = MacOSRateWindow()
  private let videoSourceChangeRate = MacOSRateWindow()
  private let flutterSourceChangeRate = MacOSRateWindow()
  private let sourceCachePublishRate = MacOSRateWindow()
  private let sourceProjectionRate = MacOSRateWindow()
  private let displayTickIntervalDuration = MacOSDurationWindow()
  private let frameCpuDuration = MacOSDurationWindow()
  private let videoAcquireDuration = MacOSDurationWindow()
  private let flutterAcquireDuration = MacOSDurationWindow()
  private let sourceAcquireDuration = MacOSDurationWindow()
  private let inFlightWaitDuration = MacOSDurationWindow()
  private let drawableAcquireDuration = MacOSDurationWindow()
  private let inFlightSemaphore = DispatchSemaphore(value: 2)
  private var pendingCompositeTrace: MacOSCompositorLatencyTrace?
  private var lastDisplayTickNs: UInt64 = 0

  private static let metricsSampleIntervalNs: UInt64 = 1_000_000_000
  private static let displayLinkWarmGraceNs: UInt64 = 250_000_000

  static var isEnabled: Bool {
    MacOSPresentationConfiguration.current.nativeCompositorEnabled
  }

  init?(
    engine: FlutterEngine,
    latencyProfiler: MacOSCompositorLatencyProfiler,
    firstFrameLatency: MacOSFirstFrameLatencyTracker
  ) {
    let configuration = MacOSPresentationConfiguration.current
    let outputPixelFormat = configuration.compositorPixelFormat
    let device = MTLCreateSystemDefaultDevice()
    guard let device,
          let commandQueue = device.makeCommandQueue(),
          let pipelines = MacOSNativeCompositorPipelineFactory.make(
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
    self.latencyProfiler = latencyProfiler
    self.firstFrameLatency = firstFrameLatency
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

  func setVideoTexture(_ texture: MacOSVideoSurface?) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      videoTexture = texture
      videoReadyToken &+= 1
      videoReadySnapshot = nil
      videoReadyLastError = texture == nil ? "no video texture" : ""
      if texture == nil {
        lastVideoTextureAvailable = false
        markCompositorDirty()
        return
      }
      requestVideoReadyPublishOnCompositorQueue(reason: "set-video-texture")
    }
  }

  func requestVideoReadyFrame(reason: String) {
    compositorQueue.async { [weak self] in
      self?.requestVideoReadyPublishOnCompositorQueue(reason: reason)
    }
  }

  func setViewportRect(
    left: Int,
    top: Int,
    width: Int,
    height: Int,
    surfaceWidth: Int,
    surfaceHeight: Int,
    trace: MacOSCompositorLatencyTrace? = nil
  ) {
    guard width > 0, height > 0, surfaceWidth > 0, surfaceHeight > 0 else {
      compositorQueue.async { [weak self] in
        guard let self else { return }
        explicitHoleRect = nil
        if let trace {
          recordPendingCompositeTrace(trace)
        }
        markCompositorDirty()
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
      if let trace {
        recordPendingCompositeTrace(trace)
      }
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

  /// Publishes the latest live source buffers (from the source ring). Same-topology
  /// projection updates may arrive separately; topology-changing packages install
  /// projection and buffers atomically through `setSourcePackage`.
  func setSourceBuffers(
    textures: [MacOSNativeCompositorSourceTexture],
    overlay: MacOSNativeOverlayPrimitives? = nil,
    error: String = ""
  ) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      if let overlay {
        overlayPrimitives = overlay
      }
      sourceCacheLastError = error
      sourceCacheGeneration &+= 1
      sourceReadyToken &+= 1
      let token = sourceReadyToken
      sourceCachePublishRate.record()
      logOverlayPrimitivesIfChanged(reason: "source-buffers")
      guard !textures.isEmpty else {
        sourceReadyState = nil
        sourceReadyLastError = error
        markCompositorDirty()
        return
      }
      publishSourceReadyState(
        textures: textures,
        token: token,
        generation: sourceCacheGeneration,
        error: error,
        projection: nil
      )
    }
  }

  func setSourcePackage(
    textures: [MacOSNativeCompositorSourceTexture],
    projection: MacOSNativeCompositorSourceProjection,
    overlay: MacOSNativeOverlayPrimitives? = nil,
    error: String = ""
  ) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      if let overlay {
        overlayPrimitives = overlay
      }
      sourceCacheLastError = error
      sourceCacheGeneration &+= 1
      sourceReadyToken &+= 1
      let token = sourceReadyToken
      sourceCachePublishRate.record()
      logOverlayPrimitivesIfChanged(reason: "source-package")
      guard !textures.isEmpty else {
        sourceReadyState = nil
        sourceReadyLastError = error
        markCompositorDirty()
        return
      }
      publishSourceReadyState(
        textures: textures,
        token: token,
        generation: sourceCacheGeneration,
        error: error,
        projection: projection
      )
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
      if sourceReadyState == nil && !sourceProjectionSet {
        return
      }
      sourceReadyState = nil
      sourceReadyToken &+= 1
      overlayPrimitives = .empty
      sourceProjectionSet = false
      sourceCacheLastError = reason
      sourceReadyLastError = reason
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
    viewOffsetUvY: [Double],
    trace: MacOSCompositorLatencyTrace? = nil
  ) {
    let projection = MacOSNativeCompositorSourceProjection(
      mode: mode,
      splitPos: splitPos,
      activeTrackCount: activeTrackCount,
      order: order,
      displayOffsetX: displayOffsetX,
      displayOffsetY: displayOffsetY,
      invDisplaySizeX: invDisplaySizeX,
      invDisplaySizeY: invDisplaySizeY,
      viewOffsetUvX: viewOffsetUvX,
      viewOffsetUvY: viewOffsetUvY,
      trace: trace
    )
    compositorQueue.async { [weak self] in
      guard let self else { return }
      applySourceProjectionOnCompositorQueue(projection)
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
    let compositeHz = compositeRate.rateHz()
    let sourceCacheBytes = sourceReadyState?.bytes ?? 0
    let sourceCacheTextureCount = sourceReadyState?.textures.count ?? 0
    result["nativeCompositorEnabled"] = true
    result["nativeCompositorFrames"] = frameCount
    result["nativeCompositorCompositeHz"] = compositeHz
    result["nativeCompositorCompositeHzX1000"] = Int(compositeHz * 1000.0)
    result["nativeCompositorDisplayTickHz"] = displayTickRate.rateHz()
    result["nativeCompositorDisplayTickHzX1000"] = Int(displayTickRate.rateHz() * 1000.0)
    result["nativeCompositorDisplayTickIntervalLastMs"] = displayTickIntervalDuration.lastMs()
    result["nativeCompositorDisplayTickIntervalP95Ms"] = displayTickIntervalDuration.p95Ms()
    result["nativeCompositorFrameCpuLastMs"] = frameCpuDuration.lastMs()
    result["nativeCompositorFrameCpuP95Ms"] = frameCpuDuration.p95Ms()
    result["nativeCompositorVideoAcquireLastMs"] = videoAcquireDuration.lastMs()
    result["nativeCompositorVideoAcquireP95Ms"] = videoAcquireDuration.p95Ms()
    result["readyVideoAcquireLastMs"] = readyVideoAcquireDuration.lastMs()
    result["readyVideoAcquireP95Ms"] = readyVideoAcquireDuration.p95Ms()
    result["nativeCompositorFlutterAcquireLastMs"] = flutterAcquireDuration.lastMs()
    result["nativeCompositorFlutterAcquireP95Ms"] = flutterAcquireDuration.p95Ms()
    result["nativeCompositorSourceAcquireLastMs"] = sourceAcquireDuration.lastMs()
    result["nativeCompositorSourceAcquireP95Ms"] = sourceAcquireDuration.p95Ms()
    result["readySourceAcquireLastMs"] = readySourceAcquireDuration.lastMs()
    result["readySourceAcquireP95Ms"] = readySourceAcquireDuration.p95Ms()
    result["nativeCompositorInFlightWaitLastMs"] = inFlightWaitDuration.lastMs()
    result["nativeCompositorInFlightWaitP95Ms"] = inFlightWaitDuration.p95Ms()
    result["nativeCompositorDrawableAcquireLastMs"] = drawableAcquireDuration.lastMs()
    result["nativeCompositorDrawableAcquireP95Ms"] = drawableAcquireDuration.p95Ms()
    result["nativeCompositorBackendSubmitCpuLastMs"] = 0.0
    result["nativeCompositorBackendSubmitCpuP95Ms"] = 0.0
    result["nativeCompositorBackendCompletionLastMs"] = 0.0
    result["nativeCompositorBackendCompletionP95Ms"] = 0.0
    result["nativeCompositorBackendLastSubmittedFrame"] = 0
    result["nativeCompositorBackendLastCompletedFrame"] = 0
    result["nativeCompositorBackendPendingCompletionCount"] = 0
    result["nativeCompositorBackendLastCompletionResult"] = 0
    result["nativeCompositorStaticSkipHz"] = staticSkipRate.rateHz()
    result["nativeCompositorInFlightSkipHz"] = inFlightSkipRate.rateHz()
    result["nativeCompositorSourceChangeHz"] = sourceChangeRate.rateHz()
    result["nativeCompositorVideoSourceChangeHz"] = videoSourceChangeRate.rateHz()
    result["nativeCompositorFlutterSourceChangeHz"] = flutterSourceChangeRate.rateHz()
    result["nativeCompositorVideoTextureAvailable"] = lastVideoTextureAvailable
    result["nativeCompositorFlutterTextureAvailable"] = lastFlutterTextureAvailable
    result["nativeCompositorLastCompositeSucceeded"] = lastCompositeSucceeded
    result["nativeCompositorLastFailure"] = lastFailure
    result["nativeCompositorFlutterAlphaAverageX1000"] = lastFlutterAlphaAverageX1000
    result["nativeCompositorFlutterTransparentRatioX1000"] =
      lastFlutterTransparentRatioX1000
    result["nativeCompositorHoleLeftX1000"] = Int(lastHoleRect.x * 1000.0)
    result["nativeCompositorHoleTopX1000"] = Int(lastHoleRect.y * 1000.0)
    result["nativeCompositorHoleRightX1000"] = Int(lastHoleRect.z * 1000.0)
    result["nativeCompositorHoleBottomX1000"] = Int(lastHoleRect.w * 1000.0)
    result["nativeCompositorDrawableWidth"] = Int(metalLayer.drawableSize.width)
    result["nativeCompositorDrawableHeight"] = Int(metalLayer.drawableSize.height)
    result["nativeCompositorOutputMode"] = outputMode
    result["nativeCompositorBackend"] = "metal"
    result["nativeCompositorOutputPixelFormat"] = String(describing: outputPixelFormat)
    result["nativeCompositorEDREnabled"] = outputPixelFormat == .rgba16Float
    result["nativeCompositorEDRWantsExtendedDynamicRangeContent"] =
      metalLayer.wantsExtendedDynamicRangeContent
    result["nativeCompositorVideoPixelFormat"] = lastVideoPixelFormat
    result["nativeCompositorEDRVideoSampleCount"] = lastEDRVideoSampleCount
    result["nativeCompositorEDRVideoMaxRGBX1000"] = lastEDRVideoMaxRGBX1000
    result["nativeCompositorEDRVideoPixelsOver1X1000"] =
      lastEDRVideoPixelsOver1X1000
    result["nativeCompositorVideoSRGBToLinearEnabled"] = lastVideoSRGBToLinearEnabled
    result["nativeCompositorFlutterSRGBToLinearEnabled"] =
      lastFlutterSRGBToLinearEnabled
    result["nativeCompositorSkippedInFlightFrames"] = skippedInFlightFrames
    result["nativeCompositorSkippedStaticFrames"] = skippedStaticFrames
    result["producerVideoPublishHz"] = producerVideoPublishRate.rateHz()
    result["producerVideoPublishHzX1000"] = Int(producerVideoPublishRate.rateHz() * 1000.0)
    result["producerSourcePublishHz"] = producerSourcePublishRate.rateHz()
    result["producerSourcePublishHzX1000"] = Int(producerSourcePublishRate.rateHz() * 1000.0)
    result["displayTickReuseVideoCount"] = displayTickReuseVideoCount
    result["displayTickReuseSourceCount"] = displayTickReuseSourceCount
    result["displayTickBlockedProducerCount"] = displayTickBlockedProducerCount
    result["nativeCompositorVideoReadyLastError"] = videoReadyLastError
    result["nativeCompositorSourceReadyLastError"] = sourceReadyLastError
    result["rendererOwnedCompositeProducerSubmitCount"] = 0
    result["rendererOwnedCompositeSkippedWhileInFlight"] = skippedInFlightFrames
    result["nativeCompositorViewportTransformEnabled"] = false
    result["nativeCompositorViewportTransformRequestedEnabled"] = false
    result["nativeCompositorViewportTransformGeneration"] = Int(
      min(sourceCacheGeneration, UInt64(Int.max))
    )
    result["nativeCompositorDisplayedLayoutRevision"] = Int(
      min(displayedLayoutRevision, UInt64(Int.max))
    )
    result["nativeCompositorViewportTransformBaseDisplayedLayoutRevision"] = Int(
      min(displayedLayoutRevision, UInt64(Int.max))
    )
    result["nativeCompositorViewportTransformScaleXX1000"] = 1000
    result["nativeCompositorViewportTransformScaleYX1000"] = 1000
    result["nativeCompositorViewportTransformTranslateXX1000"] = 0
    result["nativeCompositorViewportTransformTranslateYX1000"] = 0
    result["nativeCompositorSourceProjectionEnabled"] = sourceProjectionSet
    result["nativeCompositorSourceCacheActive"] =
      sourceProjectionSet && sourceCacheTextureCount > 0
    result["nativeCompositorSourceCacheTextureCount"] = sourceCacheTextureCount
    result["nativeCompositorSourceSlotSignature"] =
      sourceReadyState?.slotSignature ?? ""
    result["nativeCompositorSourceFileIdSignature"] =
      sourceReadyState?.fileIdSignature ?? ""
    result["nativeCompositorSourceTextureSignature"] =
      sourceReadyState?.textureSignature ?? ""
    result["nativeCompositorSourceDuplicateSlotCount"] =
      sourceReadyState?.duplicateSlotCount ?? 0
    result["nativeCompositorSourceDuplicateFileIdCount"] =
      sourceReadyState?.duplicateFileIdCount ?? 0
    result["nativeCompositorSourceDuplicateTextureCount"] =
      sourceReadyState?.duplicateTextureCount ?? 0
    result["nativeCompositorSourceCacheGeneration"] = Int(
      min(sourceCacheGeneration, UInt64(Int.max))
    )
    result["nativeCompositorSourceCacheBytes"] = sourceCacheBytes
    result["nativeCompositorSourceCacheLastError"] = sourceCacheLastError
    result["nativeCompositorOverlayGeneration"] = Int(
      min(overlayPrimitives.generation, UInt64(Int.max))
    )
    result["nativeCompositorOverlayFillRectCount"] = overlayPrimitives.fillRects.count
    result["nativeCompositorOverlayLineRectCount"] = overlayPrimitives.lineRects.count
    result["nativeCompositorOverlayMotionLineCount"] = overlayPrimitives.motionLines.count
    result["nativeCompositorSourceBakedOverlayDisabled"] =
      overlayPrimitives.sourceBakedOverlayDisabled
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
    result["nativeCompositorSourceProjectionApplyCount"] = sourceProjectionRate.total()
    result["nativeCompositorSource0DisplayOffsetXX1000"] = Int(sourceProjDisplayOffsetX.x * 1000.0)
    result["nativeCompositorSource0DisplayOffsetYX1000"] = Int(sourceProjDisplayOffsetY.x * 1000.0)
    result["nativeCompositorSource0InvDisplaySizeXX1000"] = Int(sourceProjInvDisplaySizeX.x * 1000.0)
    result["nativeCompositorSource0InvDisplaySizeYX1000"] = Int(sourceProjInvDisplaySizeY.x * 1000.0)
    result["nativeCompositorSource1DisplayOffsetXX1000"] = Int(sourceProjDisplayOffsetX.y * 1000.0)
    result["nativeCompositorSource1DisplayOffsetYX1000"] = Int(sourceProjDisplayOffsetY.y * 1000.0)
    result["nativeCompositorSource1InvDisplaySizeXX1000"] = Int(sourceProjInvDisplaySizeX.y * 1000.0)
    result["nativeCompositorSource1InvDisplaySizeYX1000"] = Int(sourceProjInvDisplaySizeY.y * 1000.0)
    result.merge(latencyProfiler.diagnosticMap()) { _, next in next }
    return result
  }

  private func logOverlayPrimitivesIfChanged(reason: String) {
    guard MacOSProfilerLog.enabled else { return }
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
      let tickStartNs = DispatchTime.now().uptimeNanoseconds
      displayTickRate.record(nowNs: tickStartNs)
      if lastDisplayTickNs > 0, tickStartNs >= lastDisplayTickNs {
        displayTickIntervalDuration.record(tickStartNs - lastDisplayTickNs)
      }
      lastDisplayTickNs = tickStartNs

      let videoAcquireStartNs = DispatchTime.now().uptimeNanoseconds
      guard let videoSnapshot = videoReadySnapshot else {
        videoAcquireDuration.record(Self.elapsedNs(from: videoAcquireStartNs))
        setHiddenOnMain(true)
        if videoTexture != nil {
          recordFailure(videoReadyLastError.isEmpty ? "no video ready texture" : videoReadyLastError)
        }
        return
      }
      videoAcquireDuration.record(Self.elapsedNs(from: videoAcquireStartNs))
      let flutterAcquireStartNs = DispatchTime.now().uptimeNanoseconds
      guard let flutterSnapshot = currentFlutterMetalTexture() else {
        flutterAcquireDuration.record(Self.elapsedNs(from: flutterAcquireStartNs))
        setHiddenOnMain(true)
        recordFailure("no Flutter texture")
        return
      }
      flutterAcquireDuration.record(Self.elapsedNs(from: flutterAcquireStartNs))
      let nowNs = DispatchTime.now().uptimeNanoseconds
      let sourceChanged =
        videoSnapshot.sourceKey != lastPresentedVideoSourceKey ||
        flutterSnapshot.sourceKey != lastPresentedFlutterSourceKey
      if sourceChanged {
        sourceChangeRate.record(nowNs: nowNs)
      }
      if videoSnapshot.sourceKey != lastPresentedVideoSourceKey {
        videoSourceChangeRate.record(nowNs: nowNs)
      }
      if flutterSnapshot.sourceKey != lastPresentedFlutterSourceKey {
        flutterSourceChangeRate.record(nowNs: nowNs)
      }
      if frameCount > 0 && videoSnapshot.sourceKey == lastPresentedVideoSourceKey {
        displayTickReuseVideoCount += 1
      }
      if !compositorDirty && !sourceChanged && nowNs >= displayLinkWarmUntilNs {
        skippedStaticFrames += 1
        staticSkipRate.record(nowNs: nowNs)
        return
      }
      let video = videoSnapshot.texture
      let flutter = flutterSnapshot.texture
      let sourceAcquireStartNs = DispatchTime.now().uptimeNanoseconds
      let sourceCache = currentSourceCacheMetalTextures()
      sourceAcquireDuration.record(Self.elapsedNs(from: sourceAcquireStartNs))
      if sourceProjectionSet &&
         sourceCache.generation > 0 &&
         sourceCache.generation == lastPresentedSourceCacheGeneration {
        displayTickReuseSourceCount += 1
      }
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
      let inFlightWaitStartNs = DispatchTime.now().uptimeNanoseconds
      guard inFlightSemaphore.wait(timeout: .now()) == .success else {
        inFlightWaitDuration.record(Self.elapsedNs(from: inFlightWaitStartNs))
        skippedInFlightFrames += 1
        inFlightSkipRate.record()
        return
      }
      inFlightWaitDuration.record(Self.elapsedNs(from: inFlightWaitStartNs))
      let drawableAcquireStartNs = DispatchTime.now().uptimeNanoseconds
      guard let drawable = metalLayer.nextDrawable() else {
        drawableAcquireDuration.record(Self.elapsedNs(from: drawableAcquireStartNs))
        inFlightSemaphore.signal()
        recordFailure("no drawable")
        return
      }
      drawableAcquireDuration.record(Self.elapsedNs(from: drawableAcquireStartNs))
      let overlayProjection = MacOSNativeOverlayProjection(
        layoutFlags: sourceLayoutFlags,
        sourceOrder: sourceOrder,
        displayOffsetX: sourceProjDisplayOffsetX,
        displayOffsetY: sourceProjDisplayOffsetY,
        invDisplaySizeX: sourceProjInvDisplaySizeX,
        invDisplaySizeY: sourceProjInvDisplaySizeY,
        viewOffsetUvX: sourceProjViewOffsetUvX,
        viewOffsetUvY: sourceProjViewOffsetUvY
      )
      let overlayVertices = sourceCacheActive
        ? overlayProjection.buildVertices(
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
      let firstFrameLatency = self.firstFrameLatency
      commandBuffer.addCompletedHandler { _ in
        inFlightSemaphore.signal()
        if sourceCacheActive,
           let summary = firstFrameLatency.markCompositeCompleted() {
          NSLog("VoidPlayer native add first-frame latency: \(summary)")
        }
      }
      commandBuffer.present(drawable)
      commandBuffer.commit()

      setHiddenOnMain(false)
      frameCount += 1
      compositeRate.record()
      if let trace = pendingCompositeTrace {
        pendingCompositeTrace = nil
        latencyProfiler.recordComposited(trace, compositeNs: nowNs)
      }
      lastVideoTextureAvailable = true
      lastFlutterTextureAvailable = true
      lastCompositeSucceeded = true
      lastFailure = ""
      lastVideoSRGBToLinearEnabled = colorFlags.y > 0.5
      lastFlutterSRGBToLinearEnabled = colorFlags.z > 0.5
      lastPresentedVideoSourceKey = videoSnapshot.sourceKey
      lastPresentedFlutterSourceKey = flutterSnapshot.sourceKey
      lastPresentedSourceCacheGeneration = sourceCache.generation
      compositorDirty = false
      frameCpuDuration.record(Self.elapsedNs(from: tickStartNs))
      logCompositeFrameSummary(
        frame: frameCount,
        video: video,
        flutter: flutter,
        sourceCacheActive: sourceCacheActive,
        sourceProjectionEnabled: sourceProjectionEnabled,
        holeRect: holeRect,
        backgroundColor: backgroundColor,
        sourceDisplayOffsetX: sourceDisplayOffsetX,
        sourceDisplayOffsetY: sourceDisplayOffsetY,
        sourceInvDisplaySizeX: sourceInvDisplaySizeX,
        sourceInvDisplaySizeY: sourceInvDisplaySizeY
      )
    }
  }

  private func markCompositorDirty() {
    compositorDirty = true
    displayLinkWarmUntilNs = DispatchTime.now().uptimeNanoseconds + Self.displayLinkWarmGraceNs
  }

  private func requestVideoReadyPublishOnCompositorQueue(reason _: String) {
    guard let videoTexture else {
      videoReadyLastError = "no video texture"
      lastVideoTextureAvailable = false
      return
    }
    if videoReadyPublishInFlight {
      videoReadyPublishPending = true
      return
    }
    videoReadyPublishInFlight = true
    videoReadyPublishPending = false
    let token = videoReadyToken
    readyStateQueue.async { [weak self] in
      guard let self else { return }
      let startNs = DispatchTime.now().uptimeNanoseconds
      let snapshot = self.makeVideoReadySnapshot(videoTexture: videoTexture)
      let elapsedNs = Self.elapsedNs(from: startNs)
      self.compositorQueue.async { [weak self] in
        guard let self else { return }
        let shouldRetry = videoReadyPublishPending || token != videoReadyToken
        videoReadyPublishInFlight = false
        videoReadyPublishPending = false
        guard token == videoReadyToken else {
          if shouldRetry {
            requestVideoReadyPublishOnCompositorQueue(reason: "video-ready-token-advanced")
          }
          return
        }
        readyVideoAcquireDuration.record(elapsedNs)
        guard let snapshot else {
          videoReadyLastError = "video ready snapshot unavailable"
          lastVideoTextureAvailable = false
          if shouldRetry {
            requestVideoReadyPublishOnCompositorQueue(reason: "video-ready-retry")
          }
          return
        }
        videoReadySnapshot = snapshot
        videoReadyLastError = ""
        lastVideoTextureAvailable = true
        maybeUpdateVideoEDRMetrics(pixelBuffer: snapshot.pixelBuffer)
        producerVideoPublishRate.record()
        markCompositorDirty()
        if shouldRetry {
          requestVideoReadyPublishOnCompositorQueue(reason: "video-ready-pending")
        }
      }
    }
  }

  private func publishSourceReadyState(
    textures: [MacOSNativeCompositorSourceTexture],
    token: UInt64,
    generation: UInt64,
    error: String,
    projection: MacOSNativeCompositorSourceProjection?
  ) {
    readyStateQueue.async { [weak self] in
      guard let self else { return }
      let startNs = DispatchTime.now().uptimeNanoseconds
      let state = self.makeSourceReadyState(textures: textures, generation: generation)
      let elapsedNs = Self.elapsedNs(from: startNs)
      self.compositorQueue.async { [weak self] in
        guard let self, token == sourceReadyToken else { return }
        readySourceAcquireDuration.record(elapsedNs)
        guard let state else {
          sourceReadyState = nil
          sourceReadyLastError = error.isEmpty
            ? "source ready texture import failed"
            : error
          markCompositorDirty()
          return
        }
        if let projection {
          applySourceProjectionOnCompositorQueue(projection)
        }
        sourceReadyState = state
        sourceReadyLastError = error
        producerSourcePublishRate.record()
        markCompositorDirty()
      }
    }
  }

  private func applySourceProjectionOnCompositorQueue(
    _ projection: MacOSNativeCompositorSourceProjection
  ) {
    let safeSplit = projection.splitPos.isFinite
      ? Float(min(1.0, max(0.0, projection.splitPos)))
      : 0.5
    sourceLayoutFlags = SIMD4<Float>(
      0,
      Float(projection.mode),
      safeSplit,
      Float(max(1, projection.activeTrackCount))
    )
    sourceOrder = SIMD4<Float>(
      Float(projection.order.indices.contains(0) ? projection.order[0] : 0),
      Float(projection.order.indices.contains(1) ? projection.order[1] : 1),
      Float(projection.order.indices.contains(2) ? projection.order[2] : 2),
      Float(projection.order.indices.contains(3) ? projection.order[3] : 3)
    )
    func vec(_ values: [Double]) -> SIMD4<Float> {
      SIMD4<Float>(
        Float(values.indices.contains(0) ? values[0] : 0),
        Float(values.indices.contains(1) ? values[1] : 0),
        Float(values.indices.contains(2) ? values[2] : 0),
        Float(values.indices.contains(3) ? values[3] : 0)
      )
    }
    sourceProjDisplayOffsetX = vec(projection.displayOffsetX)
    sourceProjDisplayOffsetY = vec(projection.displayOffsetY)
    sourceProjInvDisplaySizeX = vec(projection.invDisplaySizeX)
    sourceProjInvDisplaySizeY = vec(projection.invDisplaySizeY)
    sourceProjViewOffsetUvX = vec(projection.viewOffsetUvX)
    sourceProjViewOffsetUvY = vec(projection.viewOffsetUvY)
    sourceProjectionSet = true
    sourceProjectionRate.record()
    if let trace = projection.trace {
      recordPendingCompositeTrace(trace)
    }
  }

  private func makeVideoReadySnapshot(videoTexture: MacOSVideoSurface) -> VideoTextureSnapshot? {
    guard let presentationSnapshot = videoTexture.presentationSnapshot() else {
      return nil
    }
    let pixelBuffer = presentationSnapshot.pixelBuffer.takeRetainedValue()
    let generation = presentationSnapshot.generation
    let sourceKey = generation > 0
      ? UInt64(generation)
      : UInt64(UInt(bitPattern: Unmanaged.passUnretained(pixelBuffer).toOpaque()))
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
      return nil
    }
    return VideoTextureSnapshot(
      texture: texture,
      pixelBuffer: pixelBuffer,
      cvTexture: cvTexture,
      sourceKey: sourceKey,
      layoutRevision: presentationSnapshot.layoutRevision
    )
  }

  private func makeSourceReadyState(
    textures entries: [MacOSNativeCompositorSourceTexture],
    generation: UInt64
  ) -> SourceCacheReadyState? {
    var textures: [Int: MTLTexture] = [:]
    var pixelBuffers: [CVPixelBuffer] = []
    var cvTextures: [CVMetalTexture] = []
    var presentFlags = SIMD4<Float>(0, 0, 0, 0)
    var bytes = 0
    var slots: [Int] = []
    var fileIds: [Int] = []
    var textureIds: [UInt] = []
    var slotParts: [String] = []

    for entry in entries {
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
      let textureId = UInt(bitPattern: Unmanaged.passUnretained(texture as AnyObject).toOpaque())
      slots.append(slot)
      fileIds.append(entry.fileId)
      textureIds.append(textureId)
      slotParts.append("s\(slot):f\(entry.fileId):t\(String(textureId, radix: 16))")
      pixelBuffers.append(pixelBuffer)
      cvTextures.append(cvTexture)
      bytes += CVPixelBufferGetBytesPerRow(pixelBuffer) * CVPixelBufferGetHeight(pixelBuffer)
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

    guard !textures.isEmpty else { return nil }
    let sortedSlotParts = slotParts.sorted()
    return SourceCacheReadyState(
      textures: textures,
      pixelBuffers: pixelBuffers,
      cvTextures: cvTextures,
      presentFlags: presentFlags,
      generation: generation,
      bytes: bytes,
      slotSignature: sortedSlotParts.joined(separator: "|"),
      fileIdSignature: fileIds.map(String.init).joined(separator: ","),
      textureSignature: textureIds.map { String($0, radix: 16) }.joined(separator: ","),
      duplicateSlotCount: max(0, slots.count - Set(slots).count),
      duplicateFileIdCount: max(0, fileIds.count - Set(fileIds).count),
      duplicateTextureCount: max(0, textureIds.count - Set(textureIds).count)
    )
  }

  private func logCompositeFrameSummary(
    frame: Int,
    video: MTLTexture,
    flutter: MTLTexture,
    sourceCacheActive: Bool,
    sourceProjectionEnabled: Bool,
    holeRect: SIMD4<Float>,
    backgroundColor: SIMD4<Float>,
    sourceDisplayOffsetX: SIMD4<Float>,
    sourceDisplayOffsetY: SIMD4<Float>,
    sourceInvDisplaySizeX: SIMD4<Float>,
    sourceInvDisplaySizeY: SIMD4<Float>
  ) {
    guard MacOSProfilerLog.enabled && (frame == 1 || frame % 120 == 0) else {
      return
    }
    let compositeSummary = String(
      format:
        "NativeCompositorComposite frame=%d backend=%@ mode=%@ video=%dx%d flutter=%dx%d drawable=%dx%d layoutRevision=%llu sourceProjection=%d sourceProjectionEnabled=%d hole=%.4f,%.4f,%.4f,%.4f source0Offset=%.4f,%.4f source0InvSize=%.4f,%.4f bg=%.3f,%.3f,%.3f,%.3f frameCpuLastMs=%.2f videoAcquireLastMs=%.2f flutterAcquireLastMs=%.2f sourceAcquireLastMs=%.2f drawableAcquireLastMs=%.2f",
        frame,
      "metal",
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
      backgroundColor.w,
      frameCpuDuration.lastMs(),
      videoAcquireDuration.lastMs(),
      flutterAcquireDuration.lastMs(),
      sourceAcquireDuration.lastMs(),
      drawableAcquireDuration.lastMs()
    )
    compositeSummary.withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
  }

  private func recordPendingCompositeTrace(_ trace: MacOSCompositorLatencyTrace) {
    if pendingCompositeTrace != nil {
      latencyProfiler.recordCoalescedBeforeComposite()
    }
    latencyProfiler.recordApplied(trace, applyNs: DispatchTime.now().uptimeNanoseconds)
    pendingCompositeTrace = trace
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

  private static func elapsedNs(from startNs: UInt64) -> UInt64 {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    return nowNs >= startNs ? nowNs - startNs : 0
  }

  private static func rawPointer(_ texture: MTLTexture) -> UnsafeMutableRawPointer {
    UnsafeMutableRawPointer(Unmanaged.passUnretained(texture as AnyObject).toOpaque())
  }

  private static func writeFixedInt32Array<T>(_ storage: inout T, _ values: [Int32]) {
    withUnsafeMutableBytes(of: &storage) { raw in
      let buffer = raw.bindMemory(to: Int32.self)
      for i in 0..<min(buffer.count, values.count) {
        buffer[i] = values[i]
      }
    }
  }

  private static func writeFixedFloatArray<T>(_ storage: inout T, _ values: [Float]) {
    withUnsafeMutableBytes(of: &storage) { raw in
      let buffer = raw.bindMemory(to: Float.self)
      for i in 0..<min(buffer.count, values.count) {
        buffer[i] = values[i]
      }
    }
  }

  private static func writeFrameArray<T>(
    _ storage: inout T,
    _ body: (UnsafeMutableBufferPointer<VPMacOSNativePresentFrameInfo>) -> Void
  ) {
    withUnsafeMutableBytes(of: &storage) { raw in
      body(raw.bindMemory(to: VPMacOSNativePresentFrameInfo.self))
    }
  }

  private struct VideoTextureSnapshot {
    let texture: MTLTexture
    let pixelBuffer: CVPixelBuffer
    let cvTexture: CVMetalTexture
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
    let generation: UInt64
    let order: SIMD4<Float>
    let displayOffsetX: SIMD4<Float>
    let displayOffsetY: SIMD4<Float>
    let invDisplaySizeX: SIMD4<Float>
    let invDisplaySizeY: SIMD4<Float>
    let viewOffsetUvX: SIMD4<Float>
    let viewOffsetUvY: SIMD4<Float>
  }

  private struct SourceCacheReadyState {
    let textures: [Int: MTLTexture]
    let pixelBuffers: [CVPixelBuffer]
    let cvTextures: [CVMetalTexture]
    let presentFlags: SIMD4<Float>
    let generation: UInt64
    let bytes: Int
    let slotSignature: String
    let fileIdSignature: String
    let textureSignature: String
    let duplicateSlotCount: Int
    let duplicateFileIdCount: Int
    let duplicateTextureCount: Int
  }

  private func currentSourceCacheMetalTextures() -> SourceCacheTextureSnapshot {
    let ready = sourceReadyState
    return SourceCacheTextureSnapshot(
      textures: ready?.textures ?? [:],
      presentFlags: ready?.presentFlags ?? SIMD4<Float>(0, 0, 0, 0),
      generation: ready?.generation ?? 0,
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
      recordFailure("Flutter surface missing IOSurface")
      return
    }
    let surfaceObject = rawSurface as AnyObject
    guard CFGetTypeID(surfaceObject) == IOSurfaceGetTypeID() else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      recordFailure("Flutter surface IOSurface type mismatch")
      return
    }
    let ioSurface = unsafeBitCast(surfaceObject, to: IOSurfaceRef.self)
    let pixelFormat = IOSurfaceGetPixelFormat(ioSurface)
    guard pixelFormat == kCVPixelFormatType_32BGRA else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      recordFailure("Flutter surface unsupported pixel format")
      return
    }

    let lockResult = IOSurfaceLock(ioSurface, .readOnly, nil)
    guard lockResult == kIOReturnSuccess else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      recordFailure("Flutter surface IOSurfaceLock failed")
      return
    }
    defer { IOSurfaceUnlock(ioSurface, .readOnly, nil) }
    let baseAddress = IOSurfaceGetBaseAddress(ioSurface)
    guard Int(bitPattern: baseAddress) != 0 else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      lastHoleRect = SIMD4<Float>(0, 0, 0, 0)
      recordFailure("Flutter surface missing base address")
      return
    }

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
    if message != lastLoggedFailure || frameCount == 1 || frameCount % 120 == 0 {
      lastLoggedFailure = message
      NSLog(
        "VoidPlayer native compositor failure backend=metal frame=\(frameCount): \(message)"
      )
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

}
