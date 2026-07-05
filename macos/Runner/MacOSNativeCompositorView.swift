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
  let layoutFlags: SIMD4<Float>
  let order: SIMD4<Float>
  let displayOffsetX: SIMD4<Float>
  let displayOffsetY: SIMD4<Float>
  let invDisplaySizeX: SIMD4<Float>
  let invDisplaySizeY: SIMD4<Float>
  let viewOffsetUvX: SIMD4<Float>
  let viewOffsetUvY: SIMD4<Float>
  let trace: MacOSCompositorLatencyTrace?
}

protocol MacOSNativeCompositorSourceReadyCompletionTarget: AnyObject {
  func nativeCompositorSourceReadyCompletion(success: Bool)
}

private final class MacOSNativeCompositorWgpuCallbackContext {
  let view: MacOSNativeCompositorView
  let frame: Int
  let submittedNs: UInt64

  init(view: MacOSNativeCompositorView, frame: Int, submittedNs: UInt64) {
    self.view = view
    self.frame = frame
    self.submittedNs = submittedNs
  }
}

private let macOSNativeCompositorWgpuCompletion: @convention(c) (
  UnsafeMutableRawPointer?,
  Int32
) -> Void = { userData, result in
  guard let userData else {
    return
  }
  let context = Unmanaged<MacOSNativeCompositorWgpuCallbackContext>
    .fromOpaque(userData)
    .takeRetainedValue()
  context.view.completeWgpuCompositeFromCallback(
    result: result,
    frame: context.frame,
    submittedNs: context.submittedNs,
    completedNs: DispatchTime.now().uptimeNanoseconds
  )
}

final class MacOSNativeCompositorView: NSView {
  private let device: MTLDevice
  private let commandQueue: MTLCommandQueue
  private let videoPipeline: MTLRenderPipelineState
  private let flutterPipeline: MTLRenderPipelineState
  private let overlayPipeline: MTLRenderPipelineState
  private let configuration: MacOSPresentationConfiguration
  private let latencyProfiler: MacOSCompositorLatencyProfiler
  private let outputPixelFormat: MTLPixelFormat
  private let outputMode: String
  private let useWgpuCompositor: Bool
  private let textureCache: CVMetalTextureCache
  private var wgpuRenderer: OpaquePointer?
  private weak var engine: FlutterEngine?
  private weak var videoTexture: MacOSVideoTexture?
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
  private let wgpuSubmitCpuDuration = MacOSDurationWindow()
  private let wgpuCompletionDuration = MacOSDurationWindow()
  private let inFlightSemaphore = DispatchSemaphore(value: 2)
  private var pendingCompositeTrace: MacOSCompositorLatencyTrace?
  private var lastDisplayTickNs: UInt64 = 0
  private var lastWgpuProfilerSnapshot = VPWgpuMetalProfilerSnapshot()
  private var lastWgpuCompletionResult: Int32 = 0

  private static let metricsSampleIntervalNs: UInt64 = 1_000_000_000
  private static let displayLinkWarmGraceNs: UInt64 = 250_000_000

  static var isEnabled: Bool {
    MacOSPresentationConfiguration.current.nativeCompositorEnabled
  }

