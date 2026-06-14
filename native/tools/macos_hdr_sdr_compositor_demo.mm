#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 540;

NSString* shader_source() {
  return @R"metal(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float2 uv;
};

vertex VertexOut vertex_main(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[6] = {
      float2(-1.0, -1.0),
      float2( 1.0, -1.0),
      float2(-1.0,  1.0),
      float2( 1.0, -1.0),
      float2( 1.0,  1.0),
      float2(-1.0,  1.0),
  };
  const float2 p = positions[vertex_id];
  VertexOut out;
  out.position = float4(p, 0.0, 1.0);
  out.uv = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
  return out;
}

fragment float4 fragment_main(
    VertexOut in [[stage_in]],
    texture2d<float> hdr_texture [[texture(0)]],
    texture2d<float> sdr_texture [[texture(1)]],
    sampler texture_sampler [[sampler(0)]]) {
  const float4 hdr = hdr_texture.sample(texture_sampler, in.uv);
  const float4 sdr = sdr_texture.sample(texture_sampler, in.uv);
  const float3 composited = hdr.rgb * (1.0 - sdr.a) + sdr.rgb * sdr.a;
  return float4(composited, 1.0);
}
)metal";
}

struct Float4 {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
};

int fail(const std::string& message) {
  std::cerr << message << "\n";
  return 1;
}

std::vector<float> make_hdr_texture_data(int width, int height) {
  std::vector<float> pixels(static_cast<size_t>(width) *
                            static_cast<size_t>(height) * 4u);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(width - 1);
      const float v = static_cast<float>(y) / static_cast<float>(height - 1);
      float r = 0.02f + 0.12f * u;
      float g = 0.025f + 0.10f * v;
      float b = 0.035f + 0.08f * (1.0f - u);

      const bool in_hdr_ramp =
          x > width / 8 && x < width * 7 / 8 && y > height * 3 / 8 &&
          y < height * 5 / 8;
      if (in_hdr_ramp) {
        const float ramp = static_cast<float>(x - width / 8) /
                           static_cast<float>(width * 3 / 4);
        const float edr = 1.0f + 3.0f * std::clamp(ramp, 0.0f, 1.0f);
        r = edr;
        g = edr * (0.78f + 0.12f * std::sin(u * 12.0f));
        b = edr * 0.52f;
      }

      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(width) +
           static_cast<size_t>(x)) *
          4u;
      pixels[offset + 0] = r;
      pixels[offset + 1] = g;
      pixels[offset + 2] = b;
      pixels[offset + 3] = 1.0f;
    }
  }
  return pixels;
}

std::vector<uint8_t> make_sdr_texture_data(int width, int height) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) *
                              static_cast<size_t>(height) * 4u,
                              0);
  const int panel_left = width / 4;
  const int panel_top = height * 2 / 5;
  const int panel_right = width * 3 / 4;
  const int panel_bottom = height * 3 / 5;

  for (int y = panel_top; y < panel_bottom; ++y) {
    for (int x = panel_left; x < panel_right; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(width) +
           static_cast<size_t>(x)) *
          4u;
      pixels[offset + 0] = 230;
      pixels[offset + 1] = 236;
      pixels[offset + 2] = 244;
      pixels[offset + 3] = 154;
    }
  }

  for (int y = panel_top + 18; y < panel_top + 34; ++y) {
    for (int x = panel_left + 28; x < panel_right - 28; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(width) +
           static_cast<size_t>(x)) *
          4u;
      pixels[offset + 0] = 36;
      pixels[offset + 1] = 92;
      pixels[offset + 2] = 255;
      pixels[offset + 3] = 255;
    }
  }

  for (int button = 0; button < 4; ++button) {
    const int left = panel_left + 36 + button * 82;
    const int right = left + 44;
    const int top = panel_bottom - 62;
    const int bottom = top + 30;
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        const size_t offset =
            (static_cast<size_t>(y) * static_cast<size_t>(width) +
             static_cast<size_t>(x)) *
            4u;
        pixels[offset + 0] = button == 0 ? 255 : 42;
        pixels[offset + 1] = button == 1 ? 255 : 42;
        pixels[offset + 2] = button == 2 ? 255 : 42;
        pixels[offset + 3] = 242;
      }
    }
  }
  return pixels;
}

id<MTLTexture> make_float_texture(id<MTLDevice> device,
                                  int width,
                                  int height,
                                  const std::vector<float>& pixels) {
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    return nil;
  }
  [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:static_cast<NSUInteger>(width) * 4u * sizeof(float)];
  return texture;
}

