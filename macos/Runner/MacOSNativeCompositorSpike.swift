import Cocoa
import CoreVideo
import FlutterMacOS
import IOSurface
import Metal
import QuartzCore

final class MacOSNativeCompositorSpikeView: NSView {
  private let device: MTLDevice
  private let commandQueue: MTLCommandQueue
  private let pipeline: MTLRenderPipelineState
  private let outputPixelFormat: MTLPixelFormat
  private let outputMode: String
  private let textureCache: CVMetalTextureCache
  private weak var engine: FlutterEngine?
  private weak var videoTexture: MacOSVideoTexture?
  private let metalLayer = CAMetalLayer()
  private var displayLink: MacOSViewportDisplayLink?
  private var frameCount = 0
  private var lastVideoTextureAvailable = false
  private var lastFlutterTextureAvailable = false
  private var lastCompositeSucceeded = false
  private var lastFailure = "not drawn"
  private var lastFlutterAlphaAverageX1000 = -1
  private var lastFlutterTransparentRatioX1000 = -1
  private var explicitHoleRect: SIMD4<Float>?
  private var lastHoleRect = SIMD4<Float>(0, 0, 0, 0)

  static var isEnabled: Bool {
    ProcessInfo.processInfo.environment["VOIDPLAYER_NATIVE_COMPOSITOR_SPIKE"] == "1"
  }

  private static var useEDROutput: Bool {
    let environment = ProcessInfo.processInfo.environment
    return environment["VOIDPLAYER_NATIVE_COMPOSITOR_EDR"] == "1" ||
      environment["VOIDPLAYER_FLUTTER_HDR_SPIKE"] == "1"
  }

  init?(engine: FlutterEngine) {
    let outputPixelFormat: MTLPixelFormat = Self.useEDROutput
      ? .rgba16Float
      : .bgra8Unorm
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
    self.outputPixelFormat = outputPixelFormat
    self.outputMode = outputPixelFormat == .rgba16Float ? "edr-rgba16float" : "sdr-bgra8unorm"
    self.textureCache = cache
    self.engine = engine
    super.init(frame: .zero)

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
  }

  func attach(to parent: NSView) {
    frame = parent.bounds
    parent.addSubview(self, positioned: .above, relativeTo: nil)
    displayLink = MacOSViewportDisplayLink { [weak self] in
      self?.drawComposite()
    }
    displayLink?.start()
    NSLog("VoidPlayer native compositor spike: installed")
  }

  func setVideoTexture(_ texture: MacOSVideoTexture?) {
    videoTexture = texture
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
      explicitHoleRect = nil
      return
    }
    let minX = Float(max(0, left)) / Float(surfaceWidth)
    let minY = Float(max(0, top)) / Float(surfaceHeight)
    let maxX = Float(min(surfaceWidth, left + width)) / Float(surfaceWidth)
    let maxY = Float(min(surfaceHeight, top + height)) / Float(surfaceHeight)
    explicitHoleRect = SIMD4<Float>(minX, minY, maxX, maxY)
    lastHoleRect = explicitHoleRect!
  }

  func diagnostics() -> [String: Any] {
    return [
      "nativeCompositorSpikeEnabled": true,
      "nativeCompositorFrames": frameCount,
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
    ]
  }

  private func drawComposite() {
    autoreleasepool {
      guard let drawable = metalLayer.nextDrawable() else {
        recordFailure("no drawable")
        return
      }
      guard let video = currentVideoMetalTexture() else {
        recordFailure("no video texture")
        return
      }
      guard let flutter = currentFlutterMetalTexture() else {
        recordFailure("no Flutter texture")
        return
      }
      guard let commandBuffer = commandQueue.makeCommandBuffer(),
            let encoder = commandBuffer.makeRenderCommandEncoder(
              descriptor: renderPassDescriptor(drawable: drawable)
            ) else {
        recordFailure("failed to create command encoder")
        return
      }

      encoder.setRenderPipelineState(pipeline)
      encoder.setFragmentTexture(video, index: 0)
      encoder.setFragmentTexture(flutter, index: 1)
      var holeRect = explicitHoleRect ?? lastHoleRect
      encoder.setFragmentBytes(
        &holeRect,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 0
      )
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
      encoder.endEncoding()
      commandBuffer.present(drawable)
      commandBuffer.commit()

      frameCount += 1
      lastVideoTextureAvailable = true
      lastFlutterTextureAvailable = true
      lastCompositeSucceeded = true
      lastFailure = ""
      if frameCount == 1 || frameCount % 120 == 0 {
        NSLog(
          "VoidPlayer native compositor spike: composite frame=%d mode=%@ video=%dx%d flutter=%dx%d drawable=%dx%d",
          frameCount,
          outputMode,
          video.width,
          video.height,
          flutter.width,
          flutter.height,
          Int(metalLayer.drawableSize.width),
          Int(metalLayer.drawableSize.height)
        )
      }
    }
  }

  private func currentVideoMetalTexture() -> MTLTexture? {
    guard let retainedBuffer = videoTexture?.copyPixelBuffer() else {
      lastVideoTextureAvailable = false
      return nil
    }
    let pixelBuffer = retainedBuffer.takeRetainedValue()
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
      lastVideoTextureAvailable = false
      return nil
    }
    return texture
  }

  private func currentFlutterMetalTexture() -> MTLTexture? {
    guard let info = engine?.voidPlayerHDRCurrentFlutterSurfaceInfos().first,
          let texture = info["texture"] as? MTLTexture else {
      lastFlutterTextureAvailable = false
      return nil
    }
    updateFlutterAlphaMetrics(info: info)
    return texture
  }

  private func updateFlutterAlphaMetrics(info: [String: Any]) {
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
      NSLog("VoidPlayer native compositor spike: \(message)")
    }
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

      fragment float4 fs_main(
        VertexOut in [[stage_in]],
        texture2d<float> videoTexture [[texture(0)]],
        texture2d<float> flutterTexture [[texture(1)]],
        constant float4& holeRect [[buffer(0)]]
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
        float4 video = insideHole ? videoTexture.sample(s, videoUv) : float4(0.0);
        float4 flutter = flutterTexture.sample(s, uv);
        float alpha = clamp(flutter.a, 0.0, 1.0);
        float3 rgb = flutter.rgb + video.rgb * (1.0 - alpha);
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
