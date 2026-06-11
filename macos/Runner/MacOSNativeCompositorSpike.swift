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
  private let configuration: MacOSPresentationConfiguration
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
  private var lastVideoPixelFormat = "unknown"
  private var lastEDRVideoSampleCount = 0
  private var lastEDRVideoMaxRGBX1000 = 0
  private var lastEDRVideoPixelsOver1X1000 = 0
  private var lastVideoSRGBToLinearEnabled = false
  private var lastFlutterSRGBToLinearEnabled = false
  private var explicitHoleRect: SIMD4<Float>?
  private var lastHoleRect = SIMD4<Float>(0, 0, 0, 0)

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
    var result = configuration.diagnostics
    result.merge([
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
      "nativeCompositorVideoPixelFormat": lastVideoPixelFormat,
      "nativeCompositorEDRVideoSampleCount": lastEDRVideoSampleCount,
      "nativeCompositorEDRVideoMaxRGBX1000": lastEDRVideoMaxRGBX1000,
      "nativeCompositorEDRVideoPixelsOver1X1000": lastEDRVideoPixelsOver1X1000,
      "nativeCompositorVideoSRGBToLinearEnabled": lastVideoSRGBToLinearEnabled,
      "nativeCompositorFlutterSRGBToLinearEnabled": lastFlutterSRGBToLinearEnabled,
    ]) { _, next in next }
    return result
  }

  private func drawComposite() {
    autoreleasepool {
      guard let drawable = metalLayer.nextDrawable() else {
        recordFailure("no drawable")
        return
      }
      guard let video = currentVideoMetalTexture() else {
        isHidden = true
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
      var colorFlags = SIMD4<Float>(
        outputPixelFormat == .rgba16Float ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: video) ? 1.0 : 0.0,
        shouldConvertSRGBToLinear(texture: flutter) ? 1.0 : 0.0,
        0.0
      )
      encoder.setFragmentBytes(
        &colorFlags,
        length: MemoryLayout<SIMD4<Float>>.stride,
        index: 1
      )
      encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
      encoder.endEncoding()
      commandBuffer.present(drawable)
      commandBuffer.commit()

      isHidden = false
      frameCount += 1
      lastVideoTextureAvailable = true
      lastFlutterTextureAvailable = true
      lastCompositeSucceeded = true
      lastFailure = ""
      lastVideoSRGBToLinearEnabled = colorFlags.y > 0.5
      lastFlutterSRGBToLinearEnabled = colorFlags.z > 0.5
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

  private func currentVideoMetalTexture() -> MTLTexture? {
    guard let retainedBuffer = videoTexture?.copyPixelBuffer() else {
      lastVideoTextureAvailable = false
      return nil
    }
    let pixelBuffer = retainedBuffer.takeRetainedValue()
    let width = CVPixelBufferGetWidth(pixelBuffer)
    let height = CVPixelBufferGetHeight(pixelBuffer)
    updateVideoEDRMetrics(pixelBuffer: pixelBuffer)
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

  private func updateVideoEDRMetrics(pixelBuffer: CVPixelBuffer) {
    let format = CVPixelBufferGetPixelFormatType(pixelBuffer)
    lastVideoPixelFormat = Self.pixelFormatName(format)
    guard format == kCVPixelFormatType_64RGBAHalf else {
      lastEDRVideoSampleCount = 0
      lastEDRVideoMaxRGBX1000 = 0
      lastEDRVideoPixelsOver1X1000 = 0
      return
    }
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

      fragment float4 fs_main(
        VertexOut in [[stage_in]],
        texture2d<float> videoTexture [[texture(0)]],
        texture2d<float> flutterTexture [[texture(1)]],
        constant float4& holeRect [[buffer(0)]],
        constant float4& colorFlags [[buffer(1)]]
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