  init?(engine: FlutterEngine, latencyProfiler: MacOSCompositorLatencyProfiler) {
    let configuration = MacOSPresentationConfiguration.current
    let outputPixelFormat = configuration.compositorPixelFormat
    let useWgpuCompositor = configuration.request == "wgpu-metal" || configuration.request == "wgpu"
    var wgpuRenderer: OpaquePointer?
    let device: MTLDevice?
    if useWgpuCompositor {
      var error = [CChar](repeating: 0, count: 512)
      guard VPWgpuFfiVersion() >= VP_WGPU_FFI_ABI_VERSION,
            let renderer = VPWgpuMetalRendererCreate(&error, error.count),
            let rawDevice = VPWgpuMetalRendererMetalDevice(renderer) else {
        let message = error.withUnsafeBufferPointer { buffer in
          buffer.baseAddress.map { String(cString: $0) } ?? "wgpu-metal renderer unavailable"
        }
        NSLog("VoidPlayer WGPU compositor: init failed: \(message)")
        return nil
      }
      wgpuRenderer = renderer
      device = unsafeBitCast(rawDevice, to: MTLDevice.self)
    } else {
      device = MTLCreateSystemDefaultDevice()
    }
    guard let device,
          let commandQueue = device.makeCommandQueue(),
          let pipelines = Self.makePipelines(
            device: device,
            outputPixelFormat: outputPixelFormat
          ) else {
      if let wgpuRenderer {
        VPWgpuMetalRendererDestroy(wgpuRenderer)
      }
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
    self.outputPixelFormat = outputPixelFormat
    self.outputMode = configuration.compositorOutputMode
    self.useWgpuCompositor = useWgpuCompositor
    self.textureCache = cache
    self.wgpuRenderer = wgpuRenderer
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

  deinit {
    if let wgpuRenderer {
      VPWgpuMetalRendererDestroy(wgpuRenderer)
    }
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

  /// Publishes the latest live source buffers (from the source ring). Topology
  /// changes may carry a projection; in that case the buffers and projection are
  /// committed together after the Metal texture imports succeed.
  func setSourceBuffers(
    textures: [MacOSNativeCompositorSourceTexture],
    overlay: MacOSNativeOverlayPrimitives? = nil,
    error: String = "",
    projection: MacOSNativeCompositorSourceProjection? = nil,
    completionTarget: MacOSNativeCompositorSourceReadyCompletionTarget? = nil
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
        completionTarget?.nativeCompositorSourceReadyCompletion(success: false)
        return
      }
      publishSourceReadyState(
        textures: textures,
        token: token,
        generation: sourceCacheGeneration,
        error: error,
        projection: projection,
        completionTarget: completionTarget
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

  func makeSourceProjection(
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
  ) -> MacOSNativeCompositorSourceProjection {
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
    return MacOSNativeCompositorSourceProjection(
      layoutFlags: flags,
      order: orderVec,
      displayOffsetX: dox,
      displayOffsetY: doy,
      invDisplaySizeX: idsx,
      invDisplaySizeY: idsy,
      viewOffsetUvX: voux,
      viewOffsetUvY: vouy,
      trace: trace
    )
  }

  /// Updates the full-layout projection the compositor applies to the source
  /// textures. Called by Dart on every layout change (pan/zoom/split/mode/resize)
  /// with the current layout's per-slot projection params. This is the single
  /// projection path — the source view always reflects the current layout.
  func setSourceProjection(
    _ projection: MacOSNativeCompositorSourceProjection
  ) {
    compositorQueue.async { [weak self] in
      guard let self else { return }
      applySourceProjectionOnCompositorQueue(projection)
      markCompositorDirty()
    }
  }

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
    setSourceProjection(makeSourceProjection(
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
    ))
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
    result["nativeCompositorSpikeEnabled"] = true
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
    result["nativeCompositorWgpuSubmitCpuLastMs"] = wgpuSubmitCpuDuration.lastMs()
    result["nativeCompositorWgpuSubmitCpuP95Ms"] = wgpuSubmitCpuDuration.p95Ms()
    result["nativeCompositorWgpuCompletionLastMs"] = wgpuCompletionDuration.lastMs()
    result["nativeCompositorWgpuCompletionP95Ms"] = wgpuCompletionDuration.p95Ms()
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
    result["nativeCompositorBackend"] = useWgpuCompositor ? "wgpu-metal-thin-runner" : "metal"
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
    result["nativeCompositorSourceProjectionTrackCount"] = Int(sourceLayoutFlags.w)
    result["nativeCompositorSourceCacheActive"] =
      sourceProjectionSet && sourceCacheTextureCount > 0
    result["nativeCompositorSourceCacheTextureCount"] = sourceCacheTextureCount
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
    result["nativeCompositorWgpuLastCompletionResult"] = Int(lastWgpuCompletionResult)
    result["nativeCompositorWgpuDestinationImportCount"] =
      Int(min(lastWgpuProfilerSnapshot.destination_import_count, UInt64(Int.max)))
    result["nativeCompositorWgpuDestinationImportReuseCount"] =
      Int(min(lastWgpuProfilerSnapshot.destination_import_reuse_count, UInt64(Int.max)))
    result["nativeCompositorWgpuSourceImportCount"] =
      Int(min(lastWgpuProfilerSnapshot.source_import_count, UInt64(Int.max)))
    result["nativeCompositorWgpuSourceImportReuseCount"] =
      Int(min(lastWgpuProfilerSnapshot.source_import_reuse_count, UInt64(Int.max)))
    result["nativeCompositorWgpuImportedTextureCacheSize"] =
      Int(min(lastWgpuProfilerSnapshot.imported_texture_cache_size, UInt64(Int.max)))
    result["nativeCompositorWgpuImportedTextureCacheEvictionCount"] =
      Int(min(lastWgpuProfilerSnapshot.imported_texture_cache_eviction_count, UInt64(Int.max)))
    result["nativeCompositorWgpuFinalBindGroupCreateCount"] =
      Int(min(lastWgpuProfilerSnapshot.final_bind_group_create_count, UInt64(Int.max)))
    result["nativeCompositorWgpuOverlayBindGroupCreateCount"] =
      Int(min(lastWgpuProfilerSnapshot.overlay_bind_group_create_count, UInt64(Int.max)))
    result["nativeCompositorWgpuOverlayLayerRebuildCount"] =
      Int(min(lastWgpuProfilerSnapshot.overlay_layer_rebuild_count, UInt64(Int.max)))
    result["nativeCompositorWgpuOverlayLayerReuseCount"] =
      Int(min(lastWgpuProfilerSnapshot.overlay_layer_reuse_count, UInt64(Int.max)))
    result["nativeCompositorWgpuSubmitCount"] =
      Int(min(lastWgpuProfilerSnapshot.submit_count, UInt64(Int.max)))
    result["nativeCompositorWgpuLastImportUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_import_us, UInt64(Int.max)))
    result["nativeCompositorWgpuLastPrepareUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_prepare_us, UInt64(Int.max)))
    result["nativeCompositorWgpuLastOverlayEncodeUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_overlay_encode_us, UInt64(Int.max)))
    result["nativeCompositorWgpuLastBindGroupUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_bind_group_us, UInt64(Int.max)))
    result["nativeCompositorWgpuLastPassEncodeUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_pass_encode_us, UInt64(Int.max)))
    result["nativeCompositorWgpuLastSubmitUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_submit_us, UInt64(Int.max)))
    result["nativeCompositorWgpuLastCpuRenderUs"] =
      Int(min(lastWgpuProfilerSnapshot.last_cpu_render_us, UInt64(Int.max)))
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
      if useWgpuCompositor {
        let wgpuSubmitStartNs = DispatchTime.now().uptimeNanoseconds
        let ok = drawWgpuComposite(
          drawable: drawable,
          video: video,
          flutter: flutter,
          sourceCache: sourceCache,
          holeRect: holeRect,
          layoutFlags: layoutFlags,
          sourceProjectionEnabled: sourceProjectionEnabled,
          colorFlags: colorFlags,
          backgroundColor: backgroundColor,
          frame: frameCount + 1,
          submittedNs: wgpuSubmitStartNs
        )
        wgpuSubmitCpuDuration.record(Self.elapsedNs(from: wgpuSubmitStartNs))
        guard ok else {
          inFlightSemaphore.signal()
          return
        }
        drawable.present()
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

  private func applySourceProjectionOnCompositorQueue(
    _ projection: MacOSNativeCompositorSourceProjection
  ) {
    sourceLayoutFlags = projection.layoutFlags
    sourceOrder = projection.order
    sourceProjDisplayOffsetX = projection.displayOffsetX
    sourceProjDisplayOffsetY = projection.displayOffsetY
    sourceProjInvDisplaySizeX = projection.invDisplaySizeX
    sourceProjInvDisplaySizeY = projection.invDisplaySizeY
    sourceProjViewOffsetUvX = projection.viewOffsetUvX
    sourceProjViewOffsetUvY = projection.viewOffsetUvY
    sourceProjectionSet = true
    sourceProjectionRate.record()
    if let trace = projection.trace {
      recordPendingCompositeTrace(trace)
    }
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
    projection: MacOSNativeCompositorSourceProjection?,
    completionTarget: MacOSNativeCompositorSourceReadyCompletionTarget?
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
          completionTarget?.nativeCompositorSourceReadyCompletion(success: false)
          return
        }
        if let projection {
          applySourceProjectionOnCompositorQueue(projection)
        }
        sourceReadyState = state
        sourceReadyLastError = error
        producerSourcePublishRate.record()
        markCompositorDirty()
        completionTarget?.nativeCompositorSourceReadyCompletion(success: true)
      }
    }
  }

  private func makeVideoReadySnapshot(videoTexture: MacOSVideoTexture) -> VideoTextureSnapshot? {
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
    return SourceCacheReadyState(
      textures: textures,
      pixelBuffers: pixelBuffers,
      cvTextures: cvTextures,
      presentFlags: presentFlags,
      generation: generation,
      bytes: bytes
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
        "NativeCompositorComposite frame=%d backend=%@ mode=%@ video=%dx%d flutter=%dx%d drawable=%dx%d layoutRevision=%llu sourceProjection=%d sourceProjectionEnabled=%d hole=%.4f,%.4f,%.4f,%.4f source0Offset=%.4f,%.4f source0InvSize=%.4f,%.4f bg=%.3f,%.3f,%.3f,%.3f frameCpuLastMs=%.2f videoAcquireLastMs=%.2f flutterAcquireLastMs=%.2f sourceAcquireLastMs=%.2f drawableAcquireLastMs=%.2f wgpuSubmitLastMs=%.2f wgpuCompletionP95Ms=%.2f",
      frame,
      useWgpuCompositor ? "wgpu-metal-thin-runner" : "metal",
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
      drawableAcquireDuration.lastMs(),
      wgpuSubmitCpuDuration.lastMs(),
      wgpuCompletionDuration.p95Ms()
    )
    compositeSummary.withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
  }

  private func finishWgpuComposite(
    result: Int32,
    frame: Int,
    submittedNs: UInt64,
    completedNs: UInt64
  ) {
    inFlightSemaphore.signal()
    lastWgpuCompletionResult = result
    if submittedNs > 0, completedNs >= submittedNs {
      wgpuCompletionDuration.record(completedNs - submittedNs)
    }
    if let wgpuRenderer {
      var snapshot = VPWgpuMetalProfilerSnapshot()
      if VPWgpuMetalRendererGetProfilerSnapshot(wgpuRenderer, &snapshot) == 0 {
        lastWgpuProfilerSnapshot = snapshot
      }
    }
    if result != 0 {
      recordFailure("wgpu-metal runner composite completion failed result=\(result)")
    }
    if MacOSProfilerLog.enabled && (frame == 1 || frame % 120 == 0 || result != 0) {
      let summary = String(
        format:
          "NativeCompositorWgpuCompletion frame=%d result=%d completionLastMs=%.2f completionP95Ms=%.2f rustImportUs=%llu rustPrepareUs=%llu rustBindGroupUs=%llu rustPassEncodeUs=%llu rustSubmitUs=%llu rustCpuRenderUs=%llu destImport=%llu/%llu sourceImport=%llu/%llu cacheSize=%llu cacheEvict=%llu finalBindGroups=%llu overlayBindGroups=%llu overlayLayer=%llu/%llu submitCount=%llu",
        frame,
        result,
        wgpuCompletionDuration.lastMs(),
        wgpuCompletionDuration.p95Ms(),
        lastWgpuProfilerSnapshot.last_import_us,
        lastWgpuProfilerSnapshot.last_prepare_us,
        lastWgpuProfilerSnapshot.last_bind_group_us,
        lastWgpuProfilerSnapshot.last_pass_encode_us,
        lastWgpuProfilerSnapshot.last_submit_us,
        lastWgpuProfilerSnapshot.last_cpu_render_us,
        lastWgpuProfilerSnapshot.destination_import_count,
        lastWgpuProfilerSnapshot.destination_import_reuse_count,
        lastWgpuProfilerSnapshot.source_import_count,
        lastWgpuProfilerSnapshot.source_import_reuse_count,
        lastWgpuProfilerSnapshot.imported_texture_cache_size,
        lastWgpuProfilerSnapshot.imported_texture_cache_eviction_count,
        lastWgpuProfilerSnapshot.final_bind_group_create_count,
        lastWgpuProfilerSnapshot.overlay_bind_group_create_count,
        lastWgpuProfilerSnapshot.overlay_layer_rebuild_count,
        lastWgpuProfilerSnapshot.overlay_layer_reuse_count,
        lastWgpuProfilerSnapshot.submit_count
      )
      summary.withCString { pointer in
        VPMacOSLogProfilerSummary(pointer)
      }
    }
  }

  fileprivate func completeWgpuCompositeFromCallback(
    result: Int32,
    frame: Int,
    submittedNs: UInt64,
    completedNs: UInt64
  ) {
    compositorQueue.async {
      self.finishWgpuComposite(
        result: result,
        frame: frame,
        submittedNs: submittedNs,
        completedNs: completedNs
      )
    }
  }

  private func drawWgpuComposite(
    drawable: CAMetalDrawable,
    video: MTLTexture,
    flutter: MTLTexture,
    sourceCache: SourceCacheTextureSnapshot,
    holeRect: SIMD4<Float>,
    layoutFlags: SIMD4<Float>,
    sourceProjectionEnabled: Bool,
    colorFlags: SIMD4<Float>,
    backgroundColor: SIMD4<Float>,
    frame: Int,
    submittedNs: UInt64
  ) -> Bool {
    guard let wgpuRenderer else {
      recordFailure("wgpu-metal runner renderer is unavailable")
      return false
    }
    guard let videoFormat = Self.wgpuTextureFormat(texture: video),
          let flutterFormat = Self.wgpuTextureFormat(texture: flutter),
          let outputFormat = Self.wgpuOutputFormat(pixelFormat: outputPixelFormat) else {
      recordFailure("wgpu-metal runner texture format is unsupported")
      return false
    }

    let outputWidth = max(1, drawable.texture.width)
    let outputHeight = max(1, drawable.texture.height)
    let viewportRect = SIMD4<Float>(
      max(0, min(Float(outputWidth), holeRect.x * Float(outputWidth))),
      max(0, min(Float(outputHeight), holeRect.y * Float(outputHeight))),
      max(1, (holeRect.z - holeRect.x) * Float(outputWidth)),
      max(1, (holeRect.w - holeRect.y) * Float(outputHeight))
    )
    let sourceProjectionActive = sourceProjectionEnabled
    let overlayActive = sourceProjectionActive && !sourceCache.textures.isEmpty
    var decision = VPMacOSNativePresentDecisionInfo()
    decision.should_present = 1
    decision.frame_count = 1
    decision.track_count = Int32(max(1, min(4, Int(layoutFlags.w.rounded()))))
    decision.mode = Int32(layoutFlags.y.rounded())
    decision.split_pos = layoutFlags.z
    Self.writeFixedFloatArray(&decision.background_color, [
      backgroundColor.x,
      backgroundColor.y,
      backgroundColor.z,
      backgroundColor.w,
    ])
    Self.writeFixedInt32Array(&decision.order, [
      Int32(sourceCache.order.x.rounded()),
      Int32(sourceCache.order.y.rounded()),
      Int32(sourceCache.order.z.rounded()),
      Int32(sourceCache.order.w.rounded()),
    ])
    Self.writeFixedFloatArray(&decision.display_offset_x, [
      sourceCache.displayOffsetX.x,
      sourceCache.displayOffsetX.y,
      sourceCache.displayOffsetX.z,
      sourceCache.displayOffsetX.w,
    ])
    Self.writeFixedFloatArray(&decision.display_offset_y, [
      sourceCache.displayOffsetY.x,
      sourceCache.displayOffsetY.y,
      sourceCache.displayOffsetY.z,
      sourceCache.displayOffsetY.w,
    ])
    Self.writeFixedFloatArray(&decision.inv_display_size_x, [
      sourceCache.invDisplaySizeX.x,
      sourceCache.invDisplaySizeX.y,
      sourceCache.invDisplaySizeX.z,
      sourceCache.invDisplaySizeX.w,
    ])
    Self.writeFixedFloatArray(&decision.inv_display_size_y, [
      sourceCache.invDisplaySizeY.x,
      sourceCache.invDisplaySizeY.y,
      sourceCache.invDisplaySizeY.z,
      sourceCache.invDisplaySizeY.w,
    ])
    Self.writeFixedFloatArray(&decision.view_offset_uv_x, [
      sourceCache.viewOffsetUvX.x,
      sourceCache.viewOffsetUvX.y,
      sourceCache.viewOffsetUvX.z,
      sourceCache.viewOffsetUvX.w,
    ])
    Self.writeFixedFloatArray(&decision.view_offset_uv_y, [
      sourceCache.viewOffsetUvY.x,
      sourceCache.viewOffsetUvY.y,
      sourceCache.viewOffsetUvY.z,
      sourceCache.viewOffsetUvY.w,
    ])

    var sourceWidths = [Int32](repeating: Int32(video.width), count: 4)
    var sourceHeights = [Int32](repeating: Int32(video.height), count: 4)
    var present = [Int32](repeating: 0, count: 4)
    var sourceTextures = [VPWgpuMetalRunnerTexture](
      repeating: VPWgpuMetalRunnerTexture(),
      count: 4
    )
    for slot in 0..<4 {
      if sourceProjectionActive, let texture = sourceCache.textures[slot],
         let format = Self.wgpuTextureFormat(texture: texture) {
        present[slot] = 1
        sourceWidths[slot] = Int32(texture.width)
        sourceHeights[slot] = Int32(texture.height)
        sourceTextures[slot] = Self.runnerTexture(texture: texture, format: format, present: true)
      }
    }
    if !sourceProjectionActive {
      present[0] = 1
    }
    Self.writeFixedInt32Array(&decision.source_width, sourceWidths)
    Self.writeFixedInt32Array(&decision.source_height, sourceHeights)
    Self.writeFixedInt32Array(&decision.color_range, [2, 2, 2, 2])
    Self.writeFixedInt32Array(&decision.color_matrix, [2, 2, 2, 2])
    Self.writeFixedInt32Array(&decision.color_transfer, [1, 1, 1, 1])
    Self.writeFixedInt32Array(&decision.color_primaries, [2, 2, 2, 2])
    Self.writeFrameArray(&decision.frames) { frames in
      for slot in 0..<4 {
        var frame = VPMacOSNativePresentFrameInfo()
        frame.present = present[slot]
        frame.slot = Int32(slot)
        frame.width = sourceWidths[slot]
        frame.height = sourceHeights[slot]
        frame.color_range = 2
        frame.color_matrix = 2
        frame.color_transfer = 1
        frame.color_primaries = 2
        frames[slot] = frame
      }
    }

    let fillRects = overlayActive ? overlayPrimitives.fillRects : []
    let lineRects = overlayActive ? overlayPrimitives.lineRects : []
    let motionLines = overlayActive ? overlayPrimitives.motionLines : []
    var error = [CChar](repeating: 0, count: 512)
    let context = MacOSNativeCompositorWgpuCallbackContext(
      view: self,
      frame: frame,
      submittedNs: submittedNs
    )
    let retainedContext = Unmanaged.passRetained(context)
    let ret = fillRects.withUnsafeBufferPointer { fillBuffer in
      lineRects.withUnsafeBufferPointer { lineBuffer in
        motionLines.withUnsafeBufferPointer { motionBuffer in
          withUnsafePointer(to: &decision) { decisionPointer in
            error.withUnsafeMutableBufferPointer { errorBuffer in
              var request = VPWgpuMetalRunnerCompositeRequest()
              request.destination_mtl_texture = Self.rawPointer(drawable.texture)
              request.output_format = outputFormat
              request.output_color_mode = outputPixelFormat == .rgba16Float
                ? Int32(VP_WGPU_METAL_OUTPUT_COLOR_MODE_EDR)
                : Int32(VP_WGPU_METAL_OUTPUT_COLOR_MODE_SDR)
              request.video = Self.runnerTexture(
                texture: video,
                format: videoFormat,
                present: true
              )
              request.flutter = Self.runnerTexture(
                texture: flutter,
                format: flutterFormat,
                present: true
              )
              Self.writeRunnerTextureArray(&request.sources, sourceTextures)
              request.decision = decisionPointer
              Self.writeFixedFloatArray(&request.viewport_rect, [
                viewportRect.x,
                viewportRect.y,
                viewportRect.z,
                viewportRect.w,
              ])
              request.source_cache_active = sourceProjectionActive ? 1 : 0
              request.video_srgb_to_linear = colorFlags.y > 0.5 ? 1 : 0
              request.flutter_srgb_to_linear = colorFlags.z > 0.5 ? 1 : 0
              request.source_srgb_to_linear =
                (outputPixelFormat == .rgba16Float &&
                  sourceCache.textures.values.contains { $0.pixelFormat == .bgra8Unorm })
                ? 1
                : 0
              request.overlay_fill_rects = fillBuffer.baseAddress
              request.overlay_fill_rect_count = fillBuffer.count
              request.overlay_line_rects = lineBuffer.baseAddress
              request.overlay_line_rect_count = lineBuffer.count
              request.overlay_motion_lines = motionBuffer.baseAddress
              request.overlay_motion_line_count = motionBuffer.count
              request.overlay_generation = overlayPrimitives.generation
              request.width = Int32(outputWidth)
              request.height = Int32(outputHeight)
              request.error = errorBuffer.baseAddress
              request.error_size = errorBuffer.count
              var completion = VPWgpuMetalAsyncCompletion()
              completion.callback = macOSNativeCompositorWgpuCompletion
              completion.user_data = retainedContext.toOpaque()
              completion.profiler_snapshot = nil
              return VPWgpuMetalRendererCompositeRunnerAsync(wgpuRenderer, &request, completion)
            }
          }
        }
      }
    }
    guard ret == 0 else {
      retainedContext.release()
      let message = error.withUnsafeBufferPointer { buffer in
        buffer.baseAddress.map { String(cString: $0) } ?? "wgpu-metal runner composite failed"
      }
      recordFailure(message.isEmpty ? "wgpu-metal runner composite failed" : message)
      return false
    }
    return true
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

  private static func wgpuOutputFormat(pixelFormat: MTLPixelFormat) -> Int32? {
    switch pixelFormat {
    case .bgra8Unorm:
      return Int32(VP_WGPU_METAL_OUTPUT_FORMAT_BGRA8_UNORM)
    case .rgba16Float:
      return Int32(VP_WGPU_METAL_OUTPUT_FORMAT_RGBA16_FLOAT)
    default:
      return nil
    }
  }

  private static func wgpuTextureFormat(texture: MTLTexture) -> Int32? {
    switch texture.pixelFormat {
    case .bgra8Unorm:
      return Int32(VP_WGPU_METAL_TEXTURE_FORMAT_BGRA8_UNORM)
    case .rgba16Float:
      return Int32(VP_WGPU_METAL_TEXTURE_FORMAT_RGBA16_FLOAT)
    default:
      return nil
    }
  }

  private static func runnerTexture(
    texture: MTLTexture,
    format: Int32,
    present: Bool
  ) -> VPWgpuMetalRunnerTexture {
    var result = VPWgpuMetalRunnerTexture()
    result.mtl_texture = rawPointer(texture)
    result.width = Int32(texture.width)
    result.height = Int32(texture.height)
    result.format = format
    result.present = present ? 1 : 0
    return result
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

  private static func writeRunnerTextureArray<T>(
    _ storage: inout T,
    _ textures: [VPWgpuMetalRunnerTexture]
  ) {
    withUnsafeMutableBytes(of: &storage) { raw in
      let buffer = raw.bindMemory(to: VPWgpuMetalRunnerTexture.self)
      for i in 0..<min(buffer.count, textures.count) {
        buffer[i] = textures[i]
      }
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
  }

  private struct OverlayVertex {
    var position: SIMD2<Float>
    var color: SIMD4<Float>
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
        "VoidPlayer native compositor failure backend=\(useWgpuCompositor ? "wgpu-metal-thin-runner" : "metal") frame=\(frameCount): \(message)"
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
        texture2d<float> source,
        float2 uv,
        sampler linearSampler,
        sampler nearestSampler
      ) {
        float2 sourceSize = float2(source.get_width(), source.get_height());
        float2 dx = dfdx(uv) * sourceSize;
        float2 dy = dfdy(uv) * sourceSize;
        float footprint = max(
          max(abs(dx.x), abs(dx.y)),
          max(abs(dy.x), abs(dy.y)));
        if (footprint > 1.0001) {
          return source.sample(linearSampler, uv);
        }
        return source.sample(nearestSampler, uv);
      }

      float4 sampleSourceCacheTexture(
        int slot,
        float2 uv,
        texture2d<float> source0,
        texture2d<float> source1,
        texture2d<float> source2,
        texture2d<float> source3,
        sampler linearSampler,
        sampler nearestSampler
      ) {
        if (slot == 0) return sampleSourceCacheTexture(source0, uv, linearSampler, nearestSampler);
        if (slot == 1) return sampleSourceCacheTexture(source1, uv, linearSampler, nearestSampler);
        if (slot == 2) return sampleSourceCacheTexture(source2, uv, linearSampler, nearestSampler);
        return sampleSourceCacheTexture(source3, uv, linearSampler, nearestSampler);
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
        sampler linearSampler,
        sampler nearestSampler
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
          linearSampler,
          nearestSampler);
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
        constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);
        constexpr sampler nearestSampler(address::clamp_to_edge, filter::nearest);
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
              linearSampler,
              nearestSampler);
          } else if (compositorFlags.x > 0.5) {
            video = outputBackground;
          } else {
            video = videoTexture.sample(linearSampler, videoUv);
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
