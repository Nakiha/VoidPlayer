import Metal

enum MacOSNativeCompositorPipelineFactory {
  static func make(
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

        float2 displayOffset = float2(
          valueAt(sourceDisplayOffsetX, sourceSlot),
          valueAt(sourceDisplayOffsetY, sourceSlot));
        float2 invDisplaySize = float2(
          valueAt(sourceInvDisplaySizeX, sourceSlot),
          valueAt(sourceInvDisplaySizeY, sourceSlot));
        float2 viewOffsetUv = float2(
          valueAt(sourceViewOffsetUvX, sourceSlot),
          valueAt(sourceViewOffsetUvY, sourceSlot));
        float2 sourceUv = (localUv - displayOffset) * invDisplaySize - viewOffsetUv;
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
        if (colorFlags.w > 0.5) {
          return video;
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