id<MTLTexture> make_sdr_texture(id<MTLDevice> device,
                                int width,
                                int height,
                                const std::vector<uint8_t>& pixels) {
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    return nil;
  }
  [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
               withBytes:pixels.data()
             bytesPerRow:static_cast<NSUInteger>(width) * 4u];
  return texture;
}

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                         MTLPixelFormat pixel_format) {
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:shader_source()
                                                options:nil
                                                  error:&error];
  if (!library) {
    std::cerr << "failed to compile Metal shader: "
              << (error ? [[error localizedDescription] UTF8String] : "unknown")
              << "\n";
    return nil;
  }
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
  descriptor.colorAttachments[0].pixelFormat = pixel_format;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!pipeline) {
    std::cerr << "failed to create Metal pipeline: "
              << (error ? [[error localizedDescription] UTF8String] : "unknown")
              << "\n";
  }
  return pipeline;
}

id<MTLSamplerState> make_sampler(id<MTLDevice> device) {
  MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.minFilter = MTLSamplerMinMagFilterLinear;
  descriptor.magFilter = MTLSamplerMinMagFilterLinear;
  descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  return [device newSamplerStateWithDescriptor:descriptor];
}

void encode_composite(id<MTLCommandBuffer> command_buffer,
                      id<MTLRenderPipelineState> pipeline,
                      id<MTLSamplerState> sampler,
                      id<MTLTexture> hdr_texture,
                      id<MTLTexture> sdr_texture,
                      MTLRenderPassDescriptor* pass_descriptor) {
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:pass_descriptor];
  [encoder setRenderPipelineState:pipeline];
  [encoder setFragmentTexture:hdr_texture atIndex:0];
  [encoder setFragmentTexture:sdr_texture atIndex:1];
  [encoder setFragmentSamplerState:sampler atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
  [encoder endEncoding];
}

Float4 read_float4(const std::vector<float>& pixels, int width, int x, int y) {
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(width) +
       static_cast<size_t>(x)) *
      4u;
  return {pixels[offset + 0],
          pixels[offset + 1],
          pixels[offset + 2],
          pixels[offset + 3]};
}

int run_headless() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      return fail("Metal is unavailable");
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLRenderPipelineState> pipeline =
        make_pipeline(device, MTLPixelFormatRGBA32Float);
    id<MTLSamplerState> sampler = make_sampler(device);
    id<MTLTexture> hdr_texture =
        make_float_texture(device, kWidth, kHeight, make_hdr_texture_data(kWidth, kHeight));
    id<MTLTexture> sdr_texture =
        make_sdr_texture(device, kWidth, kHeight, make_sdr_texture_data(kWidth, kHeight));
    if (!queue || !pipeline || !sampler || !hdr_texture || !sdr_texture) {
      return fail("failed to initialize headless compositor resources");
    }

    MTLTextureDescriptor* output_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:kWidth
                                                          height:kHeight
                                                       mipmapped:NO];
    output_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    output_descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> output = [device newTextureWithDescriptor:output_descriptor];
    if (!output) {
      return fail("failed to create headless output texture");
    }

    MTLRenderPassDescriptor* pass_descriptor =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass_descriptor.colorAttachments[0].texture = output;
    pass_descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass_descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    encode_composite(command_buffer,
                     pipeline,
                     sampler,
                     hdr_texture,
                     sdr_texture,
                     pass_descriptor);
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    if ([command_buffer status] != MTLCommandBufferStatusCompleted) {
      return fail("headless compositor command buffer did not complete");
    }

    std::vector<float> output_pixels(static_cast<size_t>(kWidth) *
                                     static_cast<size_t>(kHeight) * 4u);
    [output getBytes:output_pixels.data()
         bytesPerRow:static_cast<NSUInteger>(kWidth) * 4u * sizeof(float)
          fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
         mipmapLevel:0];

    const Float4 hdr_sample = read_float4(output_pixels, kWidth, kWidth * 13 / 16, kHeight / 2);
    const Float4 overlap_sample = read_float4(output_pixels, kWidth, kWidth * 5 / 8, kHeight / 2);
    const Float4 dark_sample = read_float4(output_pixels, kWidth, kWidth / 20, kHeight / 2);
    if (hdr_sample.r <= 2.0f || hdr_sample.g <= 1.4f) {
      std::cerr << "HDR sample did not preserve extended range: r=" << hdr_sample.r
                << " g=" << hdr_sample.g << "\n";
      return 1;
    }
    if (overlap_sample.r <= 1.0f || overlap_sample.r >= hdr_sample.r ||
        overlap_sample.g <= 1.0f || overlap_sample.g >= hdr_sample.g) {
      std::cerr << "SDR overlay did not blend over HDR as expected: overlap=("
                << overlap_sample.r << ", " << overlap_sample.g << ", "
                << overlap_sample.b << "), hdr=(" << hdr_sample.r << ", "
                << hdr_sample.g << ", " << hdr_sample.b << ")\n";
      return 1;
    }
    if (dark_sample.r >= 0.3f || dark_sample.g >= 0.3f || dark_sample.b >= 0.3f) {
      std::cerr << "dark HDR background sample was unexpectedly bright: r="
                << dark_sample.r << " g=" << dark_sample.g
                << " b=" << dark_sample.b << "\n";
      return 1;
    }

    std::cout << "macOS HDR/SDR native compositor headless passed; "
              << "hdr_sample=(" << hdr_sample.r << ", " << hdr_sample.g << ", "
              << hdr_sample.b << "), overlap_sample=(" << overlap_sample.r
              << ", " << overlap_sample.g << ", " << overlap_sample.b << ")\n";
    return 0;
  }
}

}  // namespace

