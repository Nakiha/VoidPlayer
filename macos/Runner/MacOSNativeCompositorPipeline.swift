import Metal

enum MacOSNativeCompositorPipelineFactory {
  static func make(
    device: MTLDevice,
    outputPixelFormat: MTLPixelFormat
  ) -> (
    video: MTLRenderPipelineState,
    flutter: MTLRenderPipelineState
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
          srgbChannelToLinear(color.b));
      }

      float3 premultipliedSRGBToLinear(float3 color, float alpha) {
        if (alpha <= 0.0001) {
          return float3(0.0);
        }
        return srgbToLinear(clamp(color / alpha, 0.0, 1.0)) * alpha;
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

      fragment float4 fs_video(
        VertexOut in [[stage_in]],
        texture2d<float> videoTexture [[texture(0)]],
        constant float4& viewportRect [[buffer(0)]],
        constant float4& colorFlags [[buffer(1)]],
        constant float4& backgroundColor [[buffer(2)]]) {
        constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);
        float2 uv = clamp(in.uv, 0.0, 1.0);
        bool validRect = viewportRect.z > viewportRect.x && viewportRect.w > viewportRect.y;
        bool inside = validRect &&
          uv.x >= viewportRect.x && uv.x <= viewportRect.z &&
          uv.y >= viewportRect.y && uv.y <= viewportRect.w;
        if (!inside) {
          return mapSDRUIToOutput(backgroundColor, colorFlags.x > 0.5);
        }
        float2 videoUv = float2(
          (uv.x - viewportRect.x) / max(0.0001, viewportRect.z - viewportRect.x),
          (uv.y - viewportRect.y) / max(0.0001, viewportRect.w - viewportRect.y));
        float4 video = videoTexture.sample(linearSampler, videoUv);
        return colorFlags.y > 0.5
          ? float4(srgbToLinear(video.rgb), video.a)
          : video;
      }

      fragment float4 fs_flutter(
        VertexOut in [[stage_in]],
        texture2d<float> flutterTexture [[texture(0)]],
        constant float4& colorFlags [[buffer(0)]]) {
        constexpr sampler linearSampler(address::clamp_to_edge, filter::linear);
        float4 flutter = flutterTexture.sample(linearSampler, clamp(in.uv, 0.0, 1.0));
        float alpha = clamp(flutter.a, 0.0, 1.0);
        float3 rgb = colorFlags.z > 0.5
          ? premultipliedSRGBToLinear(flutter.rgb, alpha)
          : flutter.rgb;
        return float4(rgb, alpha);
      }
      """
    guard let library = try? device.makeLibrary(source: source, options: nil),
          let vertex = library.makeFunction(name: "vp_main"),
          let videoFragment = library.makeFunction(name: "fs_video"),
          let flutterFragment = library.makeFunction(name: "fs_flutter") else {
      return nil
    }

    func descriptor(fragment: MTLFunction, blending: Bool) -> MTLRenderPipelineDescriptor {
      let descriptor = MTLRenderPipelineDescriptor()
      descriptor.vertexFunction = vertex
      descriptor.fragmentFunction = fragment
      descriptor.colorAttachments[0].pixelFormat = outputPixelFormat
      if blending {
        let attachment = descriptor.colorAttachments[0]!
        attachment.isBlendingEnabled = true
        attachment.rgbBlendOperation = .add
        attachment.alphaBlendOperation = .add
        attachment.sourceRGBBlendFactor = .one
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
      }
      return descriptor
    }

    guard let video = try? device.makeRenderPipelineState(
      descriptor: descriptor(fragment: videoFragment, blending: false)
    ), let flutter = try? device.makeRenderPipelineState(
      descriptor: descriptor(fragment: flutterFragment, blending: true)
    ) else {
      return nil
    }
    return (video: video, flutter: flutter)
  }
}
