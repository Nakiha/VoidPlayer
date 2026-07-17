import Cocoa
import CoreVideo
import FlutterMacOS
import IOSurface
import Metal
import QuartzCore

/// Runner-owned final compositor. The native renderer owns one complete video
/// target (layout, SDR/HDR conversion, and overlays); Flutter owns its complete
/// premultiplied-alpha surface. This view only places and blends those targets.
final class MacOSNativeCompositorView: NSView {
  private let commandQueue: MTLCommandQueue
  private let videoPipeline: MTLRenderPipelineState
  private let flutterPipeline: MTLRenderPipelineState
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
    label: "dev.nakiha.voidplayer.macos.native-compositor.video-ready",
    qos: .userInteractive
  )
  private let compositorQueueKey = DispatchSpecificKey<Bool>()
  private let inFlightSemaphore = DispatchSemaphore(value: 2)
  private var displayLink: MacOSViewportDisplayLink?
  private var videoReadySnapshot: VideoTextureSnapshot?
  private var videoReadyPublishInFlight = false
  private var videoReadyPublishPending = false
  private var videoReadyToken: UInt64 = 0
  private var videoReadyLastError = ""
  private var explicitViewportRect: SIMD4<Float>?
  private var lastViewportRect = SIMD4<Float>(0, 0, 0, 0)
  private var viewportBackgroundColor = SIMD4<Float>(0, 0, 0, 1)
  private var pendingCompositeTrace: MacOSCompositorLatencyTrace?

  private var frameCount = 0
  private var lastVideoTextureAvailable = false
  private var lastFlutterSurfaceAvailable = false
  private var lastCompositeSucceeded = false
  private var lastFailure = "not drawn"
  private var lastLoggedFailure = ""
  private var displayedLayoutRevision: UInt64 = 0
  private var lastPresentedVideoSourceKey: UInt64 = 0
  private var lastPresentedFlutterSourceKey: UInt64 = 0
  private var displayTickReuseVideoCount = 0
  private var displayTickBlockedProducerCount = 0
  private var skippedInFlightFrames = 0
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
  private var lastDisplayTickNs: UInt64 = 0

  private let compositeRate = MacOSRateWindow()
  private let displayTickRate = MacOSRateWindow()
  private let inFlightSkipRate = MacOSRateWindow()
  private let surfaceChangeRate = MacOSRateWindow()
  private let videoSourceChangeRate = MacOSRateWindow()
  private let flutterSourceChangeRate = MacOSRateWindow()
  private let producerVideoPublishRate = MacOSRateWindow()
  private let displayTickIntervalDuration = MacOSDurationWindow()
  private let frameCpuDuration = MacOSDurationWindow()
  private let videoAcquireDuration = MacOSDurationWindow()
  private let readyVideoAcquireDuration = MacOSDurationWindow()
  private let flutterAcquireDuration = MacOSDurationWindow()
  private let inFlightWaitDuration = MacOSDurationWindow()
  private let drawableAcquireDuration = MacOSDurationWindow()

  private static let metricsSampleIntervalNs: UInt64 = 1_000_000_000

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
    guard let device = MTLCreateSystemDefaultDevice(),
          let commandQueue = device.makeCommandQueue(),
          let pipelines = MacOSNativeCompositorPipelineFactory.make(
            device: device,
            outputPixelFormat: outputPixelFormat
          ) else {
      return nil
    }
    var cache: CVMetalTextureCache?
    guard CVMetalTextureCacheCreate(
      kCFAllocatorDefault,
      nil,
      device,
      nil,
      &cache
    ) == kCVReturnSuccess, let cache else {
      return nil
    }

    self.commandQueue = commandQueue
    self.videoPipeline = pipelines.video
    self.flutterPipeline = pipelines.flutter
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
    nil
  }

  override func hitTest(_ point: NSPoint) -> NSView? {
    nil
  }

  override func layout() {
    super.layout()
    let scale = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2.0
    metalLayer.contentsScale = scale
    metalLayer.drawableSize = CGSize(
      width: max(1.0, bounds.width * scale),
      height: max(1.0, bounds.height * scale)
    )
  }

  func attach(to parent: NSView) {
    frame = parent.bounds
    parent.addSubview(self, positioned: .above, relativeTo: nil)
    displayLink = MacOSViewportDisplayLink(deliveryQueue: compositorQueue) { [weak self] in
      self?.drawComposite()
    }
    displayLink?.start()
    NSLog("VoidPlayer native compositor installed: native target + Flutter surface")
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
      } else {
        requestVideoReadyPublishOnCompositorQueue(reason: "set-video-texture")
      }
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
    let rect: SIMD4<Float>? = {
      guard width > 0, height > 0, surfaceWidth > 0, surfaceHeight > 0 else {
        return nil
      }
      return SIMD4<Float>(
        Float(max(0, left)) / Float(surfaceWidth),
        Float(max(0, top)) / Float(surfaceHeight),
        Float(min(surfaceWidth, left + width)) / Float(surfaceWidth),
        Float(min(surfaceHeight, top + height)) / Float(surfaceHeight)
      )
    }()
    compositorQueue.async { [weak self] in
      guard let self else { return }
      explicitViewportRect = rect
      if let rect {
        lastViewportRect = rect
      }
      if let trace {
        recordPendingCompositeTrace(trace)
      }
    }
  }

  func setViewportBackgroundColor(_ color: UInt32) {
    let next = SIMD4<Float>(
      Float((color >> 16) & 0xFF) / 255.0,
      Float((color >> 8) & 0xFF) / 255.0,
      Float(color & 0xFF) / 255.0,
      Float((color >> 24) & 0xFF) / 255.0
    )
    compositorQueue.async { [weak self] in
      self?.viewportBackgroundColor = next
    }
  }

  func diagnostics() -> [String: Any] {
    if DispatchQueue.getSpecific(key: compositorQueueKey) == true {
      return diagnosticsOnCompositorQueue()
    }
    return compositorQueue.sync { diagnosticsOnCompositorQueue() }
  }

  private func diagnosticsOnCompositorQueue() -> [String: Any] {
    var result = configuration.diagnostics
    let compositeHz = compositeRate.rateHz()
    result["nativeCompositorEnabled"] = true
    result["nativeCompositorBackend"] = "metal"
    result["nativeCompositorOutputMode"] = outputMode
    result["nativeCompositorOutputPixelFormat"] = String(describing: outputPixelFormat)
    result["nativeCompositorEDREnabled"] = outputPixelFormat == .rgba16Float
    result["nativeCompositorEDRWantsExtendedDynamicRangeContent"] =
      metalLayer.wantsExtendedDynamicRangeContent
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
    result["nativeCompositorInFlightWaitLastMs"] = inFlightWaitDuration.lastMs()
    result["nativeCompositorInFlightWaitP95Ms"] = inFlightWaitDuration.p95Ms()
    result["nativeCompositorDrawableAcquireLastMs"] = drawableAcquireDuration.lastMs()
    result["nativeCompositorDrawableAcquireP95Ms"] = drawableAcquireDuration.p95Ms()
    result["nativeCompositorInFlightSkipHz"] = inFlightSkipRate.rateHz()
    result["nativeCompositorStaticSkipHz"] = 0.0
    result["nativeCompositorSurfaceChangeHz"] = surfaceChangeRate.rateHz()
    result["nativeCompositorVideoSourceChangeHz"] = videoSourceChangeRate.rateHz()
    result["nativeCompositorFlutterSourceChangeHz"] = flutterSourceChangeRate.rateHz()
    result["nativeCompositorVideoTextureAvailable"] = lastVideoTextureAvailable
    result["nativeCompositorFlutterSurfaceAvailable"] = lastFlutterSurfaceAvailable
    result["nativeCompositorLastCompositeSucceeded"] = lastCompositeSucceeded
    result["nativeCompositorLastFailure"] = lastFailure
    result["nativeCompositorFlutterAlphaAverageX1000"] = lastFlutterAlphaAverageX1000
    result["nativeCompositorFlutterTransparentRatioX1000"] =
      lastFlutterTransparentRatioX1000
    result["nativeCompositorHoleLeftX1000"] = Int(lastViewportRect.x * 1000.0)
    result["nativeCompositorHoleTopX1000"] = Int(lastViewportRect.y * 1000.0)
    result["nativeCompositorHoleRightX1000"] = Int(lastViewportRect.z * 1000.0)
    result["nativeCompositorHoleBottomX1000"] = Int(lastViewportRect.w * 1000.0)
    result["nativeCompositorDrawableWidth"] = Int(metalLayer.drawableSize.width)
    result["nativeCompositorDrawableHeight"] = Int(metalLayer.drawableSize.height)
    result["nativeCompositorVideoPixelFormat"] = lastVideoPixelFormat
    result["nativeCompositorEDRVideoSampleCount"] = lastEDRVideoSampleCount
    result["nativeCompositorEDRVideoMaxRGBX1000"] = lastEDRVideoMaxRGBX1000
    result["nativeCompositorEDRVideoPixelsOver1X1000"] = lastEDRVideoPixelsOver1X1000
    result["nativeCompositorVideoSRGBToLinearEnabled"] = lastVideoSRGBToLinearEnabled
    result["nativeCompositorFlutterSRGBToLinearEnabled"] =
      lastFlutterSRGBToLinearEnabled
    result["nativeCompositorSkippedInFlightFrames"] = skippedInFlightFrames
    result["nativeCompositorSkippedStaticFrames"] = 0
    result["producerVideoPublishHz"] = producerVideoPublishRate.rateHz()
    result["producerVideoPublishHzX1000"] = Int(producerVideoPublishRate.rateHz() * 1000.0)
    result["displayTickReuseVideoCount"] = displayTickReuseVideoCount
    result["displayTickBlockedProducerCount"] = displayTickBlockedProducerCount
    result["nativeCompositorVideoReadyLastError"] = videoReadyLastError
    result["nativeCompositorDisplayedLayoutRevision"] = Int(
      min(displayedLayoutRevision, UInt64(Int.max))
    )
    result.merge(latencyProfiler.diagnosticMap()) { _, next in next }
    return result
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
      guard let flutterSnapshot = currentFlutterSurface() else {
        flutterAcquireDuration.record(Self.elapsedNs(from: flutterAcquireStartNs))
        setHiddenOnMain(true)
        recordFailure("no Flutter surface")
        return
      }
      flutterAcquireDuration.record(Self.elapsedNs(from: flutterAcquireStartNs))

      let nowNs = DispatchTime.now().uptimeNanoseconds
      let videoChanged = videoSnapshot.sourceKey != lastPresentedVideoSourceKey
      let flutterChanged = flutterSnapshot.sourceKey != lastPresentedFlutterSourceKey
      if videoChanged || flutterChanged {
        surfaceChangeRate.record(nowNs: nowNs)
      }
      if videoChanged { videoSourceChangeRate.record(nowNs: nowNs) }
      if flutterChanged { flutterSourceChangeRate.record(nowNs: nowNs) }
      if frameCount > 0 && !videoChanged { displayTickReuseVideoCount += 1 }

      let inFlightWaitStartNs = DispatchTime.now().uptimeNanoseconds
      guard inFlightSemaphore.wait(timeout: .now()) == .success else {
        inFlightWaitDuration.record(Self.elapsedNs(from: inFlightWaitStartNs))
        skippedInFlightFrames += 1
        inFlightSkipRate.record(nowNs: nowNs)
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

      var viewportRect = explicitViewportRect ?? lastViewportRect
      var colorFlags = SIMD4<Float>(
        outputPixelFormat == .rgba16Float ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: videoSnapshot.texture) ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: flutterSnapshot.texture) ? 1.0 : 0.0,
        0.0
      )
      var backgroundColor = viewportBackgroundColor
      guard let commandBuffer = commandQueue.makeCommandBuffer(),
            let encoder = commandBuffer.makeRenderCommandEncoder(
              descriptor: renderPassDescriptor(
                drawable: drawable,
                backgroundColor: backgroundColor
              )
            ) else {
        inFlightSemaphore.signal()
        recordFailure("failed to create command encoder")
        return
      }

      encoder.setRenderPipelineState(videoPipeline)
      encoder.setFragmentTexture(videoSnapshot.texture, index: 0)
      encoder.setFragmentBytes(&viewportRect, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
      encoder.setFragmentBytes(&colorFlags, length: MemoryLayout<SIMD4<Float>>.stride, index: 1)
      encoder.setFragmentBytes(&backgroundColor, length: MemoryLayout<SIMD4<Float>>.stride, index: 2)
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)

      encoder.setRenderPipelineState(flutterPipeline)
      encoder.setFragmentTexture(flutterSnapshot.texture, index: 0)
      encoder.setFragmentBytes(&colorFlags, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
      encoder.endEncoding()

      let semaphore = inFlightSemaphore
      let firstFrameLatency = firstFrameLatency
      commandBuffer.addCompletedHandler { _ in
        semaphore.signal()
        if let summary = firstFrameLatency.markCompositeCompleted() {
          NSLog("VoidPlayer native add first-frame latency: \(summary)")
        }
      }
      commandBuffer.present(drawable)
      commandBuffer.commit()

      setHiddenOnMain(false)
      frameCount += 1
      compositeRate.record(nowNs: nowNs)
      if let trace = pendingCompositeTrace {
        pendingCompositeTrace = nil
        latencyProfiler.recordComposited(trace, compositeNs: nowNs)
      }
      displayedLayoutRevision = max(displayedLayoutRevision, videoSnapshot.layoutRevision)
      lastVideoTextureAvailable = true
      lastFlutterSurfaceAvailable = true
      lastCompositeSucceeded = true
      lastFailure = ""
      lastVideoSRGBToLinearEnabled = colorFlags.y > 0.5
      lastFlutterSRGBToLinearEnabled = colorFlags.z > 0.5
      lastPresentedVideoSourceKey = videoSnapshot.sourceKey
      lastPresentedFlutterSourceKey = flutterSnapshot.sourceKey
      frameCpuDuration.record(Self.elapsedNs(from: tickStartNs))
      logCompositeFrameSummary(
        video: videoSnapshot.texture,
        flutter: flutterSnapshot.texture,
        viewportRect: viewportRect
      )
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
      displayTickBlockedProducerCount += 1
      return
    }
    videoReadyPublishInFlight = true
    videoReadyPublishPending = false
    let token = videoReadyToken
    readyStateQueue.async { [weak self] in
      guard let self else { return }
      let startNs = DispatchTime.now().uptimeNanoseconds
      let snapshot = makeVideoReadySnapshot(videoTexture: videoTexture)
      let elapsedNs = Self.elapsedNs(from: startNs)
      compositorQueue.async { [weak self] in
        guard let self else { return }
        let retry = videoReadyPublishPending || token != videoReadyToken
        videoReadyPublishInFlight = false
        videoReadyPublishPending = false
        guard token == videoReadyToken else {
          if retry { requestVideoReadyPublishOnCompositorQueue(reason: "token-advanced") }
          return
        }
        readyVideoAcquireDuration.record(elapsedNs)
        guard let snapshot else {
          videoReadyLastError = "video ready snapshot unavailable"
          lastVideoTextureAvailable = false
          if retry { requestVideoReadyPublishOnCompositorQueue(reason: "retry") }
          return
        }
        videoReadySnapshot = snapshot
        videoReadyLastError = ""
        lastVideoTextureAvailable = true
        maybeUpdateVideoEDRMetrics(pixelBuffer: snapshot.pixelBuffer)
        producerVideoPublishRate.record()
        firstFrameLatency.markTargetReady()
        if retry { requestVideoReadyPublishOnCompositorQueue(reason: "pending") }
      }
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
    let format: MTLPixelFormat =
      CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_64RGBAHalf
      ? .rgba16Float
      : .bgra8Unorm
    var cvTexture: CVMetalTexture?
    let status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      textureCache,
      pixelBuffer,
      nil,
      format,
      CVPixelBufferGetWidth(pixelBuffer),
      CVPixelBufferGetHeight(pixelBuffer),
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

  private func currentFlutterSurface() -> FlutterSurfaceSnapshot? {
    guard let info = engine?.voidPlayerHDRCurrentFlutterSurfaceInfos().first,
          let texture = info["texture"] as? MTLTexture else {
      lastFlutterSurfaceAvailable = false
      return nil
    }
    maybeUpdateFlutterAlphaMetrics(info: info)
    return FlutterSurfaceSnapshot(
      texture: texture,
      sourceKey: flutterSurfaceSourceKey(info: info, texture: texture)
    )
  }

  private func flutterSurfaceSourceKey(info: [String: Any], texture: MTLTexture) -> UInt64 {
    if let value = info["ioSurfaceId"] as? UInt64 { return value }
    if let value = info["ioSurfaceId"] as? Int { return UInt64(max(0, value)) }
    if let value = info["texturePointer"] as? UInt64 { return value }
    if let value = info["texturePointer"] as? Int { return UInt64(max(0, value)) }
    return UInt64(UInt(bitPattern: Unmanaged.passUnretained(texture as AnyObject).toOpaque()))
  }

  private func maybeUpdateFlutterAlphaMetrics(info: [String: Any]) {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    if lastFlutterAlphaMetricsSampleNs > 0,
       nowNs - lastFlutterAlphaMetricsSampleNs < Self.metricsSampleIntervalNs {
      return
    }
    lastFlutterAlphaMetricsSampleNs = nowNs
    guard let rawSurface = info["ioSurface"] else {
      lastFlutterAlphaAverageX1000 = -1
      lastFlutterTransparentRatioX1000 = -1
      return
    }
    let object = rawSurface as AnyObject
    guard CFGetTypeID(object) == IOSurfaceGetTypeID() else { return }
    let surface = unsafeBitCast(object, to: IOSurfaceRef.self)
    guard IOSurfaceGetPixelFormat(surface) == kCVPixelFormatType_32BGRA,
          IOSurfaceLock(surface, .readOnly, nil) == kIOReturnSuccess else {
      return
    }
    defer { IOSurfaceUnlock(surface, .readOnly, nil) }
    let base = IOSurfaceGetBaseAddress(surface)
    guard Int(bitPattern: base) != 0 else { return }
    let width = IOSurfaceGetWidth(surface)
    let height = IOSurfaceGetHeight(surface)
    let bytesPerRow = IOSurfaceGetBytesPerRow(surface)
    let pixels = base.assumingMemoryBound(to: UInt8.self)
    let columns = min(96, max(1, width))
    let rows = min(96, max(1, height))
    var alphaSum = 0
    var transparent = 0
    var count = 0
    for row in 0..<rows {
      let y = rows == 1 ? 0 : row * (height - 1) / (rows - 1)
      for column in 0..<columns {
        let x = columns == 1 ? 0 : column * (width - 1) / (columns - 1)
        let alpha = Int(pixels[y * bytesPerRow + x * 4 + 3])
        alphaSum += alpha
        if alpha < 8 { transparent += 1 }
        count += 1
      }
    }
    if count > 0 {
      lastFlutterAlphaAverageX1000 = alphaSum * 1000 / (count * 255)
      lastFlutterTransparentRatioX1000 = transparent * 1000 / count
    }
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
    if lastVideoMetricsSampleNs > 0,
       nowNs - lastVideoMetricsSampleNs < Self.metricsSampleIntervalNs {
      return
    }
    lastVideoMetricsSampleNs = nowNs
    guard CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly) == kCVReturnSuccess else {
      return
    }
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
    guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
    let width = CVPixelBufferGetWidth(pixelBuffer)
    let height = CVPixelBufferGetHeight(pixelBuffer)
    let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let words = base.assumingMemoryBound(to: UInt16.self)
    let columns = min(96, max(1, width))
    let rows = min(96, max(1, height))
    var count = 0
    var overOne = 0
    var maximum: Float = 0
    for row in 0..<rows {
      let y = rows == 1 ? 0 : row * (height - 1) / (rows - 1)
      let rowOffset = y * bytesPerRow / MemoryLayout<UInt16>.stride
      for column in 0..<columns {
        let x = columns == 1 ? 0 : column * (width - 1) / (columns - 1)
        let offset = rowOffset + x * 4
        let value = max(
          Self.floatFromHalf(words[offset]),
          max(Self.floatFromHalf(words[offset + 1]), Self.floatFromHalf(words[offset + 2]))
        )
        maximum = max(maximum, value)
        if value > 1.0 { overOne += 1 }
        count += 1
      }
    }
    lastEDRVideoSampleCount = count
    lastEDRVideoMaxRGBX1000 = Int((maximum * 1000.0).rounded())
    lastEDRVideoPixelsOver1X1000 = count > 0 ? overOne * 1000 / count : 0
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

  private func recordPendingCompositeTrace(_ trace: MacOSCompositorLatencyTrace) {
    if pendingCompositeTrace != nil {
      latencyProfiler.recordCoalescedBeforeComposite()
    }
    latencyProfiler.recordApplied(trace, applyNs: DispatchTime.now().uptimeNanoseconds)
    pendingCompositeTrace = trace
  }

  private func recordFailure(_ message: String) {
    lastCompositeSucceeded = false
    lastFailure = message
    if message != lastLoggedFailure || frameCount == 0 || frameCount % 120 == 0 {
      lastLoggedFailure = message
      NSLog("VoidPlayer native compositor failure frame=\(frameCount): \(message)")
    }
  }

  private func setHiddenOnMain(_ hidden: Bool) {
    DispatchQueue.main.async { [weak self] in self?.isHidden = hidden }
  }

  private func shouldConvertSRGBToLinear(texture: MTLTexture) -> Bool {
    guard outputPixelFormat == .rgba16Float else { return false }
    return texture.pixelFormat == .bgra8Unorm || texture.pixelFormat == .rgba8Unorm
  }

  private func logCompositeFrameSummary(
    video: MTLTexture,
    flutter: MTLTexture,
    viewportRect: SIMD4<Float>
  ) {
    guard MacOSProfilerLog.enabled && (frameCount == 1 || frameCount % 120 == 0) else {
      return
    }
    String(
      format: "NativeCompositorComposite frame=%d backend=metal mode=%@ video=%dx%d flutter=%dx%d drawable=%dx%d layoutRevision=%llu viewport=%.4f,%.4f,%.4f,%.4f frameCpuLastMs=%.2f",
      frameCount,
      outputMode,
      video.width,
      video.height,
      flutter.width,
      flutter.height,
      Int(metalLayer.drawableSize.width),
      Int(metalLayer.drawableSize.height),
      displayedLayoutRevision,
      viewportRect.x,
      viewportRect.y,
      viewportRect.z,
      viewportRect.w,
      frameCpuDuration.lastMs()
    ).withCString { VPMacOSLogProfilerSummary($0) }
  }

  private static func elapsedNs(from startNs: UInt64) -> UInt64 {
    let nowNs = DispatchTime.now().uptimeNanoseconds
    return nowNs >= startNs ? nowNs - startNs : 0
  }

  private static func pixelFormatName(_ format: OSType) -> String {
    switch format {
    case kCVPixelFormatType_32BGRA: return "32BGRA"
    case kCVPixelFormatType_64RGBAHalf: return "64RGBAHalf"
    default: return String(format)
    }
  }

  private static func floatFromHalf(_ value: UInt16) -> Float {
    Float(Float16(bitPattern: value))
  }

  private struct VideoTextureSnapshot {
    let texture: MTLTexture
    let pixelBuffer: CVPixelBuffer
    let cvTexture: CVMetalTexture
    let sourceKey: UInt64
    let layoutRevision: UInt64
  }

  private struct FlutterSurfaceSnapshot {
    let texture: MTLTexture
    let sourceKey: UInt64
  }
}