@interface DemoRenderer : NSObject <MTKViewDelegate> {
 @private
  __weak MTKView* view_;
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLRenderPipelineState> pipeline_;
  id<MTLSamplerState> sampler_;
  id<MTLTexture> hdr_texture_;
  id<MTLTexture> sdr_texture_;
}
- (instancetype)initWithMTKView:(MTKView*)view;
- (BOOL)ready;
@end

@implementation DemoRenderer

- (instancetype)initWithMTKView:(MTKView*)view {
  self = [super init];
  if (self) {
    view_ = view;
    device_ = view.device;
    queue_ = [device_ newCommandQueue];
    pipeline_ = make_pipeline(device_, view.colorPixelFormat);
    sampler_ = make_sampler(device_);
    hdr_texture_ =
        make_float_texture(device_, kWidth, kHeight, make_hdr_texture_data(kWidth, kHeight));
    sdr_texture_ =
        make_sdr_texture(device_, kWidth, kHeight, make_sdr_texture_data(kWidth, kHeight));
  }
  return self;
}

- (BOOL)ready {
  return queue_ && pipeline_ && sampler_ && hdr_texture_ && sdr_texture_;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
  (void)view;
  (void)size;
}

- (void)drawInMTKView:(MTKView*)view {
  if (![self ready] || !view.currentDrawable || !view.currentRenderPassDescriptor) {
    return;
  }
  id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
  encode_composite(command_buffer,
                   pipeline_,
                   sampler_,
                   hdr_texture_,
                   sdr_texture_,
                   view.currentRenderPassDescriptor);
  [command_buffer presentDrawable:view.currentDrawable];
  [command_buffer commit];
}

@end

namespace {

int run_window() {
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      return fail("Metal is unavailable");
    }

    const NSRect frame = NSMakeRect(0, 0, kWidth, kHeight);
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled |
                                              NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    [window setTitle:@"VoidPlayer HDR/SDR Native Compositor Demo"];
    [window center];

    MTKView* view = [[MTKView alloc] initWithFrame:frame device:device];
    view.colorPixelFormat = MTLPixelFormatRGBA16Float;
    view.framebufferOnly = NO;
    view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    view.preferredFramesPerSecond = 60;
    view.enableSetNeedsDisplay = NO;
    view.paused = NO;

    CAMetalLayer* layer = static_cast<CAMetalLayer*>(view.layer);
    layer.pixelFormat = MTLPixelFormatRGBA16Float;
    layer.framebufferOnly = NO;
    CGColorSpaceRef color_space =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
    layer.colorspace = color_space;
    if (color_space) {
      CFRelease(color_space);
    }
    if ([layer respondsToSelector:@selector(setWantsExtendedDynamicRangeContent:)]) {
      layer.wantsExtendedDynamicRangeContent = YES;
    }

    DemoRenderer* renderer = [[DemoRenderer alloc] initWithMTKView:view];
    if (![renderer ready]) {
      return fail("failed to initialize window compositor resources");
    }
    view.delegate = renderer;
    objc_setAssociatedObject(view, "VoidPlayerHdrSdrDemoRenderer", renderer,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    [window setContentView:view];
    [window makeKeyAndOrderFront:nil];
    [app activateIgnoringOtherApps:YES];
    [app run];
    return 0;
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool headless = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--headless") {
      headless = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: macos_hdr_sdr_compositor_demo [--headless]\n";
      return 0;
    } else {
      return fail("unknown argument: " + arg);
    }
  }
  return headless ? run_headless() : run_window();
}
